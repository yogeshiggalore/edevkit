/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * nrf53_vendor.c — CMSIS-DAP vendor command (0x80..0x9F) dispatcher.
 *
 * Provides a strong override for `dap_process_vendor_cmd()` (which is
 * weakly defined in zephyr-patches/0003-cmsis_dap-weak-vendor-cmd-handler.patch)
 * so our high-level nRF5340 ops are reachable over USB CMSIS-DAP. Handlers
 * are added one-per-step as the implementation lands.
 *
 * Wire format (CMSIS-DAP standard):
 *   Request:  [0]=cmd byte (0x80..0x9F), [1..]=payload
 *   Response: [0]=cmd byte (echo) or 0xFF (ID_DAP_INVALID, unknown cmd),
 *             [1]=status byte (0=OK; nrf53_status_t for our cmds),
 *             [2..]=op-specific payload
 *   Return: number of response bytes used.
 */

#include "nrf53.h"

#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(nrf53_vendor, CONFIG_EDEV_NRF53_OPS_LOG_LEVEL);

/* CMSIS-DAP invalid-command sentinel */
#define ID_DAP_INVALID 0xFFU

/* Forward declaration of the Zephyr DAP context — we don't dereference
 * it here, but the function signature must match the upstream weak. */
struct dap_link_context;

/* ------------------------------------------------------------------ */
/* Per-command handlers                                               */
/* ------------------------------------------------------------------ */

/* NRF53_ERASE (0x85)
 *   Request:  []
 *   Response: [u8 status, u8 ap_count_found] (3 bytes total: echo+status+count)
 */
static uint16_t do_erase(uint8_t *const response)
{
	size_t count = 0;
	nrf53_status_t st = nrf53_erase_all(&count);
	LOG_INF("NRF53_ERASE → %s (found %u CTRL-APs)",
		nrf53_status_str(st), (unsigned int)count);

	response[1] = (uint8_t)st;
	response[2] = (uint8_t)count;
	return 3U;
}

/* NRF53_UICR_PROGRAM_APP (0x8A)
 *   Request:  []
 *   Response: [u8 status, u32 approtect_readback, u32 secureapprotect_readback]
 *             (10 bytes total: echo + status + 4 + 4)
 *
 * Readbacks are little-endian. On success expect both = 0x50FA50FA.
 * On failure the readbacks tell the host what state the chip is in.
 */
static uint16_t do_uicr_program_app(uint8_t *const response)
{
	uint32_t approtect = 0xFFFFFFFFU;
	uint32_t secure = 0xFFFFFFFFU;
	nrf53_status_t st = nrf53_uicr_program_app(&approtect, &secure);
	LOG_INF("NRF53_UICR_PROGRAM_APP → %s (APPROTECT=0x%08x SECURE=0x%08x)",
		nrf53_status_str(st), approtect, secure);

	response[1] = (uint8_t)st;
	response[2] = (uint8_t)(approtect & 0xFFU);
	response[3] = (uint8_t)((approtect >> 8) & 0xFFU);
	response[4] = (uint8_t)((approtect >> 16) & 0xFFU);
	response[5] = (uint8_t)((approtect >> 24) & 0xFFU);
	response[6] = (uint8_t)(secure & 0xFFU);
	response[7] = (uint8_t)((secure >> 8) & 0xFFU);
	response[8] = (uint8_t)((secure >> 16) & 0xFFU);
	response[9] = (uint8_t)((secure >> 24) & 0xFFU);
	return 10U;
}

/* ------------------------------------------------------------------ */
/* Strong override of the weakly-defined Zephyr handler.              */
/*                                                                    */
/* dap_process_cmd() in subsys/dap/cmsis_dap.c already filtered the   */
/* command byte to the 0x80..0x9F range before calling us. The echo   */
/* of request[0] into response[0] is our responsibility (the standard */
/* command dispatch in dap_process_cmd echoes only for non-vendor).   */
/* ------------------------------------------------------------------ */
uint16_t dap_process_vendor_cmd(struct dap_link_context *const ctx,
				const uint8_t *const request,
				uint8_t *const response)
{
	ARG_UNUSED(ctx);

	uint8_t cmd = request[0];
	LOG_DBG("vendor cmd 0x%02x", cmd);

	switch (cmd) {
	case NRF53_VENDOR_ERASE:
		response[0] = cmd;
		return do_erase(response);

	case NRF53_VENDOR_UICR_PROGRAM_APP:
		response[0] = cmd;
		return do_uicr_program_app(response);

	/* Handlers for 0x84 / 0x86..0x89 / 0x8B land in subsequent steps.
	 * They fall through to ID_DAP_INVALID until then. */

	default:
		response[0] = ID_DAP_INVALID;
		return 1U;
	}
}
