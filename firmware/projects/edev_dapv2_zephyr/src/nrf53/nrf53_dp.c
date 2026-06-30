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

/* Dormant-wake selection alert (148 bits, ADIv6 §B5 "selection alert").
 *
 * Bit-stream (LSB-first), all on a contiguous SWDIO output sequence:
 *   bits   0.. 7  = 0xFF      preamble (8 high)
 *   bits   8..135 = alert     128-bit JEDEC selection alert (16 bytes)
 *   bits 136..139 = 0000      4-bit zero filler
 *   bits 140..147 = 0x1A      8-bit SWD activation
 *
 * Because the activation byte (0x1A) is bit-aligned at offset 140 — NOT
 * a byte boundary — it straddles bytes 17 and 18:
 *   byte 17 (bits 136..143) = (lo 4 bits = 0 filler) | (hi 4 bits = 0x1A lo nibble = 0xA) → 0xA0
 *   byte 18 (bits 144..151) = (lo 4 bits = 0x1A hi nibble = 0x1)                          → 0x01
 *
 * NOTE: the Python reference (dap_ble_bridge::_SWD_ALERT_BYTES) had
 * byte 17 = 0x00 and byte 18 = 0x1A, treating them as if byte 17 was
 * "4 zeros + 4 unused" and byte 18 was the full activation. That layout
 * misaligns the activation: when swdp_output_sequence sends 148
 * contiguous LSB-first bits, bits 140..147 come out as 0x00 (nibble of
 * byte 17 = 0) | 0xA (lo nibble of byte 18) instead of 0x1A. Carrying
 * that bug forward worked on nRF5340 (DPv2 silicon resyncs on the
 * post-alert line reset) but failed on nRF52840 (DPv1). Fixed here.
 */
static const uint8_t k_swd_alert_bytes[] = {
	0xFF,                                            /* 8 ones (preamble) */
	0x92, 0xF3, 0x09, 0x62, 0x95, 0x2D, 0x85, 0x86,  /* 128-bit selection */
	0xE9, 0xAF, 0xDD, 0xE3, 0xA2, 0x0E, 0xBC, 0x19,  /* alert, LSB-first */
	0xA0,                                            /* 4 zeros | 0x1A lo-nibble */
	0x01,                                            /* 0x1A hi-nibble (+ 4 unsent bits) */
};
#define SWD_ALERT_BITS   (8U + 128U + 4U + 8U)   /* = 148 */

/* JTAG→SWD switch sequence (16 bits, 0xE79E LSB-first) */
static const uint8_t k_jtag_to_swd[] = { 0x9E, 0xE7 };

/* SWD line reset (56 bits, all ones) */
static const uint8_t k_swd_line_reset[] = {
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};
#define SWD_LINE_RESET_BITS   56U

/* 8 idle cycles (SWDIO low) — sent after a line reset, before the first
 * SWD transaction. ADIv5 §B4 doesn't strictly mandate this, but
 * empirically the standard CMSIS-DAP DAP_SWJ_Sequence pattern always
 * ends with 8 idle cycles before the first transaction. Skipping them
 * causes NO_ACK on the first DPIDR read against nRF52840 (DPv1). */
static const uint8_t k_swd_idle_byte[] = { 0x00 };
#define SWD_IDLE_BITS         8U

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

/* Issue a JTAG→SWD line reset (no dormant-wake alert). Always safe;
 * required by ADIv5 §B4.2.2 before the first DPIDR read. */
static nrf53_status_t do_simple_line_reset(void)
{
	int rc;
	rc = swdp_output_sequence(s_swdp, SWD_LINE_RESET_BITS, k_swd_line_reset);
	if (rc) { LOG_ERR("line reset rc=%d", rc); return NRF53_PROTO; }
	rc = swdp_output_sequence(s_swdp, 16, k_jtag_to_swd);
	if (rc) { LOG_ERR("jtag→swd rc=%d", rc); return NRF53_PROTO; }
	rc = swdp_output_sequence(s_swdp, SWD_LINE_RESET_BITS, k_swd_line_reset);
	if (rc) { LOG_ERR("line reset 2 rc=%d", rc); return NRF53_PROTO; }
	rc = swdp_output_sequence(s_swdp, SWD_IDLE_BITS, k_swd_idle_byte);
	if (rc) { LOG_ERR("post-reset idle rc=%d", rc); return NRF53_PROTO; }
	return NRF53_OK;
}

/* Issue the full dormant-wake → SWD sequence. Only needed for ADIv6
 * (DPv2+) chips that boot into dormant — Cortex-M33+ class. DPv1 chips
 * (Cortex-M0/M4 like nRF52840) don't have dormant mode and can be
 * wedged by the alert; this is only called as a fallback. */
static nrf53_status_t do_dormant_wake_reset(void)
{
	int rc;
	rc = swdp_output_sequence(s_swdp, SWD_LINE_RESET_BITS, k_swd_line_reset);
	if (rc) { LOG_ERR("line reset rc=%d", rc); return NRF53_PROTO; }
	rc = swdp_output_sequence(s_swdp, 16, k_jtag_to_swd);
	if (rc) { LOG_ERR("jtag→swd rc=%d", rc); return NRF53_PROTO; }
	rc = swdp_output_sequence(s_swdp, SWD_ALERT_BITS, k_swd_alert_bytes);
	if (rc) { LOG_ERR("dormant alert rc=%d", rc); return NRF53_PROTO; }
	rc = swdp_output_sequence(s_swdp, SWD_LINE_RESET_BITS, k_swd_line_reset);
	if (rc) { LOG_ERR("line reset 2 rc=%d", rc); return NRF53_PROTO; }
	rc = swdp_output_sequence(s_swdp, SWD_IDLE_BITS, k_swd_idle_byte);
	if (rc) { LOG_ERR("post-reset idle rc=%d", rc); return NRF53_PROTO; }
	return NRF53_OK;
}

nrf53_status_t nrf53_dp_full_wake(uint32_t *dpidr_out)
{
	if (s_swdp == NULL) {
		return NRF53_NO_DEV;
	}

	/* Ensure SWDIO/SWCLK are in active drive (the SWDP driver's
	 * sw_port_on configures pin direction + level). The CMSIS-DAP
	 * dispatcher calls this on DAP_Connect, but our vendor commands
	 * can run standalone without a prior DAP_Connect — so do it
	 * ourselves. Idempotent — safe to call when host already did
	 * DAP_Connect. Default clock is 1 MHz (SWDP_DEFAULT_SWCLK_FREQUENCY)
	 * which we leave alone so we don't disturb the host's chosen
	 * speed if it did set one. */
	(void)swdp_port_on(s_swdp);

	/* Adaptive sequence: try simple line reset first — works for DPv1
	 * (nRF52, Cortex-M4) and any DPv2+ chip already in active SWD.
	 * Fall back to dormant-wake alert only if the simple path fails
	 * (chip is actually in dormant mode, e.g. cold-boot Cortex-M33).
	 *
	 * Background: nRF52840 (DPv1) is wedged by an unnecessary dormant
	 * alert. nRF5340 (DPv2) resyncs after the alert + line reset so
	 * the wrong order is recoverable there but not on DPv1. */

	nrf53_status_t st = do_simple_line_reset();
	if (st != NRF53_OK) {
		return st;
	}

	/* ADIv5 §B4.2.2: first transaction after a line reset MUST be a
	 * DPIDR read, or the DP wedges and returns FAULT on every
	 * subsequent transaction. */
	uint32_t dpidr = 0;
	st = nrf53_dp_read(NRF53_DP_DPIDR, &dpidr);
	if (st != NRF53_OK) {
		LOG_INF("DPIDR after simple reset failed (%s) — trying dormant wake",
			nrf53_status_str(st));

		/* Clear sticky from the failed attempt before retry — ABORT
		 * writes work regardless of DP power state. */
		(void)nrf53_dp_sticky_clear();

		st = do_dormant_wake_reset();
		if (st != NRF53_OK) {
			return st;
		}
		st = nrf53_dp_read(NRF53_DP_DPIDR, &dpidr);
		if (st != NRF53_OK) {
			LOG_ERR("DPIDR read failed after dormant alert: %s",
				nrf53_status_str(st));
			return st;
		}
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
