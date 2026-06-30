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

	/* Handlers for 0x84 / 0x86..0x8B land in subsequent steps. They
	 * intentionally fall through to ID_DAP_INVALID until then. */

	default:
		response[0] = ID_DAP_INVALID;
		return 1U;
	}
}
