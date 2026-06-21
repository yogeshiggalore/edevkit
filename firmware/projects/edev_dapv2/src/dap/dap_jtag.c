/*
 * dap_jtag.c — JTAG protocol commands:
 *   0x14 DAP_JTAG_Sequence   — arbitrary TMS/TDI/TDO sequences
 *   0x15 DAP_JTAG_Configure  — IR length + device count
 *   0x16 DAP_JTAG_IDCODE     — read IDCODE for device N
 *
 * Bit-bang implementation lives in src/hw/jtag.c — see there for the
 * SIO-vs-PIO decision and timing notes.
 */

#include "dap_internal.h"

#include <stdbool.h>
#include <string.h>

#include "hw/jtag.h"

#define DAP_MAX_JTAG_DEVICES 8

static uint8_t s_jtag_ir_length[DAP_MAX_JTAG_DEVICES];
static uint8_t s_jtag_dev_count;

void dap_jtag_init(void)
{
    memset(s_jtag_ir_length, 0, sizeof(s_jtag_ir_length));
    s_jtag_dev_count = 0;
}

/* ------------------------------------------------------------------ */
/*  0x14 DAP_JTAG_Sequence                                             */
/* ------------------------------------------------------------------ */

uint16_t dap_handle_jtag_sequence(const uint8_t *req, uint16_t req_avail,
                                  uint8_t *resp, uint16_t resp_cap,
                                  uint16_t *resp_used)
{
    if (req_avail < 2u || resp_cap < 2u) {
        *resp_used = 0;
        return 0;
    }

    uint8_t seq_count = req[1];
    uint16_t req_idx = 2u;
    uint16_t resp_idx = 2u;

    if (dap_active_port != EDEV_DAP_PORT_JTAG) {
        /* Some hosts probe with JTAG_Sequence before connecting — be
         * lenient: still consume the bytes, but skip the actual shift
         * and return status OK. */
    }

    for (uint8_t i = 0; i < seq_count; i++) {
        if (req_idx >= req_avail) {
            *resp_used = 0;
            return 0;
        }

        uint8_t info = req[req_idx++];
        uint8_t bits = info & 0x3Fu;
        if (bits == 0u) {
            bits = 64u;
        }
        bool capture  = (info & 0x40u) != 0;
        bool tms_high = (info & 0x80u) != 0;
        uint8_t bytes = (uint8_t)((bits + 7u) / 8u);

        if ((uint16_t)(req_idx + bytes) > req_avail) {
            *resp_used = 0;
            return 0;
        }
        if (capture && (uint16_t)(resp_idx + bytes) > resp_cap) {
            *resp_used = 0;
            return 0;
        }

        /* Pack source bytes into a 64-bit LSB-first word. */
        uint64_t tdi = 0;
        for (uint8_t b = 0; b < bytes; b++) {
            tdi |= ((uint64_t)req[req_idx + b]) << (8u * b);
        }
        req_idx = (uint16_t)(req_idx + bytes);

        uint64_t tdo = 0;
        if (dap_active_port == EDEV_DAP_PORT_JTAG) {
            tdo = jtag_shift(bits, tdi, tms_high, capture);
        }

        if (capture) {
            for (uint8_t b = 0; b < bytes; b++) {
                resp[resp_idx + b] = (uint8_t)(tdo >> (8u * b));
            }
            resp_idx = (uint16_t)(resp_idx + bytes);
        }
    }

    resp[1] = 0;                          /* status OK */
    *resp_used = resp_idx;
    return req_idx;
}

/* ------------------------------------------------------------------ */
/*  0x15 DAP_JTAG_Configure                                            */
/* ------------------------------------------------------------------ */

uint16_t dap_handle_jtag_configure(const uint8_t *req, uint16_t req_avail,
                                   uint8_t *resp, uint16_t resp_cap,
                                   uint16_t *resp_used)
{
    if (req_avail < 2u || resp_cap < 2u) {
        *resp_used = 0;
        return 0;
    }
    uint8_t count = req[1];
    if (count > DAP_MAX_JTAG_DEVICES) {
        count = DAP_MAX_JTAG_DEVICES;
    }
    if (req_avail < (uint16_t)(2u + count)) {
        *resp_used = 0;
        return 0;
    }
    s_jtag_dev_count = count;
    for (uint8_t i = 0; i < count; i++) {
        s_jtag_ir_length[i] = req[2 + i];
    }
    resp[1] = 0;
    *resp_used = 2u;
    return (uint16_t)(2u + count);
}

/* ------------------------------------------------------------------ */
/*  0x16 DAP_JTAG_IDCODE                                               */
/*                                                                      */
/*  Per spec: this is a convenience command — read the IDCODE of the   */
/*  device at the given index in the chain. The host typically just    */
/*  does it via DAP_JTAG_Sequence + IR-shift + DR-shift; spec says     */
/*  implementing this command is optional. We provide a real            */
/*  implementation by walking the TAP state machine and shifting in    */
/*  the IDCODE-select instruction, then 32 bits of DR.                  */
/* ------------------------------------------------------------------ */

/* Move TAPs from any state → Run-Test/Idle via 5 TMS=1 clocks (force
 * Test-Logic-Reset) then 1 TMS=0 (→ Run-Test/Idle). */
static void tap_reset_to_idle(void)
{
    jtag_shift(5, 0, /*tms*/ true,  false);
    jtag_shift(1, 0, /*tms*/ false, false);
}

uint16_t dap_handle_jtag_idcode(const uint8_t *req, uint16_t req_avail,
                                uint8_t *resp, uint16_t resp_cap,
                                uint16_t *resp_used)
{
    if (req_avail < 2u || resp_cap < 6u) {
        *resp_used = 0;
        return 0;
    }
    uint8_t idx = req[1];

    if (dap_active_port != EDEV_DAP_PORT_JTAG ||
        idx >= s_jtag_dev_count) {
        resp[1] = DAP_TRANSFER_ERROR;
        resp[2] = 0; resp[3] = 0; resp[4] = 0; resp[5] = 0;
        *resp_used = 6u;
        return 2u;
    }

    /* All compliant TAPs reset their IR to IDCODE-select, so we can
     * skip the IR shift and go straight to DR-Shift to capture the
     * 32-bit IDCODE. The chain is daisy-chained — we must shift
     * `dev_count` IDCODEs and pick the (count - 1 - idx)-th one
     * (LSB-first chain, last device shifted first). */
    tap_reset_to_idle();

    /* TMS sequence: 0 (idle) → 1 (Select-DR-Scan) → 0 (Capture-DR) → 0 (Shift-DR) */
    jtag_shift(1, 0, true,  false);   /* idle → Select-DR */
    jtag_shift(1, 0, false, false);   /* Select-DR → Capture-DR */
    jtag_shift(1, 0, false, false);   /* Capture-DR → Shift-DR */

    /* Shift `dev_count` × 32 bits of DR — but we only care about the
     * (count - 1 - idx)-th IDCODE in the stream. To get out of Shift-DR
     * cleanly we need to assert TMS=1 on the last bit, so split the
     * shift in two: first all-but-one bits with TMS=0, then last bit
     * with TMS=1. */
    uint32_t idcode = 0;
    uint8_t target_position = (uint8_t)(s_jtag_dev_count - 1u - idx);

    for (uint8_t dev = 0; dev < s_jtag_dev_count; dev++) {
        bool last_dev   = (dev == s_jtag_dev_count - 1u);
        uint64_t shifted;
        if (!last_dev) {
            shifted = jtag_shift(32, 0xFFFFFFFFull, false, true);
        } else {
            /* Last device: hold TMS=0 for 31 bits, then TMS=1 for 1 bit
             * to leave Shift-DR. */
            shifted     = jtag_shift(31, 0xFFFFFFFFull, false, true);
            uint64_t last_bit = jtag_shift(1, 1, true, true);
            shifted    |= last_bit << 31;
        }
        if (dev == target_position) {
            idcode = (uint32_t)shifted;
        }
    }

    /* Exit-DR → Update-DR → Run-Test/Idle */
    jtag_shift(1, 0, true,  false);   /* Exit1-DR → Update-DR */
    jtag_shift(1, 0, false, false);   /* Update-DR → Run-Test/Idle */

    resp[1] = 0;                       /* status OK */
    resp[2] = (uint8_t)(idcode);
    resp[3] = (uint8_t)(idcode >> 8);
    resp[4] = (uint8_t)(idcode >> 16);
    resp[5] = (uint8_t)(idcode >> 24);
    *resp_used = 6u;
    return 2u;
}
