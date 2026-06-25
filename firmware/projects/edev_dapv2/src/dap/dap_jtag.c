/*
 * dap_jtag.c — JTAG group stubs.
 *
 * v0.1 advertises JTAG in the capability byte (so probe-rs/openocd see
 * the option) but doesn't implement the sequences. If a host actually
 * issues a JTAG_* command we return DAP_ERROR — the host falls back to
 * SWD (which is what nRF5340 et al. use anyway).
 *
 * Full JTAG implementation is a later milestone — requires extending
 * the bit-banger with TDI/TDO scan-chain handling.
 */

#include "dap/dap_internal.h"

uint16_t dap_handle_jtag_seq(const uint8_t *req, uint16_t req_len,
                             uint8_t *resp, uint16_t resp_cap)
{
    (void) req; (void) req_len;
    if (resp_cap < 1) return 0;
    resp[0] = DAP_ERROR;
    return 1;
}

uint16_t dap_handle_jtag_cfg(const uint8_t *req, uint16_t req_len,
                             uint8_t *resp, uint16_t resp_cap)
{
    (void) req; (void) req_len;
    if (resp_cap < 1) return 0;
    resp[0] = DAP_ERROR;
    return 1;
}

uint16_t dap_handle_jtag_idcode(const uint8_t *req, uint16_t req_len,
                                uint8_t *resp, uint16_t resp_cap)
{
    (void) req; (void) req_len;
    if (resp_cap < 5) return 0;
    resp[0] = DAP_ERROR;
    resp[1] = 0; resp[2] = 0; resp[3] = 0; resp[4] = 0;
    return 5;
}
