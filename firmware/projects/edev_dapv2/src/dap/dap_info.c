/*
 * dap_info.c — DAP_Info (0x00) command handler.
 *
 * Returns metadata about the probe: vendor/product/serial strings, DAP
 * protocol version, capabilities bitmap, packet size/count, SWO buffer
 * size, and the timing-domain timer frequency.
 *
 * This is the first command a host issues; getting it right is what
 * makes the probe discoverable. iProduct ("edev_dapv2 CMSIS-DAP") plus
 * DAP_Info(0xF0) = capabilities bitmap is what every host scans for.
 */

#include "dap_internal.h"

#include <string.h>

#include "usb_descriptors.h"
#include "hw/chip_id.h"

void dap_info_init(void) { /* nothing */ }

/* Helper: write a NUL-terminated ASCII string to resp, with length byte
 * prefix per CMSIS-DAP "type 1 / string" response shape:
 *     resp[0] = cmd echo (already filled by dispatcher)
 *     resp[1] = N (number of bytes that follow)
 *     resp[2..2+N-1] = string bytes, including the trailing NUL
 *
 * We return the total length written (2 + N). */
static uint16_t write_string(uint8_t *resp, uint16_t resp_cap, const char *s)
{
    if (resp_cap < 2u) {
        return 0;
    }
    size_t n = strlen(s) + 1u;          /* include trailing NUL */
    if (n > (size_t)(resp_cap - 2u)) {
        n = (size_t)(resp_cap - 2u);
    }
    resp[1] = (uint8_t)n;
    memcpy(&resp[2], s, n);
    return (uint16_t)(2u + n);
}

uint16_t dap_handle_info(const uint8_t *req, uint16_t req_avail,
                         uint8_t *resp, uint16_t resp_cap,
                         uint16_t *resp_used)
{
    if (req_avail < 2u || resp_cap < 2u) {
        *resp_used = 0;
        return 0;
    }

    uint8_t subcmd = req[1];
    uint16_t written = 0;

    switch (subcmd) {
        /* ----- string responses ----- */
        case DAP_INFO_VENDOR_NAME:
            written = write_string(resp, resp_cap, EDEV_USB_MANUFACTURER);
            break;

        case DAP_INFO_PRODUCT_NAME:
            written = write_string(resp, resp_cap, EDEV_USB_PRODUCT);
            break;

        case DAP_INFO_SERIAL_NUMBER:
            written = write_string(resp, resp_cap, chip_id_serial());
            break;

        case DAP_INFO_PROTOCOL_VERSION:
            /* probe-rs 0.31+ rejects values < "2.2.0" at probe-open time
             * — see project memory `reference_probe_rs_bcd_device_gate`. */
            written = write_string(resp, resp_cap, EDEV_DAPV2_DAP_FW_VER);
            break;

        case DAP_INFO_TARGET_VENDOR:
        case DAP_INFO_TARGET_NAME:
        case DAP_INFO_TARGET_BOARD_VENDOR:
        case DAP_INFO_TARGET_BOARD_NAME:
            /* We're a generic probe — no target device info. Send a
             * zero-length string (count = 0, no payload). */
            if (resp_cap >= 2u) {
                resp[1] = 0;
                written = 2u;
            }
            break;

        case DAP_INFO_PRODUCT_FW_VERSION:
            /* EDEV_DAPV2_FW_VERSION is set by CMakeLists from the
             * EDEV_DAPV2_VERSION_{MAJOR,MINOR,PATCH} block at the top. */
            written = write_string(resp, resp_cap, EDEV_DAPV2_FW_VERSION);
            break;

        /* ----- byte response ----- */
        case DAP_INFO_CAPABILITIES:
            /* CMSIS-DAP v2 capability bitmap is 1 or 2 bytes. We only
             * use byte 0 — byte 1 carries USB COM Port and other bits
             * we don't implement in v0.1. */
            if (resp_cap >= 3u) {
                resp[1] = 1u;                     /* length */
                resp[2] = EDEV_DAP_CAPABILITIES_BYTE0;
                written = 3u;
            }
            break;

        case DAP_INFO_PACKET_COUNT:
            if (resp_cap >= 3u) {
                resp[1] = 1u;
                resp[2] = (uint8_t)EDEV_DAP_PACKET_COUNT;
                written = 3u;
            }
            break;

        /* ----- word response ----- */
        case DAP_INFO_PACKET_SIZE:
            if (resp_cap >= 4u) {
                resp[1] = 2u;
                resp[2] = (uint8_t)(EDEV_DAP_PACKET_SIZE & 0xFFu);
                resp[3] = (uint8_t)(EDEV_DAP_PACKET_SIZE >> 8);
                written = 4u;
            }
            break;

        case DAP_INFO_SWO_BUF_SIZE:
            /* Advertise zero — SWO ring buffer arrives in M7. OpenOCD
             * never reaches this branch because we cleared the SWO caps
             * bits (dap_config.h). probe-rs/pyocd may still ask, so we
             * answer with a well-formed 4-byte zero. */
            if (resp_cap >= 6u) {
                resp[1] = 4u;
                resp[2] = 0; resp[3] = 0; resp[4] = 0; resp[5] = 0;
                written = 6u;
            }
            break;

        case DAP_INFO_TD_TIMER_FREQ:
            /* Test-domain timer not implemented; return 0. */
            if (resp_cap >= 6u) {
                resp[1] = 4u;
                resp[2] = 0; resp[3] = 0; resp[4] = 0; resp[5] = 0;
                written = 6u;
            }
            break;

        case DAP_INFO_UART_RX_BUF_SIZE:
        case DAP_INFO_UART_TX_BUF_SIZE:
            if (resp_cap >= 4u) {
                resp[1] = 2u;
                resp[2] = 0; resp[3] = 0;
                written = 4u;
            }
            break;

        default:
            /* Unknown subcommand — emit count=0 so the host treats this
             * as "no data". Some hosts go ballistic on a hard error so
             * we choose silence over noise. */
            if (resp_cap >= 2u) {
                resp[1] = 0;
                written = 2u;
            }
            break;
    }

    *resp_used = written;
    return 2u; /* consumed: cmd + subcmd */
}
