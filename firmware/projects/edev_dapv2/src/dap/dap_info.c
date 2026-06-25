/*
 * dap_info.c — DAP_Info (0x00) subcommand handler.
 *
 * Request: req[0] = info ID
 * Response: payload-length byte followed by data.
 *
 *   0x01  Get Vendor Name        string
 *   0x02  Get Product Name       string
 *   0x03  Get Serial Number      string
 *   0x04  Get FW Version         string
 *   0x05  Get Target Device Vendor (we don't know — empty)
 *   0x06  Get Target Device Name   (we don't know — empty)
 *   0xF0  Get Capabilities       byte
 *   0xF1  Get Test Domain Timer  u32
 *   0xF2  Get SWO Trace Buffer Sz u32
 *   0xFE  Get USB Packet Count   byte
 *   0xFF  Get USB Packet Size    u16
 */

#include "dap/dap_internal.h"
#include "dap/dap_config.h"
#include "hw/chip_id.h"

#include <string.h>

static uint16_t emit_string(const char *s, uint8_t *resp, uint16_t resp_cap)
{
    /* CMSIS-DAP info strings: byte0 = length (incl. NUL), then chars. */
    size_t n = (s != NULL) ? strlen(s) : 0u;
    if (n + 2u > resp_cap) return 0;   /* won't fit */
    resp[0] = (uint8_t) (n + 1u);
    if (n) memcpy(resp + 1, s, n);
    resp[1 + n] = '\0';
    return (uint16_t) (n + 2u);
}

static uint16_t emit_u8(uint8_t v, uint8_t *resp, uint16_t resp_cap)
{
    if (resp_cap < 2) return 0;
    resp[0] = 1;
    resp[1] = v;
    return 2;
}

static uint16_t emit_u16(uint16_t v, uint8_t *resp, uint16_t resp_cap)
{
    if (resp_cap < 3) return 0;
    resp[0] = 2;
    resp[1] = (uint8_t) (v & 0xFFu);
    resp[2] = (uint8_t) ((v >> 8) & 0xFFu);
    return 3;
}

static uint16_t emit_u32(uint32_t v, uint8_t *resp, uint16_t resp_cap)
{
    if (resp_cap < 5) return 0;
    resp[0] = 4;
    resp[1] = (uint8_t) (v        & 0xFFu);
    resp[2] = (uint8_t) ((v >>  8) & 0xFFu);
    resp[3] = (uint8_t) ((v >> 16) & 0xFFu);
    resp[4] = (uint8_t) ((v >> 24) & 0xFFu);
    return 5;
}

uint16_t dap_handle_info(const uint8_t *req, uint16_t req_len,
                         uint8_t *resp, uint16_t resp_cap)
{
    if (req_len < 1 || resp_cap < 1) {
        if (resp_cap >= 1) resp[0] = 0;
        return 1;
    }

    const uint8_t id = req[0];
    switch (id) {
    case 0x01: return emit_string(DAP_VENDOR_NAME_STR,  resp, resp_cap);
    case 0x02: return emit_string(DAP_PRODUCT_NAME_STR, resp, resp_cap);
    case 0x03: return emit_string(chip_id_string(),     resp, resp_cap);
    case 0x04: return emit_string(DAP_FW_VERSION_STR,   resp, resp_cap);
    case 0x05: return emit_string("",                   resp, resp_cap);
    case 0x06: return emit_string("",                   resp, resp_cap);

    case 0xF0:  /* Capabilities — 1 byte */
        return emit_u8(DAP_CAPABILITIES_BYTE0, resp, resp_cap);

    case 0xF1:  /* Test domain timer — not implemented */
        return emit_u32(0u, resp, resp_cap);

    case 0xF2:  /* SWO trace buffer size */
        return emit_u32(0u, resp, resp_cap);

    case 0xFE:  /* USB packet count */
        return emit_u8(DAP_PACKET_COUNT, resp, resp_cap);

    case 0xFF:  /* USB packet size */
        return emit_u16(DAP_PACKET_SIZE, resp, resp_cap);

    default:
        /* Unknown info ID — return zero-length payload. */
        resp[0] = 0;
        return 1;
    }
}
