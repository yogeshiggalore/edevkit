/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * nrf53_uicr.c — UICR.APPROTECT / SECUREAPPROTECT programming.
 *
 * App core: host-side NVMC writes via App AHB-AP (this file).
 * Net core: on-target stub — see nrf53_stubs.c (step 4).
 *
 * Algorithm reference: docs/NRF5340_ALGORITHMS.md §4 Stage 3.
 *
 * Why on-target only for Net: nRF5340's Net core is "Selectable Secure"
 * — its peripherals fault under bare Secure-mode AHB-AP writes until
 * the SPU has been configured. The App core's secure context is set
 * up by silicon defaults, so host-side writes work directly. See
 * memory note reference_nrf5340_net_uicr_write_attempts_2026_06_26.
 */

#include "nrf53.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(nrf53_uicr, CONFIG_EDEV_NRF53_OPS_LOG_LEVEL);

/* Per-step NVMC.READY timeout. The Python reference uses 0.5 s; we
 * stick with the same since the Pico → host USB round-trip cost is in
 * the wait loop and 0.5 s leaves slack for slow chips. */
#define UICR_NVMC_TIMEOUT_MS   500U

nrf53_status_t nrf53_uicr_program_app(uint32_t *out_approtect,
				      uint32_t *out_secureapprotect)
{
	const uint8_t ap = NRF53_AP_APP;
	const uint32_t csw = NRF53_CSW_APP;
	const uint32_t ready = NRF53_APP_NVMC_READY;
	const uint32_t config = NRF53_APP_NVMC_CONFIG;
	nrf53_status_t st;

	/* Re-arm the DP before we start. Doesn't tear down a previously
	 * attached session — just clears any sticky bits and re-asserts
	 * the power request. Cheap. */
	(void)nrf53_dp_sticky_clear();
	(void)nrf53_dp_power_up(50);

	/* CONFIG ← Wen (the helper waits READY before and after). */
	st = nrf53_nvmc_set_config(ap, csw, ready, config,
				   NRF53_NVMC_CONFIG_WEN, UICR_NVMC_TIMEOUT_MS);
	if (st != NRF53_OK) {
		LOG_ERR("App NVMC.CONFIG=Wen failed: %s", nrf53_status_str(st));
		return st;
	}

	/* Write UICR.APPROTECT = HwDisabled. */
	st = nrf53_mem_write(ap, csw, NRF53_APP_UICR_APPROTECT, NRF53_UNLOCK_VAL);
	if (st != NRF53_OK) {
		LOG_ERR("App UICR.APPROTECT write failed: %s", nrf53_status_str(st));
		goto restore_ren;
	}
	st = nrf53_nvmc_wait_ready(ap, csw, ready, UICR_NVMC_TIMEOUT_MS);
	if (st != NRF53_OK) {
		LOG_ERR("App NVMC.READY after APPROTECT failed: %s", nrf53_status_str(st));
		goto restore_ren;
	}

	/* Write UICR.SECUREAPPROTECT = HwDisabled. */
	st = nrf53_mem_write(ap, csw, NRF53_APP_UICR_SECUREAPPROTECT, NRF53_UNLOCK_VAL);
	if (st != NRF53_OK) {
		LOG_ERR("App UICR.SECUREAPPROTECT write failed: %s",
			nrf53_status_str(st));
		goto restore_ren;
	}
	st = nrf53_nvmc_wait_ready(ap, csw, ready, UICR_NVMC_TIMEOUT_MS);
	if (st != NRF53_OK) {
		LOG_ERR("App NVMC.READY after SECUREAPPROTECT failed: %s",
			nrf53_status_str(st));
		goto restore_ren;
	}

	/* CONFIG ← Ren — must happen on success or failure so we don't
	 * leave the chip with write-enabled flash. */
restore_ren:
	{
		nrf53_status_t st_ren = nrf53_nvmc_set_config(
			ap, csw, ready, config,
			NRF53_NVMC_CONFIG_REN, UICR_NVMC_TIMEOUT_MS);
		if (st_ren != NRF53_OK) {
			LOG_WRN("App NVMC.CONFIG=Ren restore failed: %s",
				nrf53_status_str(st_ren));
			/* If the program succeeded but Ren restore failed,
			 * surface the Ren failure — flash left in WEN is the
			 * more dangerous state. */
			if (st == NRF53_OK) {
				st = st_ren;
			}
		}
	}

	/* Readback for caller visibility — always attempt, even on
	 * earlier failure, so the host can see what state the chip is
	 * in. */
	uint32_t approtect = 0xFFFFFFFFU;
	uint32_t secure = 0xFFFFFFFFU;
	(void)nrf53_mem_read(ap, csw, NRF53_APP_UICR_APPROTECT, &approtect);
	(void)nrf53_mem_read(ap, csw, NRF53_APP_UICR_SECUREAPPROTECT, &secure);

	if (out_approtect) { *out_approtect = approtect; }
	if (out_secureapprotect) { *out_secureapprotect = secure; }

	if (st == NRF53_OK) {
		LOG_INF("App UICR programmed: APPROTECT=0x%08x SECUREAPPROTECT=0x%08x",
			approtect, secure);
	}
	return st;
}
