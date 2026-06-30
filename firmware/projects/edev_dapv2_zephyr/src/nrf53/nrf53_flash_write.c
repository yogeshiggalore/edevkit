/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * nrf53_flash_write.c — Net flash write via direct AP#1 + Net NVMC.
 *
 * Wraps the Nordic NVMC programming sequence into a single probe-side
 * operation: enter Wen → per-word write+poll READY → exit Ren. Caller
 * (vendor cmd 0x86 dispatch) hands us a chunk of words to write at a
 * given Net flash address; we do all the NVMC dance internally.
 *
 * Why a vendor cmd (vs composing on the host with 0x8C WRITE_MEM):
 *   - Each NVMC write takes ~40 µs on Nordic silicon; the wait-ready
 *     poll loop is faster running probe-side than over USB.
 *   - Wen/Ren wrapping is amortized over the whole chunk (one Wen at
 *     start, one Ren at end) rather than per-word.
 *   - Net AHB-AP CSW (0x03800042) is non-obvious; encoding it once in
 *     this command instead of every WRITE_MEM call saves complexity
 *     on the bridge side.
 *
 * Net AHB-AP must already be reachable (which it is right after
 * NRF53_ERASE — CTRL-AP#3 ERASEALL unlocks Net AHB-AP for the session).
 *
 * Algorithm reference: docs/NRF5340_ALGORITHMS.md §5b.
 */

#include "nrf53.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(nrf53_flash_write, CONFIG_EDEV_NRF53_OPS_LOG_LEVEL);

#define NVMC_TIMEOUT_MS           100U   /* per-word READY timeout */
#define NVMC_CONFIG_TIMEOUT_MS    500U   /* config-mode change timeout */

nrf53_status_t nrf53_flash_write_net(uint32_t addr, uint32_t word_count,
				     const uint8_t *data,
				     uint32_t *out_words_written)
{
	if (out_words_written) {
		*out_words_written = 0;
	}
	if (data == NULL || word_count == 0 || (addr & 0x03U) != 0) {
		return NRF53_ARGS;
	}

	/* Bug 6b mitigation — clear sticky bits before Net AHB-AP work. */
	(void)nrf53_dp_sticky_clear();
	(void)nrf53_dp_power_up(50);

	/* Enter Wen mode (the helper waits READY before and after). */
	nrf53_status_t st = nrf53_nvmc_set_config(NRF53_AP_NET, NRF53_CSW_NET,
						  NRF53_NET_NVMC_READY,
						  NRF53_NET_NVMC_CONFIG,
						  NRF53_NVMC_CONFIG_WEN,
						  NVMC_CONFIG_TIMEOUT_MS);
	if (st != NRF53_OK) {
		LOG_ERR("Net NVMC.CONFIG=Wen failed: %s", nrf53_status_str(st));
		return st;
	}

	uint32_t i;
	for (i = 0; i < word_count; i++) {
		uint32_t word = (uint32_t)data[i * 4U + 0U]
			     | ((uint32_t)data[i * 4U + 1U] << 8)
			     | ((uint32_t)data[i * 4U + 2U] << 16)
			     | ((uint32_t)data[i * 4U + 3U] << 24);

		/* Skip erased pattern — Net NVMC can't program 0xFF→0xFF
		 * (no-op) and skipping saves USB time on sparse data. */
		if (word == 0xFFFFFFFFU) {
			continue;
		}

		uint32_t cur_addr = addr + (i * 4U);
		st = nrf53_mem_write(NRF53_AP_NET, NRF53_CSW_NET, cur_addr, word);
		if (st != NRF53_OK) {
			LOG_ERR("Net flash write[%u] @ 0x%08x = 0x%08x failed: %s",
				(unsigned int)i, cur_addr, word, nrf53_status_str(st));
			break;
		}

		st = nrf53_nvmc_wait_ready(NRF53_AP_NET, NRF53_CSW_NET,
					   NRF53_NET_NVMC_READY,
					   NVMC_TIMEOUT_MS);
		if (st != NRF53_OK) {
			LOG_ERR("Net NVMC.READY after word @ 0x%08x failed: %s",
				cur_addr, nrf53_status_str(st));
			break;
		}
	}

	/* Restore Ren mode regardless of mid-loop failure — never leave
	 * flash write-enabled. Reports the worse of the two errors. */
	nrf53_status_t st_ren = nrf53_nvmc_set_config(NRF53_AP_NET, NRF53_CSW_NET,
						      NRF53_NET_NVMC_READY,
						      NRF53_NET_NVMC_CONFIG,
						      NRF53_NVMC_CONFIG_REN,
						      NVMC_CONFIG_TIMEOUT_MS);
	if (st_ren != NRF53_OK) {
		LOG_WRN("Net NVMC.CONFIG=Ren restore failed: %s",
			nrf53_status_str(st_ren));
		if (st == NRF53_OK) {
			st = st_ren;
		}
	}

	if (out_words_written) {
		*out_words_written = i;
	}

	if (st == NRF53_OK) {
		LOG_INF("Net flash wrote %u words @ 0x%08x", (unsigned int)i, addr);
	}
	return st;
}
