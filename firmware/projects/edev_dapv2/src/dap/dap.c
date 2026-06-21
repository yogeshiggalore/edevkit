/*
 * dap.c — CMSIS-DAP v2 command dispatcher.
 *
 * Walks bytes of the request packet, routes each command byte to the
 * matching handler in dap_*.c, accumulates the response. Variable
 * length commands (Transfer, TransferBlock, *_Sequence) are sized by
 * the handler itself — see the `_length` siblings of each handler.
 *
 * This file is the spine; the handlers are flesh.
 */

#include "dap.h"

#include <stdint.h>
#include <string.h>

#include "dap_config.h"
#include "dap_internal.h"

void dap_init(void)
{
    dap_info_init();
    dap_general_init();
    dap_swj_init();
    dap_swd_init();
    dap_jtag_init();
    dap_swo_init();
}

/* Single-command dispatch — runs one command starting at req[0].
 * Returns the number of *request* bytes consumed (so the caller can
 * advance through an atomic bundle), or 0 on parse failure. The number
 * of response bytes written is returned via *resp_used. */
static uint16_t dispatch_one(const uint8_t *req, uint16_t req_avail,
                             uint8_t *resp, uint16_t resp_cap,
                             uint16_t *resp_used)
{
    if (req_avail < 1u || resp_cap < 1u) {
        *resp_used = 0;
        return 0;
    }

    const uint8_t cmd = req[0];

    /* Default response if the handler doesn't recognise the command:
     * DAP_Invalid is a single byte 0xFF (per spec, used as the cmd ID
     * echo for "I don't know what you're asking"). */
    resp[0] = cmd;

    switch (cmd) {
        case DAP_CMD_INFO:
            return dap_handle_info(req, req_avail, resp, resp_cap, resp_used);

        case DAP_CMD_HOST_STATUS:
            return dap_handle_host_status(req, req_avail, resp, resp_cap, resp_used);

        case DAP_CMD_CONNECT:
            return dap_handle_connect(req, req_avail, resp, resp_cap, resp_used);

        case DAP_CMD_DISCONNECT:
            return dap_handle_disconnect(req, req_avail, resp, resp_cap, resp_used);

        case DAP_CMD_TRANSFER_CONFIGURE:
            return dap_handle_transfer_configure(req, req_avail, resp, resp_cap, resp_used);

        case DAP_CMD_TRANSFER:
            return dap_handle_transfer(req, req_avail, resp, resp_cap, resp_used);

        case DAP_CMD_TRANSFER_BLOCK:
            return dap_handle_transfer_block(req, req_avail, resp, resp_cap, resp_used);

        case DAP_CMD_TRANSFER_ABORT:
            return dap_handle_transfer_abort(req, req_avail, resp, resp_cap, resp_used);

        case DAP_CMD_WRITE_ABORT:
            return dap_handle_write_abort(req, req_avail, resp, resp_cap, resp_used);

        case DAP_CMD_DELAY:
            return dap_handle_delay(req, req_avail, resp, resp_cap, resp_used);

        case DAP_CMD_RESET_TARGET:
            return dap_handle_reset_target(req, req_avail, resp, resp_cap, resp_used);

        case DAP_CMD_SWJ_PINS:
            return dap_handle_swj_pins(req, req_avail, resp, resp_cap, resp_used);

        case DAP_CMD_SWJ_CLOCK:
            return dap_handle_swj_clock(req, req_avail, resp, resp_cap, resp_used);

        case DAP_CMD_SWJ_SEQUENCE:
            return dap_handle_swj_sequence(req, req_avail, resp, resp_cap, resp_used);

        case DAP_CMD_SWD_CONFIGURE:
            return dap_handle_swd_configure(req, req_avail, resp, resp_cap, resp_used);

        case DAP_CMD_SWD_SEQUENCE:
            return dap_handle_swd_sequence(req, req_avail, resp, resp_cap, resp_used);

        case DAP_CMD_JTAG_SEQUENCE:
            return dap_handle_jtag_sequence(req, req_avail, resp, resp_cap, resp_used);

        case DAP_CMD_JTAG_CONFIGURE:
            return dap_handle_jtag_configure(req, req_avail, resp, resp_cap, resp_used);

        case DAP_CMD_JTAG_IDCODE:
            return dap_handle_jtag_idcode(req, req_avail, resp, resp_cap, resp_used);

        case DAP_CMD_SWO_TRANSPORT:
        case DAP_CMD_SWO_MODE:
        case DAP_CMD_SWO_BAUDRATE:
        case DAP_CMD_SWO_CONTROL:
        case DAP_CMD_SWO_STATUS:
        case DAP_CMD_SWO_DATA:
        case DAP_CMD_SWO_EXTENDED_STATUS:
            return dap_handle_swo(req, req_avail, resp, resp_cap, resp_used);

        case DAP_CMD_EXECUTE_COMMANDS:
            return dap_handle_execute_commands(req, req_avail, resp, resp_cap, resp_used);

        case DAP_CMD_EDEV_NRF_SYS_RESET:
        case DAP_CMD_EDEV_MEM_READ:
        case DAP_CMD_EDEV_MEM_WRITE:
        case DAP_CMD_EDEV_AP_READ:
        case DAP_CMD_EDEV_AP_WRITE:
        case DAP_CMD_EDEV_CORTEX_M_DUMP:
            return dap_handle_vendor(req, req_avail, resp, resp_cap, resp_used);

        default:
            /* DAP_Invalid — single byte response carrying 0xFF as the cmd
             * echo. We've already written resp[0] = cmd above; switch it
             * to 0xFF and return 1. The host treats 0xFF as the "I don't
             * support that" sentinel. */
            resp[0] = 0xFFu;
            *resp_used = 1u;
            return 1u;
    }
}

uint16_t dap_dispatch(const uint8_t *req, uint16_t req_len,
                      uint8_t *resp, uint16_t resp_cap)
{
    uint16_t req_used = 0;
    uint16_t resp_total = 0;

    /* The dispatcher is invoked per USB OUT packet. Inside one packet
     * the host MAY have stuffed multiple back-to-back DAP commands
     * (CMSIS-DAP v2 spec allows this). We walk the buffer until either
     * (a) we've consumed it or (b) a handler refuses to advance, which
     * indicates malformed input — in that case we emit DAP_Invalid and
     * stop, mirroring ARM's reference behaviour. */
    while (req_used < req_len && resp_total < resp_cap) {
        uint16_t resp_used = 0;
        uint16_t consumed = dispatch_one(req + req_used, (uint16_t)(req_len - req_used),
                                         resp + resp_total,
                                         (uint16_t)(resp_cap - resp_total),
                                         &resp_used);
        if (consumed == 0 || resp_used == 0) {
            /* Either malformed or zero-progress handler — append a
             * DAP_Invalid byte and bail. */
            if (resp_total < resp_cap) {
                resp[resp_total++] = 0xFFu;
            }
            break;
        }
        req_used   = (uint16_t)(req_used + consumed);
        resp_total = (uint16_t)(resp_total + resp_used);
    }

    if (resp_total == 0u && resp_cap >= 1u) {
        /* Defensive: never return a zero-length response — the USB IN
         * endpoint needs at least one byte to schedule. */
        resp[0] = 0xFFu;
        resp_total = 1u;
    }

    return resp_total;
}
