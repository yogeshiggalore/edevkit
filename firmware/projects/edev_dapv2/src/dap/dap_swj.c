/*
 * dap_swj.c — DAP_SWJ_Pins, _Clock, _Sequence.
 *
 * These are pin/clock-level operations the host uses to drive the
 * physical wires directly (e.g. JTAG-to-SWD switch, dormant wakeup,
 * line reset). All three are essential for host-side ADIv5 state-
 * machine drivers (probe-rs, pyocd, OpenOCD).
 */

#include "dap/dap_internal.h"
#include "hw/probe.h"
#include "util/log.h"

#include "pico/stdlib.h"

uint16_t dap_handle_swj_pins(const uint8_t *req, uint16_t req_len,
                             uint8_t *resp, uint16_t resp_cap)
{
    if (req_len < 6 || resp_cap < 1) return 0;

    /* Request:
     *   req[0]    pin_output (bitmask)
     *   req[1]    pin_select (bitmask)
     *   req[2..5] wait_us (u32, LE)
     *
     * Pin bits per CMSIS-DAP spec:
     *   0 SWCLK / TCK    1 SWDIO / TMS    2 TDI    3 TDO
     *   5 nTRST          7 nRESET
     *
     * Response: pin_input (bitmask of current pin states).
     */
    uint8_t pin_output = req[0];
    uint8_t pin_select = req[1];
    uint32_t wait_us   = (uint32_t)req[2]
                       | ((uint32_t)req[3] <<  8)
                       | ((uint32_t)req[4] << 16)
                       | ((uint32_t)req[5] << 24);

    probe_swj_set_pins(pin_select, pin_output);

    if (wait_us > 3000000u) wait_us = 3000000u;   /* spec cap: 3 s */
    if (wait_us > 0u) sleep_us(wait_us);

    resp[0] = probe_swj_get_pins();
    return 1;
}

uint16_t dap_handle_swj_clock(const uint8_t *req, uint16_t req_len,
                              uint8_t *resp, uint16_t resp_cap)
{
    if (req_len < 4 || resp_cap < 1) return 0;
    uint32_t hz = (uint32_t)req[0]
                | ((uint32_t)req[1] <<  8)
                | ((uint32_t)req[2] << 16)
                | ((uint32_t)req[3] << 24);
    probe_set_clock(hz);
    resp[0] = DAP_OK;
    return 1;
}

uint16_t dap_handle_swj_seq(const uint8_t *req, uint16_t req_len,
                            uint8_t *resp, uint16_t resp_cap)
{
    if (req_len < 1 || resp_cap < 1) return 0;
    uint16_t bit_count = req[0];
    if (bit_count == 0) bit_count = 256;
    uint16_t byte_count = (uint16_t) ((bit_count + 7u) / 8u);
    if (req_len < 1u + byte_count) {
        resp[0] = DAP_ERROR;
        return 1;
    }

    log_printf("SWJ_SEQ bits=%u data=", bit_count);
    log_hex(req + 1, byte_count);
    log_putc('\n');

    probe_swj_sequence(bit_count, req + 1);

    resp[0] = DAP_OK;
    return 1;
}
