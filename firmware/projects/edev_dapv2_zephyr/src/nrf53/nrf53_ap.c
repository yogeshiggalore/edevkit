/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * nrf53_ap.c — AP layer + AHB-AP memory helpers.
 *
 * Always re-writes DP.SELECT (and AP.CSW for mem ops) before any AP
 * transaction — never trusts cached state across calls. That's what
 * lets us coexist with the CMSIS-DAP dispatcher (which keeps its own
 * SELECT/CSW cache) without corrupting either side's view.
 *
 * Algorithm reference: docs/NRF5340_ALGORITHMS.md §6.
 */

#include "nrf53.h"
#include "nrf53_priv.h"

#include <zephyr/drivers/swdp.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(nrf53_ap, CONFIG_EDEV_NRF53_OPS_LOG_LEVEL);

/* ------------------------------------------------------------------ */
/* Raw AP transfer — same shape as DP transfer but with APnDP=1.      */
/* ------------------------------------------------------------------ */
static nrf53_status_t ap_xfer(uint8_t reg_offset, bool is_read, uint32_t *data)
{
	if ((reg_offset & 0x03U) != 0 || reg_offset > 0x0CU) {
		return NRF53_ARGS;
	}
	uint8_t req = SWDP_REQUEST_APnDP;
	if (is_read) { req |= SWDP_REQUEST_RnW; }
	if (reg_offset & 0x04U) { req |= SWDP_REQUEST_A2; }
	if (reg_offset & 0x08U) { req |= SWDP_REQUEST_A3; }
	return nrf53__transfer(req, data);
}

/* ------------------------------------------------------------------ */
/* SELECT helper                                                      */
/* ------------------------------------------------------------------ */
nrf53_status_t nrf53_ap_select(uint8_t ap_index, uint8_t bank_offset)
{
	uint32_t sel = ((uint32_t)ap_index << 24) | (bank_offset & 0xF0U);
	return nrf53_dp_write(NRF53_DP_SELECT, sel);
}

/* ------------------------------------------------------------------ */
/* AP register read/write                                             */
/*                                                                    */
/* AP reads are POSTED — the response to an AP-read packet carries    */
/* the PREVIOUS AP-read's value, not the current one's. Per ADIv5    */
/* §B4.3, the caller must flush via a DP.RDBUFF read to get the      */
/* actual value. Our public APIs do that flush internally so callers */
/* see a synchronous read.                                            */
/* ------------------------------------------------------------------ */
nrf53_status_t nrf53_ap_read(uint8_t ap_index, uint8_t reg_offset, uint32_t *out)
{
	if (out == NULL) { return NRF53_ARGS; }
	/* Select the right bank: bank 0 covers CSW/TAR/DRW; bank 0xF covers IDR. */
	uint8_t bank = reg_offset & 0xF0U;
	nrf53_status_t st = nrf53_ap_select(ap_index, bank);
	if (st != NRF53_OK) { return st; }

	uint32_t dummy = 0;
	st = ap_xfer(reg_offset & 0x0CU, true, &dummy);  /* post the read */
	if (st != NRF53_OK) { return st; }
	return nrf53_dp_read(NRF53_DP_RDBUFF, out);       /* flush */
}

nrf53_status_t nrf53_ap_write(uint8_t ap_index, uint8_t reg_offset, uint32_t val)
{
	uint8_t bank = reg_offset & 0xF0U;
	nrf53_status_t st = nrf53_ap_select(ap_index, bank);
	if (st != NRF53_OK) { return st; }
	uint32_t data = val;
	return ap_xfer(reg_offset & 0x0CU, false, &data);
}

/* ------------------------------------------------------------------ */
/* AHB-AP memory helpers                                              */
/*                                                                    */
/* Per-word TAR write (no auto-increment) per the Python reference.   */
/* Nordic chips wrap TAR at 1 KB with auto-inc enabled — easy to hit  */
/* on a 4 KB read. Per-word is ~5% slower but always correct.        */
/* ------------------------------------------------------------------ */
nrf53_status_t nrf53_mem_read(uint8_t ap_index, uint32_t csw,
			      uint32_t addr, uint32_t *out)
{
	if (out == NULL || (addr & 0x03U) != 0) { return NRF53_ARGS; }

	nrf53_status_t st;
	st = nrf53_ap_select(ap_index, 0x00U);
	if (st != NRF53_OK) { return st; }

	uint32_t data = csw;
	st = ap_xfer(NRF53_AP_CSW, false, &data);
	if (st != NRF53_OK) { return st; }

	data = addr;
	st = ap_xfer(NRF53_AP_TAR, false, &data);
	if (st != NRF53_OK) { return st; }

	uint32_t dummy = 0;
	st = ap_xfer(NRF53_AP_DRW, true, &dummy);  /* post the read */
	if (st != NRF53_OK) { return st; }

	return nrf53_dp_read(NRF53_DP_RDBUFF, out); /* flush */
}

nrf53_status_t nrf53_mem_write(uint8_t ap_index, uint32_t csw,
			       uint32_t addr, uint32_t val)
{
	if ((addr & 0x03U) != 0) { return NRF53_ARGS; }

	nrf53_status_t st;
	st = nrf53_ap_select(ap_index, 0x00U);
	if (st != NRF53_OK) { return st; }

	uint32_t data = csw;
	st = ap_xfer(NRF53_AP_CSW, false, &data);
	if (st != NRF53_OK) { return st; }

	data = addr;
	st = ap_xfer(NRF53_AP_TAR, false, &data);
	if (st != NRF53_OK) { return st; }

	data = val;
	st = ap_xfer(NRF53_AP_DRW, false, &data);
	if (st != NRF53_OK) { return st; }

	/* Flush posted-write pipeline so the next read sees the result. */
	uint32_t tmp = 0;
	(void)nrf53_dp_read(NRF53_DP_RDBUFF, &tmp);
	return NRF53_OK;
}
