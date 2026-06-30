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
#define NVMC_ERASE_TIMEOUT_MS     500U   /* page erase typ 85 ms */

/* DHCSR mirror — only used by the boot-region halt-before-erase path */
#define DHCSR_ADDR             0xE000EDF0U
#define DHCSR_HALT_REQ         0xA05F0003U   /* DBGKEY | C_DEBUGEN | C_HALT */

/* ── Net page-0 dirty tracking ────────────────────────────────────
 * RECOVER's nrf53_uicr_program_net() writes the 224-byte UICR-disable
 * stub into Net flash page 0 (0x01000000..0x010000DF). Any later
 * user write to that page needs to erase first because NVMC can only
 * clear bits — programming non-erased flash AND's with existing data
 * and the readback won't match. ERASEALL wipes everything; this flag
 * tracks whether the residue is currently there. */
static bool s_net_page0_dirty;

void nrf53_net_page0_mark_dirty(void)  { s_net_page0_dirty = true; }
void nrf53_net_page0_mark_clean(void)  { s_net_page0_dirty = false; }
bool nrf53_net_page0_is_dirty(void)    { return s_net_page0_dirty; }

/* Net flash page erase: NVMC.ERASEPAGE register is non-functional on
 * nRF5340 Net core. The working method is CONFIG=Een + write
 * 0xFFFFFFFF to any address in the target page (matches the nrfx HAL
 * fallback at `nrf_nvmc.h` lines ~417-441). Bench-verified 2026-06-30.
 *
 * Required when overwriting bytes that are not already 0xFFFFFFFF —
 * critical for the boot region (0x01000000..0x010000FF) where RECOVER
 * leaves the UICR-disable stub. NVMC can only flip 1→0 without erase;
 * writing non-erased content gets bitwise-AND'd with the existing data.
 */
static nrf53_status_t nrf53_net_page_erase(uint32_t addr)
{
	/* Halt Net CPU first — if it's executing from this page, the erase
	 * will pull the rug out from under it. Best-effort: DHCSR halt may
	 * be ignored if the core is in lockup, but that's fine — we proceed
	 * either way. */
	(void)nrf53_mem_write(NRF53_AP_NET, NRF53_CSW_NET, DHCSR_ADDR,
			      DHCSR_HALT_REQ);

	nrf53_status_t st = nrf53_nvmc_set_config(NRF53_AP_NET, NRF53_CSW_NET,
						  NRF53_NET_NVMC_READY,
						  NRF53_NET_NVMC_CONFIG,
						  NRF53_NVMC_CONFIG_EEN,
						  NVMC_CONFIG_TIMEOUT_MS);
	if (st != NRF53_OK) {
		LOG_ERR("Net NVMC.CONFIG=Een failed: %s", nrf53_status_str(st));
		return st;
	}

	/* Write 0xFFFFFFFF to target address — triggers page erase. */
	st = nrf53_mem_write(NRF53_AP_NET, NRF53_CSW_NET, addr, 0xFFFFFFFFU);
	if (st != NRF53_OK) {
		LOG_ERR("Net page-erase trigger write @ 0x%08x failed: %s",
			addr, nrf53_status_str(st));
		(void)nrf53_nvmc_set_config(NRF53_AP_NET, NRF53_CSW_NET,
					    NRF53_NET_NVMC_READY,
					    NRF53_NET_NVMC_CONFIG,
					    NRF53_NVMC_CONFIG_REN,
					    NVMC_CONFIG_TIMEOUT_MS);
		return st;
	}

	st = nrf53_nvmc_wait_ready(NRF53_AP_NET, NRF53_CSW_NET,
				   NRF53_NET_NVMC_READY,
				   NVMC_ERASE_TIMEOUT_MS);
	if (st != NRF53_OK) {
		LOG_ERR("Net page-erase READY timeout @ 0x%08x: %s",
			addr, nrf53_status_str(st));
	}

	/* Restore Ren regardless of erase outcome — never leave NVMC in Een */
	nrf53_status_t st_ren = nrf53_nvmc_set_config(NRF53_AP_NET, NRF53_CSW_NET,
						      NRF53_NET_NVMC_READY,
						      NRF53_NET_NVMC_CONFIG,
						      NRF53_NVMC_CONFIG_REN,
						      NVMC_CONFIG_TIMEOUT_MS);
	if (st_ren != NRF53_OK && st == NRF53_OK) {
		st = st_ren;
	}
	if (st == NRF53_OK) {
		LOG_INF("Net page erased @ 0x%08x", addr & ~0x7FFU);
	}
	return st;
}

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

	/* One-shot auto-erase of Net flash page 0 — the only page that
	 * has non-0xFF residue after RECOVER (UICR-disable stub lives
	 * there). The flag is set to true when nrf53_uicr_program_net()
	 * runs and cleared either after a successful erase here or
	 * after a chip-wide ERASEALL via nrf53_net_page0_mark_clean().
	 *
	 * Why a flag instead of "read page_base and erase if dirty":
	 * the bridge writes a page in batches (≤14 words / 56 B each);
	 * after batch 1, page_base contains user data, so a "read +
	 * check FF" heuristic re-triggers erase on batch 2 and wipes
	 * the page. Tracking dirtiness explicitly avoids that. */
	const uint32_t page_base = addr & ~0x7FFU;   /* 2 KB pages on Net */
	if (page_base == 0x01000000U && nrf53_net_page0_is_dirty()) {
		LOG_INF("Net page 0 has UICR stub residue — erasing before write");
		nrf53_status_t es = nrf53_net_page_erase(page_base);
		if (es != NRF53_OK) {
			LOG_ERR("Net page 0 auto-erase failed: %s",
				nrf53_status_str(es));
			return es;
		}
		nrf53_net_page0_mark_clean();
		/* DP may be jittery after the erase; re-anchor. */
		(void)nrf53_dp_sticky_clear();
		(void)nrf53_dp_power_up(50);
	}

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

/* ------------------------------------------------------------------ */
nrf53_status_t nrf53_flash_write_app(uint32_t nvmc_base, uint32_t addr,
				     uint32_t word_count, const uint8_t *data,
				     uint32_t *out_words_written)
{
	if (out_words_written) {
		*out_words_written = 0;
	}
	if (data == NULL || word_count == 0 || (addr & 0x03U) != 0
	    || (nvmc_base & 0x0FFFU) != 0) {
		return NRF53_ARGS;
	}

	const uint32_t nvmc_ready = nvmc_base + 0x400U;
	const uint32_t nvmc_config = nvmc_base + 0x504U;

	(void)nrf53_dp_sticky_clear();
	(void)nrf53_dp_power_up(50);

	nrf53_status_t st = nrf53_nvmc_set_config(NRF53_AP_APP, NRF53_CSW_APP,
						  nvmc_ready, nvmc_config,
						  NRF53_NVMC_CONFIG_WEN,
						  NVMC_CONFIG_TIMEOUT_MS);
	if (st != NRF53_OK) {
		LOG_ERR("App NVMC.CONFIG=Wen @ 0x%08x failed: %s",
			nvmc_config, nrf53_status_str(st));
		return st;
	}

	uint32_t i;
	for (i = 0; i < word_count; i++) {
		uint32_t word = (uint32_t)data[i * 4U + 0U]
			     | ((uint32_t)data[i * 4U + 1U] << 8)
			     | ((uint32_t)data[i * 4U + 2U] << 16)
			     | ((uint32_t)data[i * 4U + 3U] << 24);

		if (word == 0xFFFFFFFFU) {
			continue;
		}

		uint32_t cur_addr = addr + (i * 4U);
		st = nrf53_mem_write(NRF53_AP_APP, NRF53_CSW_APP, cur_addr, word);
		if (st != NRF53_OK) {
			LOG_ERR("App flash write[%u] @ 0x%08x = 0x%08x failed: %s",
				(unsigned int)i, cur_addr, word, nrf53_status_str(st));
			break;
		}

		st = nrf53_nvmc_wait_ready(NRF53_AP_APP, NRF53_CSW_APP,
					   nvmc_ready, NVMC_TIMEOUT_MS);
		if (st != NRF53_OK) {
			LOG_ERR("App NVMC.READY after word @ 0x%08x failed: %s",
				cur_addr, nrf53_status_str(st));
			break;
		}
	}

	nrf53_status_t st_ren = nrf53_nvmc_set_config(NRF53_AP_APP, NRF53_CSW_APP,
						      nvmc_ready, nvmc_config,
						      NRF53_NVMC_CONFIG_REN,
						      NVMC_CONFIG_TIMEOUT_MS);
	if (st_ren != NRF53_OK) {
		LOG_WRN("App NVMC.CONFIG=Ren restore failed: %s",
			nrf53_status_str(st_ren));
		if (st == NRF53_OK) {
			st = st_ren;
		}
	}

	if (out_words_written) {
		*out_words_written = i;
	}

	if (st == NRF53_OK) {
		LOG_INF("App flash wrote %u words @ 0x%08x (NVMC=0x%08x)",
			(unsigned int)i, addr, nvmc_base);
	}
	return st;
}
