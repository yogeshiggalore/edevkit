/*
 * dap_atomic.c — DAP_ExecuteCommands (0x7F).
 *
 * Layout:
 *   req[0]    count of bundled commands
 *   req[1..]  concatenated commands (each in normal request format)
 *
 * We re-enter the dispatcher for each sub-command. The response is a
 * count byte followed by concatenated sub-responses.
 */

#include "dap/dap.h"
#include "dap/dap_internal.h"

uint16_t dap_handle_execute(const uint8_t *req, uint16_t req_len,
                            uint8_t *resp, uint16_t resp_cap)
{
    if (req_len < 1 || resp_cap < 1) return 0;

    uint8_t count = req[0];
    resp[0] = count;
    uint16_t resp_used = 1;
    uint16_t req_pos   = 1;

    for (uint8_t i = 0; i < count; ++i) {
        if (req_pos >= req_len || resp_used >= resp_cap) break;

        /* dap_dispatch wants the WHOLE packet starting at byte 0
         * (the command ID). Pass what remains. */
        uint16_t sub_resp = dap_dispatch(req + req_pos,
                                         (uint16_t)(req_len - req_pos),
                                         resp + resp_used,
                                         (uint16_t)(resp_cap - resp_used));
        if (sub_resp == 0) break;

        /* Figure out how many request bytes the sub-command consumed.
         * The dispatcher doesn't report this; for sub-commands of fixed
         * size the easiest correct answer is to disallow nesting of
         * variable-length commands. CMSIS-DAP spec recommends only
         * fixed-length commands inside ExecuteCommands — host tools
         * already obey this.
         *
         * Concretely, the request length of each sub-command is:
         *   command byte (1) + payload bytes (per command)
         * For variable-length commands we'd have to parse their count
         * fields ourselves. v0.1: refuse and break.
         */
        uint16_t sub_req_len = 0;
        switch (req[req_pos]) {
        case ID_DAP_Info:              sub_req_len = 2; break;
        case ID_DAP_HostStatus:        sub_req_len = 3; break;
        case ID_DAP_Connect:           sub_req_len = 2; break;
        case ID_DAP_Disconnect:        sub_req_len = 1; break;
        case ID_DAP_Delay:             sub_req_len = 3; break;
        case ID_DAP_ResetTarget:       sub_req_len = 1; break;
        case ID_DAP_WriteABORT:        sub_req_len = 6; break;
        case ID_DAP_SWJ_Pins:          sub_req_len = 7; break;
        case ID_DAP_SWJ_Clock:         sub_req_len = 5; break;
        case ID_DAP_SWD_Configure:     sub_req_len = 2; break;
        case ID_DAP_TransferConfigure: sub_req_len = 6; break;
        default:
            /* Variable-length or unknown — stop. Spec says hosts
             * shouldn't put these in an atomic bundle anyway. */
            return resp_used;
        }

        req_pos   = (uint16_t)(req_pos   + sub_req_len);
        resp_used = (uint16_t)(resp_used + sub_resp);
    }

    return resp_used;
}
