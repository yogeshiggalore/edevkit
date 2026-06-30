/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * nrf53_dp.c — DP layer primitives + meta-helpers (full_wake, sticky_clear,
 * power_up). Sits on top of Zephyr's SWDP driver (drivers/dp/swdp_*.c).
 *
 * Also owns:
 *   - the bound SWDP device pointer (module-internal singleton)
 *   - the shared transfer helper (nrf53__transfer) used by nrf53_ap.c
 *
 * Algorithm reference: docs/NRF5340_ALGORITHMS.md §2.
 */

#include "nrf53.h"
#include "nrf53_priv.h"

#include <zephyr/drivers/swdp.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(nrf53_dp, CONFIG_EDEV_NRF53_OPS_LOG_LEVEL);

/* ------------------------------------------------------------------ */
/* Module state — single SWDP device (set once at boot)               */
/* ------------------------------------------------------------------ */
static const struct device *s_swdp;

/* Idle cycles inserted between DP/AP transactions. ADIv5 §B4 mandates
 * ≥8 idle cycles after the data phase of a write or after a turnaround.
 * J-Link uses 0 for fast probes; pyocd uses 8. Going with 8 for safety
 * — measured cost is one byte of extra clock time per transfer (~2 µs
 * at 4 MHz SWD). */
#define DP_IDLE_CYCLES   8U

/* SWD WAIT-ACK retry budget. ADIv5 says retry up to ~8 times in-protocol
 * before bubbling FAULT up. Each retry takes ~1 µs at 25 MHz SWD. */
#define DP_WAIT_RETRIES  8U

/* Dormant-wake selection alert (148 bits) — bytes verbatim from the
 * Python reference. 8-bit preamble (0xFF) + 128-bit selection alert
 * + 4 zeros + 8-bit SWD activation. ADIv6 §B5 calls this the
 * "selection alert" sequence. */
static const uint8_t k_swd_alert_bytes[] = {
	0xFF,                                            /* 8 ones (preamble) */
	0x92, 0xF3, 0x09, 0x62, 0x95, 0x2D, 0x85, 0x86,  /* 128-bit selection */
	0xE9, 0xAF, 0xDD, 0xE3, 0xA2, 0x0E, 0xBC, 0x19,  /* alert, LSB-first */
	0x00,                                            /* 4 zeros + 4 unused */
	0x1A,                                            /* 8-bit SWD activation */
};
#define SWD_ALERT_BITS   (8U + 128U + 4U + 8U)   /* = 148 */

/* JTAG→SWD switch sequence (16 bits, 0xE79E LSB-first) */
static const uint8_t k_jtag_to_swd[] = { 0x9E, 0xE7 };

/* SWD line reset (56 bits, all ones) */
static const uint8_t k_swd_line_reset[] = {
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};
#define SWD_LINE_RESET_BITS   56U

/* ------------------------------------------------------------------ */
/* Internal accessor + transfer helper (exposed via nrf53_priv.h)     */
/* ------------------------------------------------------------------ */
const struct device *nrf53__swdp(void)
{
	return s_swdp;
}

static nrf53_status_t map_ack(uint8_t resp)
{
	/* SWDP_TRANSFER_ERROR (bit 3) indicates parity / framing. Mask the
	 * ACK to bits 0:2 — bits 3+ of resp can carry junk from some probe
	 * firmwares (see project_uh_dapv2_ack_framing_fix). */
	if (resp & SWDP_TRANSFER_ERROR) {
		return NRF53_PROTO;
	}
	switch (resp & 0x07U) {
	case SWDP_ACK_OK:    return NRF53_OK;
	case SWDP_ACK_WAIT:  return NRF53_WAIT;
	case SWDP_ACK_FAULT: return NRF53_FAULT;
	case 0x07U:          return NRF53_NO_ACK;
	default:             return NRF53_PROTO;
	}
}

nrf53_status_t nrf53__transfer(uint8_t req, uint32_t *data)
{
	if (s_swdp == NULL) {
		return NRF53_NO_DEV;
	}

	uint8_t resp = 0;
	for (unsigned int i = 0; i < DP_WAIT_RETRIES; i++) {
		int rc = swdp_transfer(s_swdp, req, data, DP_IDLE_CYCLES, &resp);
		if (rc != 0) {
			LOG_DBG("swdp_transfer rc=%d req=0x%02x", rc, req);
			return NRF53_PROTO;
		}
		nrf53_status_t st = map_ack(resp);
		if (st != NRF53_WAIT) {
			return st;
		}
		/* WAIT — let the target catch up, then retry */
	}
	LOG_WRN("WAIT-ACK retry budget exhausted (req=0x%02x)", req);
	return NRF53_WAIT;
}

/* ------------------------------------------------------------------ */
/* Status-code → string                                               */
/* ------------------------------------------------------------------ */
const char *nrf53_status_str(nrf53_status_t s)
{
	switch (s) {
	case NRF53_OK:        return "OK";
	case NRF53_WAIT:      return "WAIT";
	case NRF53_FAULT:     return "FAULT";
	case NRF53_NO_ACK:    return "NO_ACK";
	case NRF53_PROTO:     return "PROTO";
	case NRF53_TIMEOUT:   return "TIMEOUT";
	case NRF53_ARGS:      return "ARGS";
	case NRF53_NO_DEV:    return "NO_DEV";
	case NRF53_STUB_FAIL: return "STUB_FAIL";
	}
	return "?";
}

/* ------------------------------------------------------------------ */
/* Initialization                                                     */
/* ------------------------------------------------------------------ */
nrf53_status_t nrf53_bind_swdp(const struct device *swdp_dev)
{
	if (swdp_dev == NULL || !device_is_ready(swdp_dev)) {
		LOG_ERR("SWDP device not ready");
		return NRF53_NO_DEV;
	}
	s_swdp = swdp_dev;
	LOG_INF("bound SWDP device '%s'", swdp_dev->name);
	return NRF53_OK;
}

/* ------------------------------------------------------------------ */
/* DP read/write                                                      */
/* ------------------------------------------------------------------ */
nrf53_status_t nrf53_dp_read(uint8_t reg_offset, uint32_t *out)
{
	if (out == NULL || (reg_offset & 0x03U) != 0 || reg_offset > 0x0CU) {
		return NRF53_ARGS;
	}
	/* SWDP request: APnDP=0 (DP) | RnW=1 (Read) | A[3:2] from offset */
	uint8_t req = SWDP_REQUEST_RnW;
	if (reg_offset & 0x04U) { req |= SWDP_REQUEST_A2; }
	if (reg_offset & 0x08U) { req |= SWDP_REQUEST_A3; }

	uint32_t data = 0;
	nrf53_status_t st = nrf53__transfer(req, &data);
	if (st == NRF53_OK) {
		*out = data;
	}
	return st;
}

nrf53_status_t nrf53_dp_write(uint8_t reg_offset, uint32_t val)
{
	if ((reg_offset & 0x03U) != 0 || reg_offset > 0x0CU) {
		return NRF53_ARGS;
	}
	uint8_t req = 0;   /* APnDP=0, RnW=0 (Write) */
	if (reg_offset & 0x04U) { req |= SWDP_REQUEST_A2; }
	if (reg_offset & 0x08U) { req |= SWDP_REQUEST_A3; }

	uint32_t data = val;
	return nrf53__transfer(req, &data);
}

/* ------------------------------------------------------------------ */
/* Meta-helpers                                                       */
/* ------------------------------------------------------------------ */
nrf53_status_t nrf53_dp_sticky_clear(void)
{
	return nrf53_dp_write(NRF53_DP_ABORT, NRF53_ABORT_STICKY_ALL);
}

nrf53_status_t nrf53_dp_power_up(uint32_t timeout_ms)
{
	nrf53_status_t st = nrf53_dp_write(NRF53_DP_CTRL_STAT,
					   NRF53_CTRL_PWRUP_REQ);
	if (st != NRF53_OK) {
		return st;
	}

	const int64_t deadline = k_uptime_get() + (int64_t)timeout_ms;
	while (k_uptime_get() < deadline) {
		uint32_t cs = 0;
		st = nrf53_dp_read(NRF53_DP_CTRL_STAT, &cs);
		if (st != NRF53_OK) {
			return st;
		}
		if ((cs & NRF53_CTRL_PWRUP_ACK) == NRF53_CTRL_PWRUP_ACK) {
			return NRF53_OK;
		}
		k_msleep(1);
	}
	LOG_WRN("DP power-up ACK timeout");
	return NRF53_TIMEOUT;
}

nrf53_status_t nrf53_dp_full_wake(uint32_t *dpidr_out)
{
	if (s_swdp == NULL) {
		return NRF53_NO_DEV;
	}

	int rc;

	rc = swdp_output_sequence(s_swdp, SWD_LINE_RESET_BITS, k_swd_line_reset);
	if (rc) { LOG_ERR("line reset rc=%d", rc); return NRF53_PROTO; }

	rc = swdp_output_sequence(s_swdp, 16, k_jtag_to_swd);
	if (rc) { LOG_ERR("jtag→swd rc=%d", rc); return NRF53_PROTO; }

	rc = swdp_output_sequence(s_swdp, SWD_ALERT_BITS, k_swd_alert_bytes);
	if (rc) { LOG_ERR("dormant alert rc=%d", rc); return NRF53_PROTO; }

	rc = swdp_output_sequence(s_swdp, SWD_LINE_RESET_BITS, k_swd_line_reset);
	if (rc) { LOG_ERR("line reset 2 rc=%d", rc); return NRF53_PROTO; }

	/* ADIv5 §B4.2.2: first transaction after a line reset MUST be a
	 * DPIDR read, or the DP wedges and returns FAULT on every
	 * subsequent transaction. */
	uint32_t dpidr = 0;
	nrf53_status_t st = nrf53_dp_read(NRF53_DP_DPIDR, &dpidr);
	if (st != NRF53_OK) {
		LOG_ERR("DPIDR read failed: %s", nrf53_status_str(st));
		return st;
	}
	LOG_INF("DPIDR=0x%08x", dpidr);
	if (dpidr_out) {
		*dpidr_out = dpidr;
	}

	st = nrf53_dp_sticky_clear();
	if (st != NRF53_OK) {
		LOG_WRN("sticky_clear after wake: %s", nrf53_status_str(st));
		return st;
	}

	st = nrf53_dp_power_up(100);
	if (st != NRF53_OK) {
		LOG_WRN("dp_power_up after wake: %s", nrf53_status_str(st));
	}
	return st;
}
