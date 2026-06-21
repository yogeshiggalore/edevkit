/*
 * dap_swj.c — SWJ commands (shared between SWD and JTAG):
 *   0x10 DAP_SWJ_Pins      — read / write SWCLK / SWDIO / nRESET directly
 *   0x11 DAP_SWJ_Clock     — set SWCLK / TCK frequency
 *   0x12 DAP_SWJ_Sequence  — bit-bang an arbitrary 1..256-bit sequence
 *
 * SWJ_Pins is how hosts implement system reset, dormant-state wake-up,
 * and probe-of-life checks. SWJ_Sequence is the JTAG-to-SWD switch
 * (16-bit magic `0xE79E`, sandwiched between two ≥50-bit line resets).
 */

#include "dap_internal.h"

#include <string.h>

#include "pico/time.h"

#include "hw/jtag.h"
#include "hw/probe.h"

/* Module state — used by other handlers via extern in dap_internal.h. */
uint32_t dap_swj_clock_hz = 1000000u;

void dap_swj_init(void)
{
    dap_swj_clock_hz = 1000000u;
}

/* ------------------------------------------------------------------ */
/*  0x10 DAP_SWJ_Pins                                                  */
/* ------------------------------------------------------------------ */

/* Pin layout per spec — bit positions in the 1-byte values/select
 * fields. Bits 4 (TDI) and 5 (TDO) are JTAG-only; we treat them as
 * no-op since for SWD they're idle pins. */
#define SWJ_PIN_SWCLK   (1u << 0)
#define SWJ_PIN_SWDIO   (1u << 1)
#define SWJ_PIN_TDI     (1u << 2)
#define SWJ_PIN_TDO     (1u << 3)
#define SWJ_PIN_NRESET  (1u << 7)

uint16_t dap_handle_swj_pins(const uint8_t *req, uint16_t req_avail,
                             uint8_t *resp, uint16_t resp_cap,
                             uint16_t *resp_used)
{
    if (req_avail < 7u || resp_cap < 2u) {
        *resp_used = 0;
        return 0;
    }
    uint8_t  value  = req[1];
    uint8_t  select = req[2];
    uint32_t wait_us = (uint32_t)req[3] |
                       ((uint32_t)req[4] << 8) |
                       ((uint32_t)req[5] << 16) |
                       ((uint32_t)req[6] << 24);

    /* Apply selected drive states. The "value" bit per pin = the level
     * to drive when that pin's select bit is 1. */
    if (select & SWJ_PIN_SWCLK)  probe_drive_swclk ((value & SWJ_PIN_SWCLK)  != 0);
    if (select & SWJ_PIN_SWDIO)  probe_drive_swdio ((value & SWJ_PIN_SWDIO)  != 0);
    if (select & SWJ_PIN_NRESET) probe_drive_nreset((value & SWJ_PIN_NRESET) != 0);
    /* TDI/TDO: M6 — JTAG-only pins, no-op for v0.1. */

    /* Spec caps the wait at 3 s to keep the host responsive. */
    if (wait_us > 3000000u) {
        wait_us = 3000000u;
    }
    if (wait_us > 0u) {
        busy_wait_us(wait_us);
    }

    /* Read back the current pin states for the response. */
    uint8_t readback = 0;
    if (probe_read_swclk ()) readback |= SWJ_PIN_SWCLK;
    if (probe_read_swdio ()) readback |= SWJ_PIN_SWDIO;
    if (probe_read_nreset()) readback |= SWJ_PIN_NRESET;

    resp[1] = readback;
    *resp_used = 2u;
    return 7u;
}

/* ------------------------------------------------------------------ */
/*  0x11 DAP_SWJ_Clock                                                 */
/* ------------------------------------------------------------------ */

uint16_t dap_handle_swj_clock(const uint8_t *req, uint16_t req_avail,
                              uint8_t *resp, uint16_t resp_cap,
                              uint16_t *resp_used)
{
    if (req_avail < 5u || resp_cap < 2u) {
        *resp_used = 0;
        return 0;
    }
    dap_swj_clock_hz = (uint32_t)req[1] |
                       ((uint32_t)req[2] << 8) |
                       ((uint32_t)req[3] << 16) |
                       ((uint32_t)req[4] << 24);

    /* Spec floor: hosts may legitimately ask for clock < 1 kHz (e.g. to
     * debug slow targets). Below the PIO's achievable minimum we just
     * round to the slowest divider. */
    uint32_t freq_khz = dap_swj_clock_hz / 1000u;
    if (freq_khz == 0u) {
        freq_khz = 1u;
    }
    /* Both SWD (via PIO) and JTAG (via SIO bit-bang) honour the same
     * DAP_SWJ_Clock request; we configure both so a mid-session
     * Connect(SWD)→Connect(JTAG) doesn't pick up the wrong rate. */
    probe_set_swclk_freq_khz(freq_khz);
    jtag_set_tck_freq_khz   (freq_khz);

    resp[1] = 0;
    *resp_used = 2u;
    return 5u;
}

/* ------------------------------------------------------------------ */
/*  0x12 DAP_SWJ_Sequence                                              */
/* ------------------------------------------------------------------ */

uint16_t dap_handle_swj_sequence(const uint8_t *req, uint16_t req_avail,
                                 uint8_t *resp, uint16_t resp_cap,
                                 uint16_t *resp_used)
{
    if (req_avail < 2u || resp_cap < 2u) {
        *resp_used = 0;
        return 0;
    }
    uint16_t bit_count = req[1];
    if (bit_count == 0u) {
        bit_count = 256u;
    }
    uint16_t byte_count = (uint16_t)((bit_count + 7u) / 8u);
    if (req_avail < (uint16_t)(2u + byte_count)) {
        *resp_used = 0;
        return 0;
    }

    /* Bit-bang the sequence. The PIO can take up to 32 bits per
     * write — chunk anything longer. probe_write_bits with drive=true
     * also takes care of making sure PIO owns the pins (if a recent
     * DAP_SWJ_Pins call had stolen them). */
    const uint8_t *p = &req[2];
    uint16_t remaining = bit_count;
    while (remaining > 0u) {
        uint32_t chunk_bits = remaining > 32u ? 32u : remaining;
        /* Pack the next 1..4 source bytes into a 32-bit word, LSB-first
         * (the wire order). */
        uint32_t word = 0;
        uint32_t bytes_in_chunk = (chunk_bits + 7u) / 8u;
        for (uint32_t i = 0; i < bytes_in_chunk; i++) {
            word |= (uint32_t)(*p++) << (8u * i);
        }
        probe_write_bits(chunk_bits, word);
        remaining = (uint16_t)(remaining - chunk_bits);
    }

    resp[1] = 0;
    *resp_used = 2u;
    return (uint16_t)(2u + byte_count);
}
