/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * nrf53_recover.c — top-level RECOVER orchestration.
 *
 * Composes the building blocks from steps 2-4:
 *   nrf53_erase_all          → CTRL-AP ERASEALL both cores (step 2)
 *   nrf53_uicr_program_app   → host-side App UICR write   (step 3)
 *   nrf53_uicr_program_net   → on-target Net stub + UICR  (step 4)
 *
 * Matches the Python reference's `_recover_sync()`. Algorithm reference:
 * docs/NRF5340_ALGORITHMS.md §4.
 *
 * On success: chip is fully unlocked (both UICRs = 0x50FA50FA) and
 * ready for host-driven flash programming via standard CMSIS-DAP
 * DAP_Transfer commands. This is the release gate for the Zephyr port
 * to reach functional parity with the pico-sdk Python webgui.
 */

#include "nrf53.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(nrf53_recover, CONFIG_EDEV_NRF53_OPS_LOG_LEVEL);

nrf53_status_t nrf53_recover(struct nrf53_recover_info *info)
{
	/* Always populate info even on failure — the host wants to see
	 * how far we got and what state the chip ended up in. */
	struct nrf53_recover_info local = {
		.ap_count = 0,
		.app_approtect = 0xFFFFFFFFU,
		.app_secureapprotect = 0xFFFFFFFFU,
		.net_marker = 0,
		.net_approtect = 0xFFFFFFFFU,
	};

	LOG_INF("RECOVER: stage 1 — ERASE both cores");
	size_t ap_count = 0;
	nrf53_status_t st = nrf53_erase_all(&ap_count);
	local.ap_count = (uint8_t)ap_count;
	if (st != NRF53_OK) {
		LOG_ERR("RECOVER: erase_all failed: %s", nrf53_status_str(st));
		if (info) { *info = local; }
		return st;
	}

	LOG_INF("RECOVER: stage 2 — program App UICR");
	st = nrf53_uicr_program_app(&local.app_approtect,
				    &local.app_secureapprotect);
	if (st != NRF53_OK) {
		LOG_ERR("RECOVER: uicr_program_app failed: %s",
			nrf53_status_str(st));
		if (info) { *info = local; }
		return st;
	}

	LOG_INF("RECOVER: stage 3 — program Net UICR (via on-target stub)");
	st = nrf53_uicr_program_net(&local.net_marker, &local.net_approtect);
	if (st != NRF53_OK) {
		LOG_ERR("RECOVER: uicr_program_net failed: %s",
			nrf53_status_str(st));
		if (info) { *info = local; }
		return st;
	}

	LOG_INF("RECOVER complete: App APPROTECT=0x%08x SECURE=0x%08x "
		"Net marker=0x%08x APPROTECT=0x%08x",
		local.app_approtect, local.app_secureapprotect,
		local.net_marker, local.net_approtect);

	if (info) { *info = local; }
	return NRF53_OK;
}
