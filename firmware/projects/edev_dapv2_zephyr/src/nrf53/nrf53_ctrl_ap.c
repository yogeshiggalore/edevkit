/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * nrf53_ctrl_ap.c — Nordic CTRL-AP primitives + the high-level
 * nrf53_erase_all() entry point.
 *
 * Algorithm reference: docs/NRF5340_ALGORITHMS.md §3.
 *
 * Nordic CTRL-AP is a non-standard AP at one of the higher AP slots
 * (typically AP=1 on nRF52, AP=2 + AP=3 on nRF5340). Its IDR carries
 * a known Nordic-vendor value so we can discover the slot by scanning.
 * CTRL-AP registers live in bank 0:
 *   0x00 RESET            — write 1 / 0 to pulse chip reset
 *   0x04 ERASEALL         — write 1 to start mass-erase
 *   0x08 ERASEALLSTATUS   — 0 = idle, 1 = busy
 *   0x10 APPROTECTSTATUS  — read APPROTECT bits
 */

#include "nrf53.h"
#include "nrf53_priv.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(nrf53_ctrl_ap, CONFIG_EDEV_NRF53_OPS_LOG_LEVEL);

/* Max AP slots to walk during the IDR scan. ADIv5 reserves 256 AP slots
 * but no real chip has more than a handful — 8 covers everything we
 * care about (nRF52 = AP1; nRF5340 = AP0,1,2,3; nRF91 similar). */
#define CTRL_AP_SCAN_MAX_INDEX   8U

/* Sleep between CTRL-AP RESET pulse edges. The Python reference uses
 * 20 ms; ADIv5 says ≥2 ms is enough but Nordic's NVMC needs a few
 * ms to internally re-arm after reset deassertion. */
#define CTRL_AP_RESET_PULSE_MS   20U

/* ERASEALLSTATUS poll cadence. 20 ms matches the Python reference. */
#define ERASEALL_POLL_INTERVAL_MS  20U

/* ------------------------------------------------------------------ */
nrf53_status_t nrf53_ctrl_ap_scan(uint8_t *ap_indices, size_t max, size_t *count)
{
	if (ap_indices == NULL || count == NULL || max == 0) {
		return NRF53_ARGS;
	}

	size_t found = 0;
	for (uint8_t ap = 0; ap < CTRL_AP_SCAN_MAX_INDEX; ap++) {
		uint32_t idr = 0;
		/* AP_IDR lives in bank 0xF — nrf53_ap_read picks the bank
		 * automatically from the reg_offset's high nibble. */
		nrf53_status_t st = nrf53_ap_read(ap, NRF53_AP_IDR, &idr);
		if (st != NRF53_OK) {
			/* An empty AP slot returns NO_ACK or FAULT — ignore
			 * and keep walking. Only a hard PROTO error breaks
			 * the scan loop. */
			if (st == NRF53_PROTO || st == NRF53_NO_DEV) {
				return st;
			}
			/* Clear sticky bits accumulated from probing an
			 * empty slot so the next AP read starts clean. */
			(void)nrf53_dp_sticky_clear();
			continue;
		}
		if (idr == NRF53_IDR_CTRL_AP_A || idr == NRF53_IDR_CTRL_AP_B) {
			LOG_INF("CTRL-AP found at AP=%u (IDR=0x%08x)", ap, idr);
			if (found < max) {
				ap_indices[found] = ap;
			}
			found++;
		} else if (idr != 0) {
			LOG_DBG("AP=%u IDR=0x%08x (not CTRL-AP)", ap, idr);
		}
	}

	*count = found;
	return NRF53_OK;
}

/* ------------------------------------------------------------------ */
nrf53_status_t nrf53_ctrl_ap_reset_pulse(uint8_t ap_index)
{
	nrf53_status_t st;

	st = nrf53_ap_write(ap_index, NRF53_CTRL_AP_RESET, 1U);
	if (st != NRF53_OK) {
		LOG_WRN("CTRL-AP[%u] RESET=1 failed: %s", ap_index, nrf53_status_str(st));
		return st;
	}
	k_msleep(CTRL_AP_RESET_PULSE_MS);

	st = nrf53_ap_write(ap_index, NRF53_CTRL_AP_RESET, 0U);
	if (st != NRF53_OK) {
		LOG_WRN("CTRL-AP[%u] RESET=0 failed: %s", ap_index, nrf53_status_str(st));
		return st;
	}
	k_msleep(CTRL_AP_RESET_PULSE_MS);

	return NRF53_OK;
}

/* ------------------------------------------------------------------ */
nrf53_status_t nrf53_ctrl_ap_eraseall(uint8_t ap_index, uint32_t timeout_ms)
{
	nrf53_status_t st = nrf53_ap_write(ap_index, NRF53_CTRL_AP_ERASEALL, 1U);
	if (st != NRF53_OK) {
		LOG_ERR("CTRL-AP[%u] ERASEALL write failed: %s",
			ap_index, nrf53_status_str(st));
		return st;
	}

	const int64_t deadline = k_uptime_get() + (int64_t)timeout_ms;
	uint32_t status = 1;
	while (k_uptime_get() < deadline) {
		k_msleep(ERASEALL_POLL_INTERVAL_MS);
		st = nrf53_ap_read(ap_index, NRF53_CTRL_AP_ERASEALLSTATUS, &status);
		if (st != NRF53_OK) {
			LOG_WRN("CTRL-AP[%u] ERASEALLSTATUS read failed: %s",
				ap_index, nrf53_status_str(st));
			/* Don't give up immediately — sticky bits can briefly
			 * upset reads during a chip reset. Try the sticky
			 * clear and let the next poll iteration retry. */
			(void)nrf53_dp_sticky_clear();
			continue;
		}
		if (status == 0) {
			LOG_INF("CTRL-AP[%u] ERASEALL done", ap_index);
			return NRF53_OK;
		}
	}
	LOG_ERR("CTRL-AP[%u] ERASEALL timeout (last status=0x%08x)",
		ap_index, status);
	return NRF53_TIMEOUT;
}

/* ------------------------------------------------------------------ */
nrf53_status_t nrf53_erase_all(size_t *found_ap_count)
{
	if (nrf53__swdp() == NULL) {
		return NRF53_NO_DEV;
	}

	/* Stage 1 — Full DP wake. After this the DP/AP layer is stable. */
	nrf53_status_t st = nrf53_dp_full_wake(NULL);
	if (st != NRF53_OK) {
		LOG_ERR("erase_all: full_wake failed: %s", nrf53_status_str(st));
		return st;
	}

	/* Stage 2 — Discover CTRL-APs. */
	uint8_t aps[8];
	size_t count = 0;
	st = nrf53_ctrl_ap_scan(aps, sizeof(aps) / sizeof(aps[0]), &count);
	if (st != NRF53_OK) {
		LOG_ERR("erase_all: scan failed: %s", nrf53_status_str(st));
		return st;
	}
	if (count == 0) {
		LOG_ERR("erase_all: no CTRL-APs found — wrong target?");
		return NRF53_FAULT;   /* No Nordic chip on the wire */
	}
	LOG_INF("erase_all: found %u CTRL-AP(s)", (unsigned int)count);

	/* Stage 3 — For each CTRL-AP: defensive RESET pulse, then ERASEALL. */
	nrf53_status_t worst = NRF53_OK;
	for (size_t i = 0; i < count; i++) {
		uint8_t ap = aps[i];

		st = nrf53_ctrl_ap_reset_pulse(ap);
		if (st != NRF53_OK) {
			LOG_WRN("CTRL-AP[%u] reset pulse failed (continuing): %s",
				ap, nrf53_status_str(st));
			/* Don't bail — the ERASEALL might still work, and we
			 * want every chip-half attempted before reporting. */
			worst = st;
		}

		st = nrf53_ctrl_ap_eraseall(ap, 10000U);
		if (st != NRF53_OK) {
			LOG_ERR("CTRL-AP[%u] eraseall failed: %s",
				ap, nrf53_status_str(st));
			worst = st;
			continue;
		}
	}

	/* Stage 4 — Clean up DP state for the next caller. */
	(void)nrf53_dp_sticky_clear();
	(void)nrf53_dp_power_up(100);

	if (found_ap_count) {
		*found_ap_count = count;
	}
	return worst;
}
