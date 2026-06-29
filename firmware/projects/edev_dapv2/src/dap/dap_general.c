/*
 * dap_general.c — HostStatus, Connect/Disconnect, Delay,
 * ResetTarget, WriteABORT.
 */

#include "dap/dap_internal.h"
#include "dap/dap_config.h"
#include "hw/probe.h"

#include "pico/stdlib.h"

uint16_t dap_handle_host_status(const uint8_t *req, uint16_t req_len,
                                uint8_t *resp, uint16_t resp_cap)
{
    /* req[0] = type (0=Connect LED, 1=Running LED), req[1] = on/off.
     * v0.1 has no host LED — accept and ignore. */
    (void) req; (void) req_len;
    if (resp_cap < 1) return 0;
    resp[0] = DAP_OK;
    return 1;
}

uint16_t dap_handle_connect(const uint8_t *req, uint16_t req_len,
                            uint8_t *resp, uint16_t resp_cap)
{
    if (req_len < 1 || resp_cap < 1) return 0;
    uint8_t req_port = req[0];

    /* Default → pick SWD (the host doesn't care; we pick). */
    if (req_port == DAP_PORT_DEFAULT) req_port = DAP_PORT_SWD;

    if (req_port == DAP_PORT_SWD) {
        probe_connect_swd();
        dap_connected_port = DAP_PORT_SWD;
        resp[0] = DAP_PORT_SWD;
        return 1;
    }
    if (req_port == DAP_PORT_JTAG) {
        /* JTAG not implemented in v0.1. Refuse the connect rather than
         * pretending — host falls back gracefully. */
        dap_connected_port = DAP_PORT_OFF;
        resp[0] = DAP_PORT_OFF;
        return 1;
    }

    dap_connected_port = DAP_PORT_OFF;
    resp[0] = DAP_PORT_OFF;
    return 1;
}

uint16_t dap_handle_disconnect(const uint8_t *req, uint16_t req_len,
                               uint8_t *resp, uint16_t resp_cap)
{
    (void) req; (void) req_len;
    probe_disconnect();
    dap_connected_port = DAP_PORT_OFF;
    if (resp_cap < 1) return 0;
    resp[0] = DAP_OK;
    return 1;
}

uint16_t dap_handle_delay(const uint8_t *req, uint16_t req_len,
                          uint8_t *resp, uint16_t resp_cap)
{
    if (req_len < 2 || resp_cap < 1) return 0;
    uint16_t us = (uint16_t)(req[0] | (req[1] << 8));
    sleep_us(us);
    resp[0] = DAP_OK;
    return 1;
}

uint16_t dap_handle_reset_tgt(const uint8_t *req, uint16_t req_len,
                              uint8_t *resp, uint16_t resp_cap)
{
    (void) req; (void) req_len;
    if (resp_cap < 2) return 0;
    probe_reset_target();
    resp[0] = DAP_OK;
    resp[1] = 0;     /* execute = 0: no device-specific reset sequence */
    return 2;
}

uint16_t dap_handle_write_abort(const uint8_t *req, uint16_t req_len,
                                uint8_t *resp, uint16_t resp_cap)
{
    if (req_len < 5 || resp_cap < 1) return 0;
    /* req[0] = DAP index (ignored in SWD), req[1..4] = ABORT value. */
    uint32_t value = (uint32_t)req[1]
                   | ((uint32_t)req[2] <<  8)
                   | ((uint32_t)req[3] << 16)
                   | ((uint32_t)req[4] << 24);
    bool ok = probe_swd_write_abort(value);
    resp[0] = ok ? DAP_OK : DAP_ERROR;
    return 1;
}
