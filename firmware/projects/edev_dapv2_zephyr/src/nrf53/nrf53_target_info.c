/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * nrf53_target_info.c — single-call ARM target identification.
 *
 * Returns DPIDR + AP[0].IDR + CPUID — the three universal ARM
 * identification fields. The bridge can derive chip family from
 * DPIDR.VERSION + CPUID, then compose chip-specific FICR reads via
 * `0x88 NRF53_READ_MEM` if it needs subtype (e.g. nRF52840 vs
 * nRF52833).
 *
 * Why not include FICR in this call: empirical finding 2026-06-30 EOD
 * — on the bench nRF5340 DK silicon, FICR.INFO.PART at the documented
 * offset (`+0x140`) returns 0xFFFFFFFF. Either the chip is an
 * engineering sample with unprogrammed FICR, or the App AHB-AP's
 * mapping of `0x00FF0000` differs from what the spec implies, or the
 * offset is wrong for this variant. Until that's resolved, the bridge
 * is better off doing its own targeted FICR reads at chip-specific
 * addresses (where it can also handle the read failure mode it sees
 * empirically).
 */

#include "nrf53.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(nrf53_target_info, CONFIG_EDEV_NRF53_OPS_LOG_LEVEL);

/* Cortex-M System Control Space — CPUID register, same on all Cortex-M */
#define CORTEX_M_CPUID  0xE000ED00U

nrf53_status_t nrf53_target_info(struct nrf53_target_info *out)
{
	if (out == NULL) {
		return NRF53_ARGS;
	}
	out->dpidr = 0;
	out->ap0_idr = 0;
	out->cpuid = 0;

	/* DPIDR via full wake — mandatory first xact after line reset */
	nrf53_status_t st = nrf53_dp_full_wake(&out->dpidr);
	if (st != NRF53_OK) {
		LOG_ERR("TARGET_INFO: dp_full_wake failed: %s", nrf53_status_str(st));
		return st;
	}

	/* AHB-AP[0].IDR — uses ap_read which selects bank 0xF for IDR. */
	st = nrf53_ap_read(NRF53_AP_APP, NRF53_AP_IDR, &out->ap0_idr);
	if (st != NRF53_OK) {
		LOG_ERR("TARGET_INFO: AP[0].IDR failed: %s", nrf53_status_str(st));
		return st;
	}

	/* CPUID — same address on every Cortex-M (0xE000ED00 in SCS). */
	st = nrf53_mem_read(NRF53_AP_APP, NRF53_CSW_APP, CORTEX_M_CPUID,
			    &out->cpuid);
	if (st != NRF53_OK) {
		LOG_ERR("TARGET_INFO: CPUID @ 0xE000ED00 failed: %s",
			nrf53_status_str(st));
		return st;
	}

	LOG_INF("TARGET_INFO: DPIDR=0x%08x AP_IDR=0x%08x CPUID=0x%08x",
		out->dpidr, out->ap0_idr, out->cpuid);
	return NRF53_OK;
}
