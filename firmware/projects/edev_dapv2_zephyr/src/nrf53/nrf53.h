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

/* ------------------------------------------------------------------ */
/* CTRL-AP layer — Nordic-specific erase/reset/recovery primitives    */
/* ------------------------------------------------------------------ */

/**
 * @brief Scan AP indices 0..7 for Nordic CTRL-APs.
 *
 * Walks each AP, reads bank 0xF's IDR, and records the index if the IDR
 * matches a known Nordic CTRL-AP value (0x02880000 or 0x12880000).
 *
 * On nRF5340 expect to find indices {2, 3} (App + Net).
 * On nRF52840 expect to find index {1}.
 *
 * @param ap_indices  Output array; filled with the found AP indices.
 * @param max         Capacity of ap_indices.
 * @param count       Out-param: number of CTRL-APs found.
 */
nrf53_status_t nrf53_ctrl_ap_scan(uint8_t *ap_indices, size_t max, size_t *count);

/**
 * @brief Defensive RESET pulse on one CTRL-AP — write 1, sleep, write 0.
 *
 * Clears any stuck CTRL-AP state before ERASEALL or other operations.
 * Per docs/NRF5340_ALGORITHMS.md §3.
 */
nrf53_status_t nrf53_ctrl_ap_reset_pulse(uint8_t ap_index);

/**
 * @brief Issue ERASEALL on one CTRL-AP and poll for completion.
 *
 * @param ap_index     The CTRL-AP index (from nrf53_ctrl_ap_scan).
 * @param timeout_ms   Max time to wait for ERASEALLSTATUS to clear.
 */
nrf53_status_t nrf53_ctrl_ap_eraseall(uint8_t ap_index, uint32_t timeout_ms);

/**
 * @brief High-level: erase every Nordic CTRL-AP found on the bus.
 *
 * Sequence (per docs/NRF5340_ALGORITHMS.md §3):
 *   1. full DP wake
 *   2. CTRL-AP IDR scan
 *   3. For each found CTRL-AP: RESET pulse → ERASEALL → poll
 *   4. DP sticky-clear + power-up at end
 *
 * Acceptance: subsequent reads of UICR via the corresponding AHB-AP
 * should return 0xFFFFFFFF.
 *
 * @param found_ap_count  Out-param (optional). Number of CTRL-APs erased.
 */
nrf53_status_t nrf53_erase_all(size_t *found_ap_count);

/* ------------------------------------------------------------------ */
/* NVMC helpers — shared between App + Net contexts                   */
/* ------------------------------------------------------------------ */

/**
 * @brief Poll NVMC.READY (bit 0) until set, with timeout.
 *
 * @param ap_index    AHB-AP index for the core whose NVMC we're polling.
 * @param csw         CSW value to use for this AP (NRF53_CSW_APP / _NET).
 * @param nvmc_base   NVMC peripheral base — see NRF53_APP/NET_NVMC_READY.
 *                    Pass the READY register address directly (already
 *                    base + 0x400) for clarity at callsite.
 * @param timeout_ms  Max time to wait.
 */
nrf53_status_t nrf53_nvmc_wait_ready(uint8_t ap_index, uint32_t csw,
				     uint32_t nvmc_ready_addr,
				     uint32_t timeout_ms);

/**
 * @brief Write NVMC.CONFIG and wait for READY.
 *
 * @param ap_index    AHB-AP index.
 * @param csw         CSW value for this AP.
 * @param nvmc_ready_addr  NVMC.READY address (App/Net).
 * @param nvmc_config_addr NVMC.CONFIG address (App/Net).
 * @param mode        NRF53_NVMC_CONFIG_REN / _WEN / _EEN / _PEEN.
 * @param timeout_ms  Wait timeout after the write.
 */
nrf53_status_t nrf53_nvmc_set_config(uint8_t ap_index, uint32_t csw,
				     uint32_t nvmc_ready_addr,
				     uint32_t nvmc_config_addr,
				     uint32_t mode,
				     uint32_t timeout_ms);

/* ------------------------------------------------------------------ */
/* UICR programming — App core (host-side NVMC writes, no stub)       */
/* ------------------------------------------------------------------ */

/**
 * @brief Program App UICR.APPROTECT + SECUREAPPROTECT = 0x50FA50FA.
 *
 * Per docs/NRF5340_ALGORITHMS.md §4 Stage 3. Uses App AHB-AP + App
 * NVMC; no on-target stub needed (the App core's secure context lets
 * the host write its secure UICR directly). For Net UICR, use the
 * on-target stub flow at nrf53_uicr_program_net() instead.
 *
 * Preconditions:
 *   - DP is alive (host has at least called dap_connect + line reset)
 *   - Caller has cleared sticky bits / powered up the DP recently
 *   - Target App core is reachable (CTRL-AP RESET has been released)
 *
 * @param out_approtect       (optional) Readback of UICR.APPROTECT
 *                            after programming.
 * @param out_secureapprotect (optional) Readback of UICR.SECUREAPPROTECT
 *                            after programming.
 */
nrf53_status_t nrf53_uicr_program_app(uint32_t *out_approtect,
				      uint32_t *out_secureapprotect);

/* ------------------------------------------------------------------ */
/* UICR programming — Net core (on-target stub)                       */
/* ------------------------------------------------------------------ */

/**
 * @brief Program Net UICR.APPROTECT = 0x50FA50FA via on-target stub.
 *
 * Sequence (per docs/NRF5340_ALGORITHMS.md §4 Stages 4–8):
 *   1. dp_sticky_clear + dp_power_up (fix for bug 6b — Net AHB-AP
 *      fault mid-flow)
 *   2. Release Net core: App-side write of 0 to RESET.NETWORK.FORCEOFF
 *   3. Write the embedded stub (~196 B) into Net flash[0x01000000]
 *      via Net AHB-AP + Net NVMC (Wen → per-word write → Ren).
 *   4. Pulse CTRL-AP#3 RESET to boot the Net core into the stub.
 *   5. Wait for the stub's 0xDEADC0DE SRAM marker at 0x21000000.
 *   6. Read back Net UICR.APPROTECT for verification.
 *
 * Preconditions:
 *   - CTRL-AP#3 ERASEALL has run recently (Net AHB-AP unlocked)
 *   - Caller has cleared sticky bits / powered up the DP
 *   - App core already accessible (we need it to release FORCEOFF)
 *
 * @param out_marker     (optional) Final value at SRAM[0x21000000].
 *                       Expect 0xDEADC0DE on success.
 * @param out_approtect  (optional) Net UICR.APPROTECT readback.
 */
nrf53_status_t nrf53_uicr_program_net(uint32_t *out_marker,
				      uint32_t *out_approtect);

/* Size of the embedded Net stub blob (bytes). */
extern const uint16_t nrf53_net_stub_size;
extern const uint8_t  nrf53_net_stub[];

/* ------------------------------------------------------------------ */
/* RECOVER — full 8-stage unlock + UICR programming                   */
/* ------------------------------------------------------------------ */

/**
 * @brief Diagnostic info collected during a recover run.
 *
 * Filled regardless of overall success/failure so the host can see how
 * far the recover got and what state the chip ended up in. Fields not
 * yet observed default to 0xFFFFFFFF (or 0 for ap_count).
 */
struct nrf53_recover_info {
	uint8_t  ap_count;         /* Number of CTRL-APs found by the IDR scan. */
	uint32_t app_approtect;    /* App UICR.APPROTECT readback. */
	uint32_t app_secureapprotect;
	uint32_t net_marker;       /* Net SRAM[0x21000000] readback. */
	uint32_t net_approtect;    /* Net UICR.APPROTECT readback. */
};

/* ------------------------------------------------------------------ */
/* Memory read — chip-agnostic AHB-AP burst read                      */
/* ------------------------------------------------------------------ */

/* Flags for nrf53_read_mem.flags */
#define NRF53_READ_FLAG_VC_CORERESET_CLEAR  0x01U
   /* Clear DEMCR.VC_CORERESET (bit 0) + sleep 50 ms BEFORE the read.
    * Fix for Bug 6a (App readback 0xFF post-reset on nRF5340). Pass
    * this flag when reading nRF5340 App flash right after a CTRL-AP
    * RESET; ignore on nRF52 (DEMCR is the same on all Cortex-M cores
    * so the write is harmless, but the post-reset condition only
    * applies to nRF5340). */

/**
 * @brief Read N contiguous 32-bit words from AHB-AP memory.
 *
 * Chip-agnostic — works on any ARM Cortex-M via the AHB-AP. The
 * caller provides the AP index, CSW value, base address, and word
 * count; the function uses per-word TAR writes (no auto-increment)
 * to avoid Nordic's 1-KB TAR-wrap quirk.
 *
 * The output buffer must be at least `word_count * 4` bytes. Each
 * word is packed little-endian into the output buffer.
 *
 * @param ap_index     AHB-AP index (0 for App on nRF52/nRF5340).
 * @param csw          CSW for this AP (NRF53_CSW_APP / _NET, or
 *                     vendor-specific). For nRF52840 use
 *                     NRF53_CSW_APP (0x23000002).
 * @param addr         Base address; must be 4-byte aligned.
 * @param word_count   Number of 32-bit words to read.
 * @param flags        NRF53_READ_FLAG_* bitmask.
 * @param out          Output buffer (word_count * 4 bytes).
 */
nrf53_status_t nrf53_read_mem(uint8_t ap_index, uint32_t csw,
			      uint32_t addr, uint32_t word_count,
			      uint32_t flags, uint8_t *out);

/**
 * @brief Write N contiguous 32-bit words to AHB-AP memory.
 *
 * Counterpart to nrf53_read_mem. Chip-agnostic — works on any ARM
 * Cortex-M via the AHB-AP. Per-word TAR writes (no auto-increment) to
 * avoid Nordic's 1-KB TAR-wrap quirk.
 *
 * The input buffer must be at least `word_count * 4` bytes, each word
 * encoded little-endian.
 *
 * @param ap_index     AHB-AP index.
 * @param csw          CSW for this AP.
 * @param addr         Base address; must be 4-byte aligned.
 * @param word_count   Number of 32-bit words to write.
 * @param flags        Reserved for future use; pass 0.
 * @param data         Source buffer (word_count * 4 bytes).
 */
nrf53_status_t nrf53_write_mem(uint8_t ap_index, uint32_t csw,
			       uint32_t addr, uint32_t word_count,
			       uint32_t flags, const uint8_t *data);

/* ------------------------------------------------------------------ */
/* Flash write — Net core (direct AP#1 + Net NVMC)                   */
/* ------------------------------------------------------------------ */

/**
 * @brief Program N contiguous 32-bit words to Net flash.
 *
 * Wraps the NVMC programming sequence (Wen → per-word write+READY →
 * Ren) into a single probe-side operation. Caller provides the Net
 * flash destination address + the data. Words equal to 0xFFFFFFFF are
 * skipped (matches erased state, saves time).
 *
 * Preconditions:
 *   - Target page(s) already erased (typically via CTRL-AP#3 ERASEALL
 *     in NRF53_ERASE)
 *   - Net AHB-AP is reachable (DP/AP layer initialized)
 *
 * @param addr               Net flash destination address (4-byte aligned;
 *                           typically in range 0x01000000 - 0x0103FFFF).
 * @param word_count         Number of 32-bit words to write.
 * @param data               Source buffer (word_count * 4 bytes, LE words).
 * @param out_words_written  Out-param: words actually written before any
 *                           failure. On full success equals word_count.
 */
nrf53_status_t nrf53_flash_write_net(uint32_t addr, uint32_t word_count,
				     const uint8_t *data,
				     uint32_t *out_words_written);

/**
 * @brief Program N contiguous 32-bit words to App flash.
 *
 * Same NVMC programming dance as `nrf53_flash_write_net()` but on
 * App AHB-AP (ap=0, csw=0x23000002). The chip's NVMC base address
 * is a parameter to support multiple Nordic families:
 *   - nRF52840 / nRF52833 / nRF52832: NVMC at 0x4001E000
 *   - nRF5340 App:                    NVMC at 0x50039000
 *
 * Words equal to 0xFFFFFFFF are skipped (matches erased state).
 *
 * @param nvmc_base          NVMC peripheral base for the target's App
 *                           core. App NVMC.READY  = nvmc_base + 0x400,
 *                           NVMC.CONFIG = nvmc_base + 0x504.
 * @param addr               App flash destination (4-byte aligned).
 * @param word_count         Number of 32-bit words to write.
 * @param data               Source buffer (word_count * 4 bytes, LE words).
 * @param out_words_written  Out-param: words actually written before any
 *                           failure (= word_count on full success).
 */
nrf53_status_t nrf53_flash_write_app(uint32_t nvmc_base, uint32_t addr,
				     uint32_t word_count, const uint8_t *data,
				     uint32_t *out_words_written);

/**
 * @brief Full recover flow: ERASE both cores + program App + Net UICR.
 *
 * Composes the lower-level operations from steps 2-4 into a single
 * end-to-end unlock sequence, matching the Python reference's
 * `_recover_sync()`. Per docs/NRF5340_ALGORITHMS.md §4.
 *
 * Sequence:
 *   1. nrf53_erase_all      — CTRL-AP IDR scan + ERASEALL each
 *                             (also does dp_full_wake at start)
 *   2. nrf53_uicr_program_app
 *   3. nrf53_uicr_program_net
 *
 * Acceptance: post-recover, both cores' UICR.APPROTECT readbacks
 * = 0x50FA50FA and the Net SRAM marker = 0xDEADC0DE.
 *
 * @param info  Out-param (optional). Always filled — even on failure
 *              so the host can see partial state.
 */
nrf53_status_t nrf53_recover(struct nrf53_recover_info *info);

/* ------------------------------------------------------------------ */
/* CMSIS-DAP vendor command IDs (0x80..0x9F)                          */
/* ------------------------------------------------------------------ */

/* Reserved / not implemented yet on Zephyr (some are pico-sdk legacy) */
#define NRF53_VENDOR_CORTEX_M_HALT       0x80U  /* (reserved) */
#define NRF53_VENDOR_CORTEX_M_RESUME     0x81U  /* (reserved) */
#define NRF53_VENDOR_CORTEX_M_REG_READ   0x82U  /* (reserved) */
#define NRF53_VENDOR_NRF_RESET           0x83U  /* (reserved) */

/* Edevkit nRF5340 multi-step ops (implemented incrementally; see
 * docs/NRF5340_ALGORITHMS.md §9 implementation checklist) */
#define NRF53_VENDOR_RECOVER             0x84U  /* step 5 — full unlock + UICR */
#define NRF53_VENDOR_ERASE               0x85U  /* step 2 — CTRL-AP ERASEALL */
#define NRF53_VENDOR_FLASH_WRITE_NET     0x86U  /* step 6 — Net flash write */
#define NRF53_VENDOR_FLASH_WRITE_APP     0x87U  /* step 7 — App flash write */
#define NRF53_VENDOR_READ_MEM            0x88U  /* step 8 */
#define NRF53_VENDOR_TARGET_INFO         0x89U  /* later */
#define NRF53_VENDOR_UICR_PROGRAM_APP    0x8AU  /* step 3 */
#define NRF53_VENDOR_UICR_PROGRAM_NET    0x8BU  /* step 4 */
#define NRF53_VENDOR_WRITE_MEM           0x8CU  /* AHB-AP burst write */

#ifdef __cplusplus
}
#endif

#endif /* EDEV_NRF53_H_ */
