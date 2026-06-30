/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * nrf53_read_mem.c — chip-agnostic AHB-AP burst memory read.
 *
 * Works on any ARM Cortex-M target reachable via AHB-AP — nRF52840,
 * nRF5340 App, nRF5340 Net, STM32, etc. Caller passes the AP index +
 * CSW; we do per-word TAR writes (no auto-increment) to avoid Nordic's
 * 1-KB TAR-wrap quirk.
 *
 * Optional VC_CORERESET-clear flag fixes the nRF5340 App-readback-0xFF
 * post-reset bug (Bug 6a) — see docs/NRF5340_ALGORITHMS.md §6 + §8.
 * Harmless to set on non-nRF5340 targets (DEMCR is the same on all
 * Cortex-M cores, writing 0 to it just clears halt-on-reset which is
 * already 0 in normal operation).
 *
 * Exposed as CMSIS-DAP vendor command 0x88 NRF53_READ_MEM via
 * nrf53_vendor.c.
 */

#include "nrf53.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(nrf53_read_mem, CONFIG_EDEV_NRF53_OPS_LOG_LEVEL);

/* Wait this long after clearing VC_CORERESET before the first read.
 * Empirically the flash controller needs ~tens of ms to come up after
 * the halt-at-reset is released. 50 ms matches the Python reference. */
#define VC_CORERESET_SETTLE_MS  50U

nrf53_status_t nrf53_read_mem(uint8_t ap_index, uint32_t csw,
			      uint32_t addr, uint32_t word_count,
			      uint32_t flags, uint8_t *out)
{
	if (out == NULL || word_count == 0 || (addr & 0x03U) != 0) {
		return NRF53_ARGS;
	}

	/* Bug 6a fix path: clear DEMCR.VC_CORERESET so the core isn't
	 * halted at the reset vector during the readback. Only meaningful
	 * on nRF5340 App after a CTRL-AP RESET; harmless elsewhere. */
	if (flags & NRF53_READ_FLAG_VC_CORERESET_CLEAR) {
		nrf53_status_t st = nrf53_mem_write(ap_index, csw,
						    NRF53_DEMCR, 0U);
		if (st != NRF53_OK) {
			LOG_WRN("DEMCR clear failed: %s — proceeding anyway",
				nrf53_status_str(st));
			/* Don't fail — DEMCR write can return WAIT on a chip
			 * that's mid-reset; the subsequent read will still
			 * work because the reset settles fast. */
		}
		k_msleep(VC_CORERESET_SETTLE_MS);
	}

	/* Re-arm DP between high-level ops. Cheap, idempotent. */
	(void)nrf53_dp_sticky_clear();

	for (uint32_t i = 0; i < word_count; i++) {
		uint32_t cur_addr = addr + (i * 4U);
		uint32_t word = 0;
		nrf53_status_t st = nrf53_mem_read(ap_index, csw, cur_addr, &word);
		if (st != NRF53_OK) {
			LOG_ERR("mem_read[%u] @ 0x%08x failed: %s",
				(unsigned int)i, cur_addr, nrf53_status_str(st));
			return st;
		}
		out[i * 4U + 0U] = (uint8_t)(word & 0xFFU);
		out[i * 4U + 1U] = (uint8_t)((word >> 8) & 0xFFU);
		out[i * 4U + 2U] = (uint8_t)((word >> 16) & 0xFFU);
		out[i * 4U + 3U] = (uint8_t)((word >> 24) & 0xFFU);
	}
	return NRF53_OK;
}

/* ------------------------------------------------------------------ */
nrf53_status_t nrf53_write_mem(uint8_t ap_index, uint32_t csw,
			       uint32_t addr, uint32_t word_count,
			       uint32_t flags, const uint8_t *data)
{
	ARG_UNUSED(flags);
	if (data == NULL || word_count == 0 || (addr & 0x03U) != 0) {
		return NRF53_ARGS;
	}

	(void)nrf53_dp_sticky_clear();

	for (uint32_t i = 0; i < word_count; i++) {
		uint32_t cur_addr = addr + (i * 4U);
		uint32_t word = (uint32_t)data[i * 4U + 0U]
			     | ((uint32_t)data[i * 4U + 1U] << 8)
			     | ((uint32_t)data[i * 4U + 2U] << 16)
			     | ((uint32_t)data[i * 4U + 3U] << 24);
		nrf53_status_t st = nrf53_mem_write(ap_index, csw, cur_addr, word);
		if (st != NRF53_OK) {
			LOG_ERR("mem_write[%u] @ 0x%08x = 0x%08x failed: %s",
				(unsigned int)i, cur_addr, word, nrf53_status_str(st));
			return st;
		}
	}
	return NRF53_OK;
}
