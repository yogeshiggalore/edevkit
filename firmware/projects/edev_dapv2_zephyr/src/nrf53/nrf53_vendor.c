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
/* Wire-encoding helper                                               */
/* ------------------------------------------------------------------ */
static inline void put_u32_le(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xFFU);
	p[1] = (uint8_t)((v >> 8) & 0xFFU);
	p[2] = (uint8_t)((v >> 16) & 0xFFU);
	p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

/* ------------------------------------------------------------------ */
/* Per-command handlers                                               */
/* ------------------------------------------------------------------ */

/* NRF53_RECOVER (0x84)
 *   Request:  []
 *   Response: [u8 status, u8 ap_count,
 *              u32_le app_approtect, u32_le app_secureapprotect,
 *              u32_le net_marker, u32_le net_approtect]
 *             = 20 bytes total (echo + status + count + 4×u32)
 *
 * On success: ap_count = 2 (nRF5340) or 1 (nRF52), all four u32s set:
 *   app_approtect, app_secureapprotect, net_approtect → 0x50FA50FA
 *   net_marker → 0xDEADC0DE
 */
static uint16_t do_recover(uint8_t *const response)
{
	struct nrf53_recover_info info;
	nrf53_status_t st = nrf53_recover(&info);
	LOG_INF("NRF53_RECOVER → %s (aps=%u app=0x%08x/0x%08x net=0x%08x/0x%08x)",
		nrf53_status_str(st), info.ap_count,
		info.app_approtect, info.app_secureapprotect,
		info.net_marker, info.net_approtect);

	response[1] = (uint8_t)st;
	response[2] = info.ap_count;
	put_u32_le(&response[3],  info.app_approtect);
	put_u32_le(&response[7],  info.app_secureapprotect);
	put_u32_le(&response[11], info.net_marker);
	put_u32_le(&response[15], info.net_approtect);
	return 19U;
}

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

/* NRF53_READ_MEM (0x88) — chip-agnostic AHB-AP burst read
 *
 *   Request (13 bytes): [0x88, u8 flags, u8 ap_index,
 *                        u32_le csw, u32_le addr, u16_le word_count]
 *     flags bit 0 = clear DEMCR.VC_CORERESET before read (nRF5340 fix 6a)
 *   Response on success: [0x88, status, data[word_count*4]]  little-endian words
 *   Response on error:   [0x88, status]
 *
 * Caller must cap word_count so 2 + word_count*4 ≤ DAP packet size
 * (512 bytes minimum → 127 words max).
 */
#define NRF53_VENDOR_READ_MEM_MAX_WORDS  127U

static uint16_t do_read_mem(const uint8_t *const request, uint8_t *const response)
{
	uint8_t flags = request[1];
	uint8_t ap_index = request[2];
	uint32_t csw  = (uint32_t)request[3]
		     | ((uint32_t)request[4] << 8)
		     | ((uint32_t)request[5] << 16)
		     | ((uint32_t)request[6] << 24);
	uint32_t addr = (uint32_t)request[7]
		     | ((uint32_t)request[8] << 8)
		     | ((uint32_t)request[9] << 16)
		     | ((uint32_t)request[10] << 24);
	uint16_t word_count = (uint16_t)request[11] | ((uint16_t)request[12] << 8);

	if (word_count == 0 || word_count > NRF53_VENDOR_READ_MEM_MAX_WORDS) {
		LOG_WRN("READ_MEM: word_count=%u out of range", word_count);
		response[1] = (uint8_t)NRF53_ARGS;
		return 2U;
	}

	nrf53_status_t st = nrf53_read_mem(ap_index, csw, addr, word_count,
					   flags, &response[2]);
	response[1] = (uint8_t)st;
	if (st != NRF53_OK) {
		LOG_INF("NRF53_READ_MEM ap=%u addr=0x%08x n=%u → %s",
			ap_index, addr, word_count, nrf53_status_str(st));
		return 2U;
	}
	return (uint16_t)(2U + (uint32_t)word_count * 4U);
}

/* NRF53_WRITE_MEM (0x8C) — chip-agnostic AHB-AP burst write
 *
 *   Request (>= 13 bytes): [0x8C, u8 flags, u8 ap_index,
 *                           u32_le csw, u32_le addr, u16_le word_count,
 *                           data[word_count*4]]   (little-endian words)
 *     flags: reserved, pass 0
 *   Response: [0x8C, status]
 *
 * Caller must cap word_count so 13 + word_count*4 ≤ DAP packet size
 * (USB-FS = 64 bytes → 12 words max per call). At 64 B pkt size:
 *   13 hdr + 12*4 data = 61 bytes (fits).
 */
#define NRF53_VENDOR_WRITE_MEM_MAX_WORDS  12U

static uint16_t do_write_mem(const uint8_t *const request, uint8_t *const response)
{
	uint8_t flags = request[1];
	uint8_t ap_index = request[2];
	uint32_t csw  = (uint32_t)request[3]
		     | ((uint32_t)request[4] << 8)
		     | ((uint32_t)request[5] << 16)
		     | ((uint32_t)request[6] << 24);
	uint32_t addr = (uint32_t)request[7]
		     | ((uint32_t)request[8] << 8)
		     | ((uint32_t)request[9] << 16)
		     | ((uint32_t)request[10] << 24);
	uint16_t word_count = (uint16_t)request[11] | ((uint16_t)request[12] << 8);

	if (word_count == 0 || word_count > NRF53_VENDOR_WRITE_MEM_MAX_WORDS) {
		LOG_WRN("WRITE_MEM: word_count=%u out of range", word_count);
		response[1] = (uint8_t)NRF53_ARGS;
		return 2U;
	}

	nrf53_status_t st = nrf53_write_mem(ap_index, csw, addr, word_count,
					    flags, &request[13]);
	response[1] = (uint8_t)st;
	if (st != NRF53_OK) {
		LOG_INF("NRF53_WRITE_MEM ap=%u addr=0x%08x n=%u → %s",
			ap_index, addr, word_count, nrf53_status_str(st));
	}
	return 2U;
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
	put_u32_le(&response[2], approtect);
	put_u32_le(&response[6], secure);
	return 10U;
}

/* NRF53_UICR_PROGRAM_NET (0x8B)
 *   Request:  []
 *   Response: [u8 status, u32 sram_marker, u32 net_approtect_readback]
 *             (10 bytes total: echo + status + 4 + 4)
 *
 * On success: marker=0xDEADC0DE, approtect=0x50FA50FA.
 * On stub fault: marker=0xBADF00D5; PC+xPSR are logged probe-side.
 */
static uint16_t do_uicr_program_net(uint8_t *const response)
{
	uint32_t marker = 0;
	uint32_t approtect = 0xFFFFFFFFU;
	nrf53_status_t st = nrf53_uicr_program_net(&marker, &approtect);
	LOG_INF("NRF53_UICR_PROGRAM_NET → %s (marker=0x%08x APPROTECT=0x%08x)",
		nrf53_status_str(st), marker, approtect);

	response[1] = (uint8_t)st;
	put_u32_le(&response[2], marker);
	put_u32_le(&response[6], approtect);
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
	case NRF53_VENDOR_RECOVER:
		response[0] = cmd;
		return do_recover(response);

	case NRF53_VENDOR_ERASE:
		response[0] = cmd;
		return do_erase(response);

	case NRF53_VENDOR_READ_MEM:
		response[0] = cmd;
		return do_read_mem(request, response);

	case NRF53_VENDOR_UICR_PROGRAM_APP:
		response[0] = cmd;
		return do_uicr_program_app(response);

	case NRF53_VENDOR_UICR_PROGRAM_NET:
		response[0] = cmd;
		return do_uicr_program_net(response);

	case NRF53_VENDOR_WRITE_MEM:
		response[0] = cmd;
		return do_write_mem(request, response);

	/* Handlers for 0x86 / 0x87 / 0x89 land in subsequent steps. They
	 * fall through to ID_DAP_INVALID until then. */

	default:
		response[0] = ID_DAP_INVALID;
		return 1U;
	}
}
