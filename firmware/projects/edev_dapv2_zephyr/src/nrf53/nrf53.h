/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * nrf53.h — nRF5340 (and nRF52) recover / erase / write / read primitives.
 *
 * This module sits ABOVE Zephyr's SWDP driver (drivers/dp/swdp_*.c) and
 * BESIDE the CMSIS-DAP dispatcher (subsys/dap/cmsis_dap.c). It builds the
 * DP / AP / CTRL-AP transactions needed for Nordic chip recovery and
 * flash programming.
 *
 * Algorithm reference (the spec this code implements):
 *   docs/NRF5340_ALGORITHMS.md
 *
 * Concurrency model:
 *   The CMSIS-DAP dispatcher is synchronous — only one DAP packet (whether
 *   a standard DAP_Transfer or one of our custom vendor commands at
 *   0x84..0x8B) is processed at a time. So our handlers can call SWDP
 *   driver APIs directly without locking, as long as we always re-write
 *   DP.SELECT (and AP.CSW) before any AP read/write — never trust cached
 *   state across calls.
 */
#ifndef EDEV_NRF53_H_
#define EDEV_NRF53_H_

#include <stdint.h>
#include <stddef.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Status codes                                                       */
/* ------------------------------------------------------------------ */
typedef enum {
	NRF53_OK         = 0,
	NRF53_WAIT       = 1,   /* SWD ACK = WAIT after retry budget exhausted */
	NRF53_FAULT      = 2,   /* SWD ACK = FAULT (sticky bit set on target) */
	NRF53_NO_ACK     = 3,   /* SWD ACK = NO_ACK (line dead or target off) */
	NRF53_PROTO      = 4,   /* Parity / framing error from driver */
	NRF53_TIMEOUT    = 5,   /* Poll loop timeout (NVMC.READY, ERASEALL, etc.) */
	NRF53_ARGS       = 6,   /* Caller passed a bogus address / length */
	NRF53_NO_DEV     = 7,   /* No SWDP device bound — call nrf53_bind_swdp() */
	NRF53_STUB_FAIL  = 8,   /* On-target stub didn't write the success marker */
} nrf53_status_t;

const char *nrf53_status_str(nrf53_status_t s);

/* ------------------------------------------------------------------ */
/* Register addresses                                                 */
/* ------------------------------------------------------------------ */

/* DP register slots (within bank 0). The DP has 4 register slots; the
 * meaning depends on whether you're reading or writing. */
#define NRF53_DP_ABORT     0x00U   /* W: clear sticky bits */
#define NRF53_DP_DPIDR     0x00U   /* R: DP identification */
#define NRF53_DP_CTRL_STAT 0x04U   /* RW: power/reset control */
#define NRF53_DP_SELECT    0x08U   /* W: AP bank + AP index */
#define NRF53_DP_RDBUFF    0x0CU   /* R: read buffer (flushes posted-AP-read) */

/* Standard MEM-AP register offsets (within bank 0) */
#define NRF53_AP_CSW       0x00U
#define NRF53_AP_TAR       0x04U
#define NRF53_AP_DRW       0x0CU
#define NRF53_AP_IDR       0xFCU   /* bank 0xF — caller writes SELECT first */

/* Nordic CTRL-AP register offsets (within bank 0) */
#define NRF53_CTRL_AP_RESET            0x00U
#define NRF53_CTRL_AP_ERASEALL         0x04U
#define NRF53_CTRL_AP_ERASEALLSTATUS   0x08U
#define NRF53_CTRL_AP_APPROTECTSTATUS  0x10U

/* AP indices on nRF5340 */
#define NRF53_AP_APP        0U   /* App core AHB-AP */
#define NRF53_AP_NET        1U   /* Net core AHB-AP */
#define NRF53_CTRL_AP_APP   2U   /* App core CTRL-AP */
#define NRF53_CTRL_AP_NET   3U   /* Net core CTRL-AP */

/* CSW values (Nordic-specific; from J-Link wire traces) */
#define NRF53_CSW_APP   0x23000002U   /* HPROT=secure/priv, 32-bit, no auto-inc */
#define NRF53_CSW_NET   0x03800042U   /* Net-specific; auto-inc OFF, device EN */

/* Nordic CTRL-AP IDR values (used to discover CTRL-AP indices) */
#define NRF53_IDR_CTRL_AP_A   0x02880000U
#define NRF53_IDR_CTRL_AP_B   0x12880000U

/* DP.ABORT sticky-clear mask */
#define NRF53_ABORT_STICKY_ALL   0x0000001EU
   /* STKCMPCLR (bit1) | STKERRCLR (bit2) | WDERRCLR (bit3) | ORUNERRCLR (bit4) */

/* DP.CTRL/STAT power-up request + ack masks */
#define NRF53_CTRL_PWRUP_REQ   0x50000000U   /* CSYSPWRUPREQ | CDBGPWRUPREQ (lower bits 28+30) */
#define NRF53_CTRL_PWRUP_ACK   0xA0000000U   /* CSYSPWRUPACK | CDBGPWRUPACK */

/* Magic values */
#define NRF53_UNLOCK_VAL       0x50FA50FAU   /* "HwDisabled" — UICR.APPROTECT */
#define NRF53_STUB_DONE_MAGIC  0xDEADC0DEU   /* Net stub writes this on success */
#define NRF53_STUB_FAULT_MAGIC 0xBADF00D5U   /* Net stub writes this on fault */

/* nRF5340 peripheral addresses */
#define NRF53_APP_NVMC_READY   0x50039400U
#define NRF53_APP_NVMC_CONFIG  0x50039504U
#define NRF53_NET_NVMC_READY   0x41080400U
#define NRF53_NET_NVMC_CONFIG  0x41080504U
#define NRF53_APP_UICR_APPROTECT        0x00FF8000U
#define NRF53_APP_UICR_SECUREAPPROTECT  0x00FF801CU
#define NRF53_NET_UICR_APPROTECT        0x01FF8000U
#define NRF53_RESET_NETWORK_FORCEOFF    0x50005614U
#define NRF53_NET_SRAM_MARKER           0x21000000U

/* Cortex-M debug registers (in App AP MEM space) */
#define NRF53_DEMCR            0xE000EDFCU   /* bit 0 = VC_CORERESET */
#define NRF53_DEMCR_VC_CORERESET 0x00000001U

/* NVMC.CONFIG modes */
#define NRF53_NVMC_CONFIG_REN   0U   /* read-only */
#define NRF53_NVMC_CONFIG_WEN   1U   /* write enabled */
#define NRF53_NVMC_CONFIG_EEN   2U   /* erase enabled */
#define NRF53_NVMC_CONFIG_PEEN  4U   /* partial erase enabled */

/* ------------------------------------------------------------------ */
/* Initialization                                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief Bind the SWDP driver our DP/AP transactions go through.
 *
 * The caller is expected to pass `DEVICE_DT_GET_ONE(zephyr_swdp_gpio)`
 * (or whatever DT compatible the SWDP node uses on this board). Must be
 * called once at boot before any other nrf53_* call.
 *
 * @return NRF53_OK if the device is ready; NRF53_NO_DEV otherwise.
 */
nrf53_status_t nrf53_bind_swdp(const struct device *swdp_dev);

/* ------------------------------------------------------------------ */
/* DP layer — raw transactions + meta-helpers                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Read a DP register (one of ABORT/CTRL_STAT/SELECT/RDBUFF).
 *
 * @param reg_offset NRF53_DP_* register offset (0x00, 0x04, 0x08, 0x0C).
 * @param out        Where to store the 32-bit result.
 * @return NRF53_OK or an NRF53_* error.
 */
nrf53_status_t nrf53_dp_read(uint8_t reg_offset, uint32_t *out);

/**
 * @brief Write a DP register.
 */
nrf53_status_t nrf53_dp_write(uint8_t reg_offset, uint32_t val);

/**
 * @brief Clear all DP sticky-error bits (DP.ABORT ← 0x0000001E).
 *
 * Call this between CTRL-AP operations and any sustained AHB-AP work
 * (Bug 6b in docs/NRF5340_ALGORITHMS.md).
 */
nrf53_status_t nrf53_dp_sticky_clear(void);

/**
 * @brief Request CSYSPWRUPREQ + CDBGPWRUPREQ and poll for ACK.
 *
 * @param timeout_ms  Poll timeout. 100 ms is usually plenty.
 */
nrf53_status_t nrf53_dp_power_up(uint32_t timeout_ms);

/**
 * @brief Full DP wake — J-Link CORESIGHT_Configure equivalent.
 *
 * Sequence (per docs/NRF5340_ALGORITHMS.md §2):
 *   1. SWD line reset (56 SWCLK with SWDIO high)
 *   2. JTAG→SWD switch (16-bit 0xE79E)
 *   3. SWD dormant-wake alert (148 bits)
 *   4. SWD line reset (56 SWCLK)
 *   5. DPIDR read   ← ADIv5 §B4.2.2: MUST be the first xact post-reset
 *   6. DP.ABORT sticky clear
 *   7. DP.CTRL/STAT power-up
 *
 * @param dpidr_out Optional pointer; receives the DPIDR value if non-NULL.
 */
nrf53_status_t nrf53_dp_full_wake(uint32_t *dpidr_out);

/* ------------------------------------------------------------------ */
/* AP layer — caller-managed SELECT                                   */
/* ------------------------------------------------------------------ */

/**
 * @brief Write DP.SELECT to point at (ap_index, bank).
 *
 * Convenience wrapper around nrf53_dp_write(NRF53_DP_SELECT, ...).
 */
nrf53_status_t nrf53_ap_select(uint8_t ap_index, uint8_t bank_offset);

/**
 * @brief Read an AP register.
 *
 * Re-issues SELECT first, then reads. Caller must use the correct AP
 * bank for the register (bank 0 for CSW/TAR/DRW, bank 0xF for IDR).
 *
 * @param ap_index    AP index (0..15).
 * @param reg_offset  NRF53_AP_* register offset within the bank.
 * @param out         Where to store the 32-bit result.
 */
nrf53_status_t nrf53_ap_read(uint8_t ap_index, uint8_t reg_offset, uint32_t *out);

/**
 * @brief Write an AP register.
 *
 * Re-issues SELECT first, then writes.
 */
nrf53_status_t nrf53_ap_write(uint8_t ap_index, uint8_t reg_offset, uint32_t val);

/* ------------------------------------------------------------------ */
/* Memory-access helpers via an AHB-AP                                */
/* ------------------------------------------------------------------ */

/**
 * @brief Read one 32-bit word from `addr` via the given AHB-AP.
 *
 * Sets DP.SELECT, AP.CSW, AP.TAR, then reads AP.DRW → DP.RDBUFF.
 * Per-word TAR write (no auto-increment) — works around Nordic's 1 KB
 * TAR-wrap quirk and is what the Python reference does.
 *
 * @param ap_index  NRF53_AP_APP or NRF53_AP_NET.
 * @param csw       NRF53_CSW_APP or NRF53_CSW_NET (or another for non-Nordic).
 * @param addr      Target address (4-byte aligned).
 * @param out       Where to store the 32-bit value.
 */
nrf53_status_t nrf53_mem_read(uint8_t ap_index, uint32_t csw,
			      uint32_t addr, uint32_t *out);

/**
 * @brief Write one 32-bit word to `addr` via the given AHB-AP.
 */
nrf53_status_t nrf53_mem_write(uint8_t ap_index, uint32_t csw,
			       uint32_t addr, uint32_t val);

#ifdef __cplusplus
}
#endif

#endif /* EDEV_NRF53_H_ */
