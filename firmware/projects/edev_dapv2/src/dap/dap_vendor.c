/*
 * dap_vendor.c — edev_dapv2-specific DAP_VENDOR commands (0x80..0x9F).
 *
 * Each vendor command runs a fully-on-probe SWD sequence so it works
 * even when the host's higher-level DAP state machine (pyocd /
 * probe-rs) can't reach DPIDR cleanly. The host only needs to have
 * done DAP_Connect(SWD) so dap_active_port == SWD; everything from
 * there down — sticky-error clear, DP power-up, CSW/TAR/DRW dance,
 * AIRCR write, post-reset DHCSR poll — is done here on the RP2350.
 *
 * Spec-wise these are vendor extensions: implementation-defined,
 * outside the CMSIS-DAP normative surface. Unrecognised opcodes in
 * the 0x80..0x9F range fall through to a DAP_Invalid (0xFF) response.
 *
 *   0x83  EDEV_NRF_SYS_RESET — Cortex-M AIRCR SYSRESETREQ via AHB-AP.
 *                           Equivalent of `nrfjprog --reset`.
 *
 * References for the AIRCR sys-reset sequence:
 *   - ARMv8-M ARM §B5 (SCS: AIRCR, DHCSR, DEMCR)
 *   - ADIv5/v6 §6 (DP/AP register layout, transfer protocol)
 *   - pyocd cortex_m.py::_perform_reset() — open-source ground truth
 *   - Nordic libjlinkarm_nrf52_nrfjprogdll.dylib (Just_sysreset_arm)
 */

#include "dap_internal.h"

#include <stdbool.h>
#include <stdint.h>

#include "pico/time.h"

#include "hw/probe.h"

/* ----- request bit layout (mirrors dap_swd.c's TFR_*; kept local) --- */
#define R_APnDP   (1u << 0)
#define R_RnW     (1u << 1)

/* ----- DP / AP register offsets (within selected 16-byte bank) ------ */
#define DP_ABORT      0x00u
#define DP_CTRL_STAT  0x04u
#define DP_SELECT     0x08u
#define DP_RDBUFF     0x0Cu

#define AP_CSW        0x00u
#define AP_TAR        0x04u
#define AP_DRW        0x0Cu

/* ----- ARMv8-M SCS regs accessed via AHB-AP memory transactions ----- */
#define ADDR_AIRCR    0xE000ED0Cu
#define ADDR_DHCSR    0xE000EDF0u
#define ADDR_DEMCR    0xE000EDFCu

#define DHCSR_DBGKEY      (0xA05Fu << 16)
#define DHCSR_C_DEBUGEN   (1u << 0)
#define DHCSR_C_HALT      (1u << 1)
#define DHCSR_S_HALT      (1u << 17)
#define DHCSR_S_RESET_ST  (1u << 25)

#define DEMCR_VC_CORERESET (1u << 0)

#define AIRCR_VECTKEY     (0x05FAu << 16)
#define AIRCR_SYSRESETREQ (1u << 2)

/* CSW values for AHB-AP transactions. Both have standard HPROT (priv
 * data, debug) and Size=Word(2). They differ only in AddrInc:
 *
 *   CSW_WORD_NOINC = AddrInc Off    — TAR stays put; rewrite each DRW
 *   CSW_WORD_INC   = AddrInc Single — TAR auto-bumps by 4 after each DRW
 *
 * The NOINC value (0x23000052) has historically had its AddrInc bit
 * set too (matches openocd / dap_v2 v0.3.3 / probe-rs); kept for
 * compatibility — for single-word ops we rewrite TAR anyway. */
#define CSW_WORD_NOINC    0x23000052u
#define CSW_WORD_INC      0x23000012u

/* Max words per block read/write. Bounded by DAP packet size (512):
 *   mem-read response  = 3 + 4N  ≤ 512 → N ≤ 127
 *   mem-write request  = 6 + 4N  ≤ 512 → N ≤ 126
 * Cap at 64 for safety + symmetry. */
#define EDEV_MEM_MAX_WORDS  64u

/* CDBGPWRUPREQ | CSYSPWRUPREQ — request DP debug & system power-up. */
#define CTRL_PWR_REQ      0x50000000u
/* CDBGPWRUPACK | CSYSPWRUPACK — top-half nibble of CTRL/STAT once granted. */
#define CTRL_PWR_ACK      0xA0000000u

/* ----- status bytes returned to host -------------------------------- */
#define V_OK              0x00u
#define V_PORT_NOT_SWD    0x01u
#define V_DP_FAIL         0x02u  /* DP recover / power-up timed out */
#define V_AP_FAIL         0x03u  /* AHB-AP transactions FAULT (likely APPROTECT) */
#define V_RESET_TIMEOUT   0x04u  /* DHCSR.S_RESET_ST never cleared */
#define V_HALT_TIMEOUT    0x05u  /* DHCSR.S_HALT never set (reset-halt requested) */

/* ===================================================================
 *                         SWD HELPERS
 * =================================================================== */

/* WAIT-retry wrapper. Targets typically ACK within 1-2 wait cycles;
 * 64 leaves headroom for slow / sleepy chips without hanging forever. */
static uint8_t xfer(uint32_t req, uint32_t *data)
{
    uint8_t ack;
    uint16_t left = 64;
    do {
        ack = swd_transfer(req, data);
    } while (ack == DAP_TRANSFER_WAIT && left-- > 0);
    return ack;
}

static uint8_t dp_write(uint8_t addr, uint32_t value)
{
    return xfer((uint32_t)(addr & 0x0Cu), &value);
}

static uint8_t dp_read(uint8_t addr, uint32_t *out)
{
    return xfer((uint32_t)(addr & 0x0Cu) | R_RnW, out);
}

static uint8_t ap_write(uint8_t addr, uint32_t value)
{
    return xfer((uint32_t)(addr & 0x0Cu) | R_APnDP, &value);
}

/* AP read: caller MUST follow with a DP.RDBUFF read to get the value;
 * the value returned here is the previous AP read's data (pipelined). */
static uint8_t ap_read_post(uint8_t addr, uint32_t *prev_out)
{
    return xfer((uint32_t)(addr & 0x0Cu) | R_APnDP | R_RnW, prev_out);
}

/* AHB-AP single-word write. Assumes APSEL=0, APBANKSEL=0 are set. */
static uint8_t mem_w32(uint32_t addr, uint32_t value)
{
    uint8_t ack;
    ack = ap_write(AP_CSW, CSW_WORD_NOINC);
    if (ack != DAP_TRANSFER_OK) return ack;
    ack = ap_write(AP_TAR, addr);
    if (ack != DAP_TRANSFER_OK) return ack;
    return ap_write(AP_DRW, value);
}

/* AHB-AP single-word read. */
static uint8_t mem_r32(uint32_t addr, uint32_t *out)
{
    uint8_t ack;
    uint32_t scratch;
    ack = ap_write(AP_CSW, CSW_WORD_NOINC);
    if (ack != DAP_TRANSFER_OK) return ack;
    ack = ap_write(AP_TAR, addr);
    if (ack != DAP_TRANSFER_OK) return ack;
    ack = ap_read_post(AP_DRW, &scratch);     /* posted; value is stale */
    if (ack != DAP_TRANSFER_OK) return ack;
    return dp_read(DP_RDBUFF, out);           /* the actual data */
}

/* ===================================================================
 *                         DP INIT / RECOVERY
 * =================================================================== */

/* SWD line reset + idle. Spec calls for ≥50 SWCLK cycles with SWDIO
 * high, followed by ≥2 cycles SWDIO low. We use 64 + 8. Done via the
 * PIO probe_write_bits primitive (same path DAP_SWJ_Sequence uses). */
static void swj_line_reset(void)
{
    probe_write_bits(32u, 0xFFFFFFFFu);
    probe_write_bits(32u, 0xFFFFFFFFu);
    probe_write_bits(8u,  0x00u);
}

/* Bring the DP into a known good state from any prior state. Per
 * ADIv5 §B4.2.2 / §B4.3.4 the sequence MUST be:
 *
 *   1. line reset on SWDIO/SWCLK (so DP enters reset state)
 *   2. read DPIDR (mandatory first transaction after line reset —
 *      writes here are silently dropped by the target)
 *   3. write ABORT to clear sticky errors
 *   4. write CTRL/STAT to assert CDBGPWRUPREQ + CSYSPWRUPREQ
 *   5. poll CTRL/STAT for CDBGPWRUPACK + CSYSPWRUPACK
 *
 * Called twice in do_edev_nrf_sys_reset: once before the AIRCR write to
 * make sure the DP is healthy regardless of what state the host left
 * it in, and once after to recover from the reset wrecking the bus.
 *
 * Skipping the DPIDR read was the bug behind the v1 "DP_FAIL" smoke
 * test failure (2026-06-19) — OpenOCD does this read; pyocd's connect
 * path does not. Either way, our firmware must do it itself.
 */
static uint8_t dp_init(void)
{
    uint8_t ack;
    uint32_t scratch;
    uint32_t v;

    /* 1. Line reset. */
    swj_line_reset();

    /* 2. DPIDR read — DP register 0x00 reads as DPIDR (writes to 0x00
     *    are ABORT). This is the only transaction the DP will honour
     *    immediately after a line reset; the value itself we don't
     *    care about, only the ACK. */
    ack = dp_read(DP_ABORT, &scratch);
    if (ack != DAP_TRANSFER_OK) {
        return ack;
    }

    /* 3. Clear sticky errors. STKERRCLR(2) | STKCMPCLR(1) |
     *    ORUNERRCLR(4) | WDERRCLR(3) | DAPABORT(0) = 0x1F. */
    ack = dp_write(DP_ABORT, 0x1Fu);
    if (ack != DAP_TRANSFER_OK) return ack;

    /* 4. Request DP and system power-up. */
    ack = dp_write(DP_CTRL_STAT, CTRL_PWR_REQ);
    if (ack != DAP_TRANSFER_OK) return ack;

    /* 5. Poll for ACK. Typical: 1-2 iterations; we give 100 ms. */
    for (int i = 0; i < 100; i++) {
        ack = dp_read(DP_CTRL_STAT, &v);
        if (ack == DAP_TRANSFER_OK && (v & CTRL_PWR_ACK) == CTRL_PWR_ACK) {
            return DAP_TRANSFER_OK;
        }
        busy_wait_us(1000);
    }
    return DAP_TRANSFER_FAULT;
}

/* Select AHB-AP (AP[0], bank 0) via DP.SELECT. */
static uint8_t select_ahb_ap(void)
{
    return dp_write(DP_SELECT, 0x00000000u);
}

/* ===================================================================
 *           SHARED PREP — DP init + select AHB-AP
 * =================================================================== */

/* Bring DP up and select AHB-AP. Used by every memory / reset cmd. */
static uint8_t prep_for_ahb_ap(void)
{
    if (dap_active_port != EDEV_DAP_PORT_SWD) {
        return V_PORT_NOT_SWD;
    }
    if (dp_init() != DAP_TRANSFER_OK) {
        return V_DP_FAIL;
    }
    if (select_ahb_ap() != DAP_TRANSFER_OK) {
        return V_DP_FAIL;
    }
    return V_OK;
}

static inline void pack_u32_le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v       );
    p[1] = (uint8_t)(v >>  8 );
    p[2] = (uint8_t)(v >> 16 );
    p[3] = (uint8_t)(v >> 24 );
}

static inline uint32_t unpack_u32_le(const uint8_t *p)
{
    return  (uint32_t)p[0]        |
           ((uint32_t)p[1] <<  8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* ===================================================================
 *               0x86  EDEV_MEM_READ — block read via AHB-AP
 *
 * Posted-read pipeline: N AP reads + 1 DP.RDBUFF read returns N words.
 * Each AP DRW read auto-advances TAR (AddrInc=Single), so we set TAR
 * once and chain reads.
 *
 * Request:  [0x86, addr:u32_le, count:u8]
 * Response: [0x86, status:u8, count_actual:u8, data:u32_le × count_actual]
 * =================================================================== */

static uint8_t do_edev_mem_read(uint32_t addr, uint8_t count, uint8_t *out_buf,
                              uint8_t *out_count_actual)
{
    *out_count_actual = 0;
    if (count == 0) return V_OK;
    if (count > EDEV_MEM_MAX_WORDS) count = EDEV_MEM_MAX_WORDS;

    uint8_t pre = prep_for_ahb_ap();
    if (pre != V_OK) return pre;

    if (ap_write(AP_CSW, CSW_WORD_INC) != DAP_TRANSFER_OK) return V_AP_FAIL;
    if (ap_write(AP_TAR, addr)         != DAP_TRANSFER_OK) return V_AP_FAIL;

    /* First AP read initiates word[0]; the value it returns is stale (no
     * prior AP read in this batch). */
    uint32_t scratch;
    if (ap_read_post(AP_DRW, &scratch) != DAP_TRANSFER_OK) return V_AP_FAIL;

    for (uint8_t i = 0; i + 1 < count; i++) {
        uint32_t val;
        /* val = word[i]; this AP read also initiates word[i+1]. */
        if (ap_read_post(AP_DRW, &val) != DAP_TRANSFER_OK) {
            *out_count_actual = i;
            return V_AP_FAIL;
        }
        pack_u32_le(&out_buf[i * 4], val);
    }

    /* Final word: drain via DP.RDBUFF. */
    uint32_t last;
    if (dp_read(DP_RDBUFF, &last) != DAP_TRANSFER_OK) {
        *out_count_actual = (uint8_t)(count - 1);
        return V_AP_FAIL;
    }
    pack_u32_le(&out_buf[(count - 1) * 4], last);

    *out_count_actual = count;
    return V_OK;
}

/* ===================================================================
 *               0x87  EDEV_MEM_WRITE — block write via AHB-AP
 *
 * Auto-incrementing TAR: write TAR once, fire N DRW writes.
 *
 * Request:  [0x87, addr:u32_le, count:u8, data:u32_le × count]
 * Response: [0x87, status:u8, count_actual:u8]
 * =================================================================== */

static uint8_t do_edev_mem_write(uint32_t addr, uint8_t count,
                               const uint8_t *in_buf, uint8_t *out_count_actual)
{
    *out_count_actual = 0;
    if (count == 0) return V_OK;
    if (count > EDEV_MEM_MAX_WORDS) count = EDEV_MEM_MAX_WORDS;

    uint8_t pre = prep_for_ahb_ap();
    if (pre != V_OK) return pre;

    if (ap_write(AP_CSW, CSW_WORD_INC) != DAP_TRANSFER_OK) return V_AP_FAIL;
    if (ap_write(AP_TAR, addr)         != DAP_TRANSFER_OK) return V_AP_FAIL;

    for (uint8_t i = 0; i < count; i++) {
        uint32_t v = unpack_u32_le(&in_buf[i * 4]);
        if (ap_write(AP_DRW, v) != DAP_TRANSFER_OK) {
            *out_count_actual = i;
            return V_AP_FAIL;
        }
    }
    *out_count_actual = count;
    return V_OK;
}

/* ===================================================================
 *           0x88 EDEV_AP_READ / 0x89 EDEV_AP_WRITE — raw AP register
 *
 * apreg is the full 8-bit AP register address: top nibble = APBANKSEL,
 * bottom nibble = offset within bank (must be 4-byte aligned, so
 * bits 1:0 must be 0). Examples: CSW=0x00, TAR=0x04, DRW=0x0C,
 * BASE=0xF8, IDR=0xFC.
 *
 * Request (read):  [0x88, apsel:u8, apreg:u8]
 * Response (read): [0x88, status:u8, value:u32_le]
 *
 * Request (write):  [0x89, apsel:u8, apreg:u8, value:u32_le]
 * Response (write): [0x89, status:u8]
 * =================================================================== */

static uint8_t select_ap_bank(uint8_t apsel, uint8_t apreg)
{
    /* DP.SELECT: APSEL[31:24] | APBANKSEL[7:4] (zero everything else). */
    uint32_t sel = ((uint32_t)apsel << 24) | ((uint32_t)apreg & 0xF0u);
    return dp_write(DP_SELECT, sel);
}

static uint8_t do_edev_ap_read(uint8_t apsel, uint8_t apreg, uint32_t *out)
{
    if (dap_active_port != EDEV_DAP_PORT_SWD) return V_PORT_NOT_SWD;
    if (dp_init() != DAP_TRANSFER_OK)       return V_DP_FAIL;
    if (select_ap_bank(apsel, apreg) != DAP_TRANSFER_OK) return V_AP_FAIL;

    uint32_t scratch;
    /* `apreg` carries A2/A3 in bits 3:2 — ap_read_post masks to 0x0C. */
    if (ap_read_post(apreg, &scratch) != DAP_TRANSFER_OK) return V_AP_FAIL;
    if (dp_read(DP_RDBUFF, out)       != DAP_TRANSFER_OK) return V_AP_FAIL;
    return V_OK;
}

static uint8_t do_edev_ap_write(uint8_t apsel, uint8_t apreg, uint32_t value)
{
    if (dap_active_port != EDEV_DAP_PORT_SWD) return V_PORT_NOT_SWD;
    if (dp_init() != DAP_TRANSFER_OK)       return V_DP_FAIL;
    if (select_ap_bank(apsel, apreg) != DAP_TRANSFER_OK) return V_AP_FAIL;
    if (ap_write(apreg, value) != DAP_TRANSFER_OK)       return V_AP_FAIL;
    return V_OK;
}

/* ===================================================================
 *      0x8A  EDEV_CORTEX_M_DUMP — single-firmware-call combo
 *
 * Everything from dp_init through reset+halt through AP scan through
 * bulk SCB read happens in ONE vendor command. No host-side gap means
 * the chip never gets a chance to drop into deep sleep mid-sequence —
 * the structural fix for sleepy-target reliability on nRF5340 etc.
 *
 * Request:  [0x8A, reset_halt:u8, ap_count:u8 (1..8)]
 * Response (max 4 + 8*9 + 2 + 256 = 334 bytes, fits in 512-byte packet):
 *   [0]   = 0x8A echo
 *   [1]   = overall status (0=OK, else V_*)
 *   [2..5]= DPIDR (u32 LE) — 0 if dp_init failed
 *   [6]   = AHB-AP slot chosen (0..7, or 0xFF if none found)
 *   [7]   = ap_count (echoes request — number of slots scanned)
 *   [8 + 9*N + 0]: per-slot status (V_OK=0, V_AP_FAIL=3, etc.)
 *   [8 + 9*N + 1..4]: IDR (u32 LE) — 0 if status != OK
 *   [8 + 9*N + 5..8]: BASE (u32 LE) — 0 if not MEM-AP or read failed
 *   [8 + 9*ap_count + 0]: SCB block status (V_OK if all 64 words read)
 *   [8 + 9*ap_count + 1]: scb_count_actual (typically 64)
 *   [8 + 9*ap_count + 2..]: SCB words (LE, 4 bytes each, up to 64 × 4 = 256 bytes)
 * =================================================================== */

#define SCB_DUMP_BASE   0xE000ED00u
#define SCB_DUMP_WORDS  64u
#define ROM_DUMP_WORDS  32u    /* fits 32 entries + header in ~130 bytes */

static uint8_t do_edev_cortex_m_dump(bool reset_halt, uint8_t ap_count,
                                   uint8_t *resp, uint16_t resp_cap,
                                   uint16_t *resp_used)
{
    *resp_used = 0;
    if (ap_count == 0)  ap_count = 1;
    if (ap_count > 8u)  ap_count = 8u;
    /* Sanity:
     *   2 (echo+status) + 4 (DPIDR) + 1 (ahb_sel) + 1 (count) +
     *   9*ap_count + 2 (scb hdr) + 4*64 (scb) + 2 (rom hdr) + 4*32 (rom)
     *   = 8 + 9*8 + 2 + 256 + 2 + 128 = 468 bytes; well under 512. */
    if (resp_cap < (uint16_t)(8u + 9u * ap_count + 2u + 4u * SCB_DUMP_WORDS
                              + 2u + 4u * ROM_DUMP_WORDS)) {
        return V_AP_FAIL;
    }

    /* Pre-zero the variable region so partial failures don't expose junk. */
    uint8_t *p_dpidr   = &resp[2];
    uint8_t *p_ahb_sel = &resp[6];
    uint8_t *p_count   = &resp[7];
    uint8_t *p_aps     = &resp[8];
    uint8_t *p_scb_st  = &resp[8 + 9 * ap_count];
    uint8_t *p_scb_n   = &resp[9 + 9 * ap_count];
    uint8_t *p_scb     = &resp[10 + 9 * ap_count];

    pack_u32_le(p_dpidr, 0);
    *p_ahb_sel = 0xFFu;
    *p_count   = ap_count;
    for (uint8_t i = 0; i < ap_count; i++) {
        uint8_t *slot = &p_aps[i * 9];
        slot[0] = 0xFFu; /* status: not attempted */
        pack_u32_le(&slot[1], 0);
        pack_u32_le(&slot[5], 0);
    }
    *p_scb_st = 0xFFu;
    *p_scb_n  = 0u;

    /* Fixed-size scb output. */
    *resp_used = (uint16_t)(10u + 9u * ap_count + 4u * SCB_DUMP_WORDS);

    /* --- 1. Port check + DP init. */
    if (dap_active_port != EDEV_DAP_PORT_SWD) return V_PORT_NOT_SWD;
    if (dp_init() != DAP_TRANSFER_OK)        return V_DP_FAIL;

    /* --- 2. Capture DPIDR (a fresh read; DP is now powered-up). */
    {
        uint32_t dpidr;
        if (dp_read(DP_ABORT /* =0x00 reads DPIDR */, &dpidr) == DAP_TRANSFER_OK) {
            pack_u32_le(p_dpidr, dpidr);
        }
    }

    /* --- 3. Optional reset+halt. Same in-firmware loop, no host gap.
     *        Re-uses the logic from do_edev_nrf_sys_reset (inline because
     *        we want shared dp state across the whole combo). */
    if (reset_halt) {
        if (select_ahb_ap() == DAP_TRANSFER_OK) {
            /* Pre-halt the CPU. */
            (void)mem_w32(ADDR_DHCSR,
                          DHCSR_DBGKEY | DHCSR_C_HALT | DHCSR_C_DEBUGEN);
            /* Set DEMCR.VC_CORERESET so post-reset lands halted. */
            uint32_t demcr = 0;
            if (mem_r32(ADDR_DEMCR, &demcr) == DAP_TRANSFER_OK) {
                (void)mem_w32(ADDR_DEMCR, demcr | DEMCR_VC_CORERESET);
            }
            /* Fire AIRCR; ACK ignored. */
            (void)mem_w32(ADDR_AIRCR, AIRCR_VECTKEY | AIRCR_SYSRESETREQ);
            busy_wait_us(10000);
            /* Recover DP after the reset wrecks the bus. */
            (void)dp_init();
            /* Re-capture DPIDR after recovery (it may have changed
             * value if the SoC's reset cleared DP state). */
            {
                uint32_t dpidr2 = 0;
                if (dp_read(DP_ABORT, &dpidr2) == DAP_TRANSFER_OK) {
                    pack_u32_le(p_dpidr, dpidr2);
                }
            }
            /* Poll S_RESET_ST clear (chip out of reset). */
            for (int i = 0; i < 100; i++) {
                uint32_t dh = 0;
                if (mem_r32(ADDR_DHCSR, &dh) == DAP_TRANSFER_OK &&
                    !(dh & DHCSR_S_RESET_ST)) {
                    break;
                }
                busy_wait_us(1000);
            }
            /* Best-effort: clear VC_CORERESET so future resets behave. */
            if (mem_r32(ADDR_DEMCR, &demcr) == DAP_TRANSFER_OK) {
                (void)mem_w32(ADDR_DEMCR, demcr & ~DEMCR_VC_CORERESET);
            }
        }
    }

    /* --- 4. AP scan: per slot, select bank 0xF and read IDR (offset 0xFC).
     *        Capture BASE (offset 0xF8) for MEM-APs. First MEM-AP chosen
     *        as the AHB-AP for the SCB read.
     *
     *        On any failure we clear sticky errors via DP.ABORT before
     *        moving on — otherwise a single bad AP cascades to all
     *        subsequent reads (seen on locked nRF5340: AP[3] last good,
     *        AP[4-7] all FAULT, then SCB read also FAULTs without recovery). */
    uint8_t ahb_sel = 0xFFu;
    for (uint8_t apsel = 0; apsel < ap_count; apsel++) {
        uint8_t *slot = &p_aps[apsel * 9];

        if (select_ap_bank(apsel, 0xFC) != DAP_TRANSFER_OK) {
            slot[0] = V_AP_FAIL;
            (void)dp_write(DP_ABORT, 0x1Fu);   /* clear sticky before next slot */
            continue;
        }
        uint32_t junk;
        if (ap_read_post(0xFC, &junk) != DAP_TRANSFER_OK) {
            slot[0] = V_AP_FAIL;
            (void)dp_write(DP_ABORT, 0x1Fu);
            continue;
        }
        uint32_t idr;
        if (dp_read(DP_RDBUFF, &idr) != DAP_TRANSFER_OK) {
            slot[0] = V_AP_FAIL;
            (void)dp_write(DP_ABORT, 0x1Fu);
            continue;
        }
        slot[0] = V_OK;
        pack_u32_le(&slot[1], idr);

        /* If MEM-AP (class field = 8), read BASE at offset 0xF8. */
        if (idr != 0 && (((idr >> 13) & 0xFu) == 0x8u)) {
            if (select_ap_bank(apsel, 0xF8) == DAP_TRANSFER_OK) {
                uint32_t base;
                if (ap_read_post(0xF8, &junk) == DAP_TRANSFER_OK &&
                    dp_read(DP_RDBUFF, &base) == DAP_TRANSFER_OK) {
                    pack_u32_le(&slot[5], base);
                } else {
                    (void)dp_write(DP_ABORT, 0x1Fu);
                }
            } else {
                (void)dp_write(DP_ABORT, 0x1Fu);
            }
            if (ahb_sel == 0xFFu) {
                ahb_sel = apsel;
            }
        }
    }
    *p_ahb_sel = ahb_sel;

    /* Clear any residual sticky errors from AP scan before launching the
     * AHB-AP burst read. Critical for locked targets where the AP scan
     * leaves the DP with STKERR set. */
    (void)dp_write(DP_ABORT, 0x1Fu);
    /* Re-arm DP power-up (in case ABORT cleared the request bits). */
    (void)dp_write(DP_CTRL_STAT, CTRL_PWR_REQ);

    /* --- 5. Bulk SCB read via the AHB-AP (single auto-increment burst,
     *        64 words = full SCB block 0xE000ED00..0xE000EDFC). */
    if (ahb_sel != 0xFFu) {
        if (select_ahb_ap() == DAP_TRANSFER_OK &&
            ap_write(AP_CSW, CSW_WORD_INC) == DAP_TRANSFER_OK &&
            ap_write(AP_TAR, SCB_DUMP_BASE) == DAP_TRANSFER_OK) {
            /* Posted reads: first read initiates word[0], stale return. */
            uint32_t stale;
            if (ap_read_post(AP_DRW, &stale) == DAP_TRANSFER_OK) {
                uint8_t words_done = 0;
                bool ok = true;
                for (uint32_t i = 0; i + 1u < SCB_DUMP_WORDS; i++) {
                    uint32_t v;
                    if (ap_read_post(AP_DRW, &v) != DAP_TRANSFER_OK) {
                        ok = false;
                        break;
                    }
                    pack_u32_le(&p_scb[i * 4], v);
                    words_done = (uint8_t)(i + 1u);
                }
                if (ok) {
                    uint32_t last;
                    if (dp_read(DP_RDBUFF, &last) == DAP_TRANSFER_OK) {
                        pack_u32_le(&p_scb[(SCB_DUMP_WORDS - 1) * 4], last);
                        words_done = SCB_DUMP_WORDS;
                        *p_scb_st = V_OK;
                    } else {
                        *p_scb_st = V_AP_FAIL;
                    }
                } else {
                    *p_scb_st = V_AP_FAIL;
                }
                *p_scb_n = words_done;
            } else {
                *p_scb_st = V_AP_FAIL;
            }
        } else {
            *p_scb_st = V_AP_FAIL;
        }
    } else {
        *p_scb_st = V_AP_FAIL;  /* no AHB-AP available */
    }

    /* Sticky-clear before the ROM walk — same reasoning as before SCB. */
    (void)dp_write(DP_ABORT, 0x1Fu);

    /* --- 6. ROM table walk (32 entries, contiguous from AP[ahb_sel].BASE).
     *        Tacked onto the response after the SCB block. The host
     *        decodes "present" / "format" / "offset" bits. */
    uint8_t *p_rom_st = &p_scb[4u * SCB_DUMP_WORDS];
    uint8_t *p_rom_n  = p_rom_st + 1;
    uint8_t *p_rom    = p_rom_st + 2;
    *p_rom_st = 0xFFu;
    *p_rom_n  = 0u;
    *resp_used = (uint16_t)(*resp_used + 2u + 4u * ROM_DUMP_WORDS);

    if (ahb_sel != 0xFFu) {
        /* Recover BASE from the per-slot record. */
        uint32_t base = (uint32_t)p_aps[ahb_sel * 9 + 5]        |
                       ((uint32_t)p_aps[ahb_sel * 9 + 6] <<  8) |
                       ((uint32_t)p_aps[ahb_sel * 9 + 7] << 16) |
                       ((uint32_t)p_aps[ahb_sel * 9 + 8] << 24);
        uint32_t rom_base = base & 0xFFFFF000u;
        if (base != 0 && base != 0xFFFFFFFFu) {
            if (select_ahb_ap() == DAP_TRANSFER_OK &&
                ap_write(AP_CSW, CSW_WORD_INC) == DAP_TRANSFER_OK &&
                ap_write(AP_TAR, rom_base) == DAP_TRANSFER_OK) {
                uint32_t stale;
                if (ap_read_post(AP_DRW, &stale) == DAP_TRANSFER_OK) {
                    uint8_t done = 0;
                    bool ok = true;
                    for (uint32_t i = 0; i + 1u < ROM_DUMP_WORDS; i++) {
                        uint32_t v;
                        if (ap_read_post(AP_DRW, &v) != DAP_TRANSFER_OK) {
                            ok = false;
                            break;
                        }
                        pack_u32_le(&p_rom[i * 4], v);
                        done = (uint8_t)(i + 1u);
                    }
                    if (ok) {
                        uint32_t last;
                        if (dp_read(DP_RDBUFF, &last) == DAP_TRANSFER_OK) {
                            pack_u32_le(&p_rom[(ROM_DUMP_WORDS - 1) * 4], last);
                            done = ROM_DUMP_WORDS;
                            *p_rom_st = V_OK;
                        } else {
                            *p_rom_st = V_AP_FAIL;
                        }
                    } else {
                        *p_rom_st = V_AP_FAIL;
                    }
                    *p_rom_n = done;
                } else {
                    *p_rom_st = V_AP_FAIL;
                }
            } else {
                *p_rom_st = V_AP_FAIL;
            }
        } else {
            *p_rom_st = V_OK;  /* no ROM base — that's not an error */
            *p_rom_n  = 0u;
        }
    }

    return V_OK;
}

/* ===================================================================
 *                   0x83  EDEV_NRF_SYS_RESET
 * =================================================================== */

static uint8_t do_edev_nrf_sys_reset(bool halt_after_reset)
{
    uint8_t ack;
    uint32_t dhcsr;
    uint32_t demcr;

    if (dap_active_port != EDEV_DAP_PORT_SWD) {
        return V_PORT_NOT_SWD;
    }

    /* --- 1. Quiesce DP: clear sticky errors + re-assert power. --- */
    if (dp_init() != DAP_TRANSFER_OK) {
        return V_DP_FAIL;
    }
    if (select_ahb_ap() != DAP_TRANSFER_OK) {
        return V_DP_FAIL;
    }

    /* --- 2. Halt the core. A clean reset starts from halted state. --- */
    ack = mem_w32(ADDR_DHCSR,
                  DHCSR_DBGKEY | DHCSR_C_HALT | DHCSR_C_DEBUGEN);
    if (ack != DAP_TRANSFER_OK) {
        /* AHB-AP is likely locked by APPROTECT. Caller should retry
         * with DAP_CMD_EDEV_NRF_DEBUG_RESET (future) or NRF_RECOVER. */
        return V_AP_FAIL;
    }

    /* --- 3. (optional) DEMCR.VC_CORERESET so the core halts at the
     *        reset vector instead of running. --- */
    if (halt_after_reset) {
        if (mem_r32(ADDR_DEMCR, &demcr) != DAP_TRANSFER_OK) {
            return V_AP_FAIL;
        }
        if (mem_w32(ADDR_DEMCR, demcr | DEMCR_VC_CORERESET)
                != DAP_TRANSFER_OK) {
            return V_AP_FAIL;
        }
    }

    /* --- 4. The reset write. AIRCR is the magic register; the SoC's
     *        reset logic may cut the SWD transaction short — that's
     *        why pyocd / probe-rs explicitly ignore the ACK here. --- */
    (void)mem_w32(ADDR_AIRCR, AIRCR_VECTKEY | AIRCR_SYSRESETREQ);

    /* --- 5. Wait ~10 ms for the reset to propagate + the core to come
     *        back. pyocd uses the same value (reset.post_delay default). */
    busy_wait_us(10000);

    /* --- 6. The DP almost certainly has sticky errors set after the
     *        reset. Recover before any further reads. --- */
    if (dp_init() != DAP_TRANSFER_OK) {
        return V_RESET_TIMEOUT;
    }
    if (select_ahb_ap() != DAP_TRANSFER_OK) {
        return V_RESET_TIMEOUT;
    }

    /* --- 7. Poll DHCSR.S_RESET_ST until it clears (core out of reset).
     *        Generous timeout — slow targets (32 kHz clock, sleep modes)
     *        may take 100+ ms. */
    bool out_of_reset = false;
    for (int i = 0; i < 200; i++) {
        ack = mem_r32(ADDR_DHCSR, &dhcsr);
        if (ack == DAP_TRANSFER_OK && !(dhcsr & DHCSR_S_RESET_ST)) {
            out_of_reset = true;
            break;
        }
        busy_wait_us(1000);
    }
    if (!out_of_reset) {
        return V_RESET_TIMEOUT;
    }

    /* --- 8. If reset-halt was requested, confirm S_HALT and clear
     *        DEMCR.VC_CORERESET so subsequent (non-halt) resets behave
     *        normally. --- */
    if (halt_after_reset) {
        bool halted = false;
        for (int i = 0; i < 100; i++) {
            ack = mem_r32(ADDR_DHCSR, &dhcsr);
            if (ack == DAP_TRANSFER_OK && (dhcsr & DHCSR_S_HALT)) {
                halted = true;
                break;
            }
            busy_wait_us(1000);
        }
        if (!halted) {
            return V_HALT_TIMEOUT;
        }
        /* Best-effort cleanup; ignore ACK. */
        if (mem_r32(ADDR_DEMCR, &demcr) == DAP_TRANSFER_OK) {
            (void)mem_w32(ADDR_DEMCR, demcr & ~DEMCR_VC_CORERESET);
        }
    }

    return V_OK;
}

/* ===================================================================
 *                      DISPATCHER ENTRY
 * =================================================================== */

uint16_t dap_handle_vendor(const uint8_t *req, uint16_t req_avail,
                           uint8_t *resp, uint16_t resp_cap,
                           uint16_t *resp_used)
{
    if (req_avail < 1u || resp_cap < 2u) {
        *resp_used = 0;
        return 0;
    }

    const uint8_t cmd = req[0];

    switch (cmd) {

    case DAP_CMD_EDEV_NRF_SYS_RESET: {
        /* Request:  [0x83, reset_halt:u8]
         * Response: [0x83, status:u8]
         */
        if (req_avail < 2u) {
            *resp_used = 0;
            return 0;
        }
        const bool halt = (req[1] != 0u);
        resp[1] = do_edev_nrf_sys_reset(halt);
        *resp_used = 2u;
        return 2u;
    }

    case DAP_CMD_EDEV_MEM_READ: {
        /* Request:  [0x86, addr:u32_le, count:u8]                 (6 bytes)
         * Response: [0x86, status:u8, count_actual:u8, data...]   (3 + 4N bytes)
         */
        if (req_avail < 6u) { *resp_used = 0; return 0; }
        uint32_t addr = unpack_u32_le(&req[1]);
        uint8_t  count = req[5];
        if (count > EDEV_MEM_MAX_WORDS) count = EDEV_MEM_MAX_WORDS;
        if (resp_cap < (uint16_t)(3u + (uint16_t)count * 4u)) {
            /* Truncate request rather than fail — pick the largest count
             * that fits in resp_cap. */
            count = (uint8_t)((resp_cap - 3u) / 4u);
        }
        uint8_t actual = 0;
        uint8_t status = do_edev_mem_read(addr, count, &resp[3], &actual);
        resp[1] = status;
        resp[2] = actual;
        *resp_used = (uint16_t)(3u + (uint16_t)actual * 4u);
        return 6u;
    }

    case DAP_CMD_EDEV_MEM_WRITE: {
        /* Request:  [0x87, addr:u32_le, count:u8, data...]   (6 + 4N bytes)
         * Response: [0x87, status:u8, count_actual:u8]       (3 bytes)
         */
        if (req_avail < 6u || resp_cap < 3u) { *resp_used = 0; return 0; }
        uint32_t addr  = unpack_u32_le(&req[1]);
        uint8_t  count = req[5];
        if (count > EDEV_MEM_MAX_WORDS) count = EDEV_MEM_MAX_WORDS;
        uint16_t need = (uint16_t)(6u + (uint16_t)count * 4u);
        if (req_avail < need) { *resp_used = 0; return 0; }

        uint8_t actual = 0;
        uint8_t status = do_edev_mem_write(addr, count, &req[6], &actual);
        resp[1] = status;
        resp[2] = actual;
        *resp_used = 3u;
        return need;
    }

    case DAP_CMD_EDEV_AP_READ: {
        /* Request:  [0x88, apsel:u8, apreg:u8]              (3 bytes)
         * Response: [0x88, status:u8, value:u32_le]         (6 bytes)
         */
        if (req_avail < 3u || resp_cap < 6u) { *resp_used = 0; return 0; }
        uint32_t value = 0;
        uint8_t status = do_edev_ap_read(req[1], req[2], &value);
        resp[1] = status;
        pack_u32_le(&resp[2], value);
        *resp_used = 6u;
        return 3u;
    }

    case DAP_CMD_EDEV_CORTEX_M_DUMP: {
        /* Request:  [0x8A, reset_halt:u8, ap_count:u8]
         * Response: see comment block above do_edev_cortex_m_dump.
         */
        if (req_avail < 3u || resp_cap < 64u) { *resp_used = 0; return 0; }
        bool reset_halt = (req[1] != 0u);
        uint8_t ap_count = req[2];
        uint16_t used = 0;
        uint8_t st = do_edev_cortex_m_dump(reset_halt, ap_count, resp, resp_cap, &used);
        resp[1] = st;
        *resp_used = used > 0 ? used : 2u;
        return 3u;
    }

    case DAP_CMD_EDEV_AP_WRITE: {
        /* Request:  [0x89, apsel:u8, apreg:u8, value:u32_le]   (7 bytes)
         * Response: [0x89, status:u8]                          (2 bytes)
         */
        if (req_avail < 7u || resp_cap < 2u) { *resp_used = 0; return 0; }
        uint32_t value = unpack_u32_le(&req[3]);
        resp[1] = do_edev_ap_write(req[1], req[2], value);
        *resp_used = 2u;
        return 7u;
    }

    default:
        /* Unknown vendor cmd — return DAP_Invalid (0xFF echo) per spec. */
        resp[0] = 0xFFu;
        *resp_used = 1u;
        return 1u;
    }
}
