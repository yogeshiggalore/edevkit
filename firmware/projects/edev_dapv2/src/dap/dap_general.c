/*
 * dap_general.c — the "small" CMSIS-DAP commands:
 *   0x01 DAP_HostStatus  — turn the connect/run LEDs on/off
 *   0x02 DAP_Connect     — SWD or JTAG (or "Default")
 *   0x03 DAP_Disconnect  — leave the bus alone
 *   0x09 DAP_Delay       — busy-wait N microseconds
 *   0x0A DAP_ResetTarget — pulse nRESET (or claim we did)
 *
 * These are all 1–6 bytes wire-side and have no PIO interaction.
 */

#include "dap_internal.h"

#include "pico/time.h"

#include "hw/jtag.h"
#include "util/led.h"

/* Shared state owned by this module — read by dap_swd.c via the externs
 * in dap_internal.h. */
uint8_t dap_active_port = EDEV_DAP_PORT_DISABLED;

void dap_general_init(void)
{
    dap_active_port = EDEV_DAP_PORT_DISABLED;
}

/* ----------------------------------------------------------------- */

uint16_t dap_handle_host_status(const uint8_t *req, uint16_t req_avail,
                                uint8_t *resp, uint16_t resp_cap,
                                uint16_t *resp_used)
{
    if (req_avail < 3u || resp_cap < 2u) {
        *resp_used = 0;
        return 0;
    }
    uint8_t type   = req[1];   /* 0 = connect LED, 1 = running LED */
    uint8_t status = req[2];   /* 0 = off,         1 = on          */

    /* v0.1 has only one LED — let either LED type drive it. M5+ will
     * split connect vs run if the user has a multi-LED board. */
    (void)type;
    led_set(status != 0);

    resp[1] = 0;               /* status OK */
    *resp_used = 2u;
    return 3u;
}

uint16_t dap_handle_connect(const uint8_t *req, uint16_t req_avail,
                            uint8_t *resp, uint16_t resp_cap,
                            uint16_t *resp_used)
{
    if (req_avail < 2u || resp_cap < 2u) {
        *resp_used = 0;
        return 0;
    }

    /* req[1] in {0 = Default, 1 = SWD, 2 = JTAG}. Default means "host
     * doesn't care, pick one". We pick SWD because every modern
     * Cortex-M target speaks it and JTAG users always pass an explicit
     * 2. */
    uint8_t requested = req[1];
    if (requested == 0u || requested == EDEV_DAP_PORT_SWD) {
        if (dap_active_port == EDEV_DAP_PORT_JTAG) {
            jtag_detach();
        }
        dap_active_port = EDEV_DAP_PORT_SWD;
        resp[1] = EDEV_DAP_PORT_SWD;
    } else if (requested == EDEV_DAP_PORT_JTAG) {
        if (dap_active_port != EDEV_DAP_PORT_JTAG) {
            jtag_attach();
        }
        dap_active_port = EDEV_DAP_PORT_JTAG;
        resp[1] = EDEV_DAP_PORT_JTAG;
    } else {
        if (dap_active_port == EDEV_DAP_PORT_JTAG) {
            jtag_detach();
        }
        dap_active_port = EDEV_DAP_PORT_DISABLED;
        resp[1] = EDEV_DAP_PORT_DISABLED;
    }

    *resp_used = 2u;
    return 2u;
}

uint16_t dap_handle_disconnect(const uint8_t *req, uint16_t req_avail,
                               uint8_t *resp, uint16_t resp_cap,
                               uint16_t *resp_used)
{
    (void)req;
    if (req_avail < 1u || resp_cap < 2u) {
        *resp_used = 0;
        return 0;
    }
    if (dap_active_port == EDEV_DAP_PORT_JTAG) {
        jtag_detach();
    }
    dap_active_port = EDEV_DAP_PORT_DISABLED;
    resp[1] = 0;               /* status OK */
    *resp_used = 2u;
    return 1u;
}

uint16_t dap_handle_delay(const uint8_t *req, uint16_t req_avail,
                          uint8_t *resp, uint16_t resp_cap,
                          uint16_t *resp_used)
{
    if (req_avail < 3u || resp_cap < 2u) {
        *resp_used = 0;
        return 0;
    }
    uint16_t delay_us = (uint16_t)((uint16_t)req[1] | ((uint16_t)req[2] << 8));
    if (delay_us > 0u) {
        busy_wait_us(delay_us);
    }
    resp[1] = 0;
    *resp_used = 2u;
    return 3u;
}

uint16_t dap_handle_reset_target(const uint8_t *req, uint16_t req_avail,
                                 uint8_t *resp, uint16_t resp_cap,
                                 uint16_t *resp_used)
{
    (void)req;
    if (req_avail < 1u || resp_cap < 3u) {
        *resp_used = 0;
        return 0;
    }
    /* Spec says: bExecute = 0 means "no device-specific reset
     * sequence implemented" — the host uses DAP_SWJ_Pins to drive
     * nRESET itself. We don't implement a target-specific sequence,
     * so report bExecute=0. */
    resp[1] = 0;               /* status OK */
    resp[2] = 0;               /* bExecute = no special sequence */
    *resp_used = 3u;
    return 1u;
}
