/*
 * dap.c — CMSIS-DAP v2 command dispatcher.
 *
 * Layout of every DAP request packet:
 *     byte 0    command ID
 *     byte 1+   payload (variable per command)
 *
 * Response shape:
 *     byte 0    echo of command ID
 *     byte 1+   payload
 *
 * We dispatch on byte 0 to a per-command handler. Handlers see the
 * request *after* the command byte, and write the response *after* the
 * echoed command byte. The dispatcher does the echo.
 *
 * Variable-length commands (Transfer, TransferBlock, SWJ_Sequence,
 * SWD_Sequence, JTAG_Sequence, ExecuteCommands) parse their own count
 * fields. We do not pre-compute lengths from a static table — the
 * handler knows.
 */

#include "dap/dap.h"
#include "dap/dap_internal.h"
#include "util/log.h"

#include <stddef.h>

uint8_t dap_connected_port = DAP_PORT_OFF;

static uint16_t dap_handle_invalid(const uint8_t *req, uint16_t req_len,
                                   uint8_t *resp, uint16_t resp_cap)
{
    (void) req; (void) req_len;
    if (resp_cap < 1) return 0;
    resp[0] = DAP_ERROR;
    return 1;
}

static const dap_handler_fn s_handlers[256] = {
    [ID_DAP_Info]              = dap_handle_info,
    [ID_DAP_HostStatus]        = dap_handle_host_status,
    [ID_DAP_Connect]           = dap_handle_connect,
    [ID_DAP_Disconnect]        = dap_handle_disconnect,
    [ID_DAP_TransferConfigure] = dap_handle_xfer_config,
    [ID_DAP_Transfer]          = dap_handle_transfer,
    [ID_DAP_TransferBlock]     = dap_handle_xfer_block,
    [ID_DAP_WriteABORT]        = dap_handle_write_abort,
    [ID_DAP_Delay]             = dap_handle_delay,
    [ID_DAP_ResetTarget]       = dap_handle_reset_tgt,

    [ID_DAP_SWJ_Pins]          = dap_handle_swj_pins,
    [ID_DAP_SWJ_Clock]         = dap_handle_swj_clock,
    [ID_DAP_SWJ_Sequence]      = dap_handle_swj_seq,

    [ID_DAP_SWD_Configure]     = dap_handle_swd_config,
    [ID_DAP_SWD_Sequence]      = dap_handle_swd_seq,

    [ID_DAP_JTAG_Sequence]     = dap_handle_jtag_seq,
    [ID_DAP_JTAG_Configure]    = dap_handle_jtag_cfg,
    [ID_DAP_JTAG_IDCODE]       = dap_handle_jtag_idcode,

    [ID_DAP_SWO_Transport]     = dap_handle_swo_xport,
    [ID_DAP_SWO_Mode]          = dap_handle_swo_mode,
    [ID_DAP_SWO_Baudrate]      = dap_handle_swo_baud,
    [ID_DAP_SWO_Control]       = dap_handle_swo_ctrl,
    [ID_DAP_SWO_Status]        = dap_handle_swo_status,
    [ID_DAP_SWO_Data]          = dap_handle_swo_data,
    [ID_DAP_SWO_ExtendedStatus]= dap_handle_swo_estat,

    [ID_DAP_ExecuteCommands]   = dap_handle_execute,
};

void dap_init(void)
{
    dap_connected_port = DAP_PORT_OFF;
}

uint16_t dap_dispatch(const uint8_t *req, uint16_t req_len,
                      uint8_t *resp, uint16_t resp_cap)
{
    if (req_len < 1 || resp_cap < 2) return 0;

    const uint8_t cmd = req[0];
    dap_handler_fn h = s_handlers[cmd];
    if (h == NULL) h = dap_handle_invalid;

    /* Log ONLY SWD_Sequence (0x1D) and Connect/Disconnect/Reset
     * transitions. Everything else (SWJ_Sequence already logged
     * separately, Info, Transfer, etc.) is silent to keep volume low
     * — high-frequency logging from xfer_cb-adjacent code crashes the
     * firmware. */
    if (cmd == ID_DAP_SWD_Sequence) {
        log_puts("SWD_SEQ data=");
        uint16_t dump = req_len > 24 ? 24 : req_len;
        log_hex(req, dump);
        log_putc('\n');
    } else if (cmd == ID_DAP_Connect) {
        log_puts("CONNECT\n");
    } else if (cmd == ID_DAP_Disconnect) {
        log_puts("DISCONN\n");
    } else if (cmd == ID_DAP_ResetTarget) {
        log_puts("RESET\n");
    }

    /* Echo the command byte. */
    resp[0] = cmd;

    uint16_t payload_len = h(req + 1, (uint16_t)(req_len - 1),
                             resp + 1, (uint16_t)(resp_cap - 1));

    return (uint16_t) (payload_len + 1u);
}
