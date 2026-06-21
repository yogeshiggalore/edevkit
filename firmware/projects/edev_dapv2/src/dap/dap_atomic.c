/*
 * dap_atomic.c — DAP_ExecuteCommands (0x7F).
 *
 * The host can bundle multiple DAP commands into one atomic ExecuteCommands
 * packet. The wire shape is:
 *
 *   req[0]    = 0x7F
 *   req[1]    = subcommand_count
 *   req[2..]  = N concatenated DAP commands
 *
 * Response:
 *   resp[0]   = 0x7F
 *   resp[1]   = subcommand_count
 *   resp[2..] = N concatenated DAP responses
 *
 * The dispatcher in dap.c already walks back-to-back commands inside a
 * single packet — ExecuteCommands is the same idea wrapped in a
 * counted header so the host can guarantee atomicity. We re-enter
 * dap_dispatch on the bundled payload to handle it.
 */

#include "dap.h"
#include "dap_internal.h"

#include <string.h>

uint16_t dap_handle_execute_commands(const uint8_t *req, uint16_t req_avail,
                                     uint8_t *resp, uint16_t resp_cap,
                                     uint16_t *resp_used)
{
    if (req_avail < 2u || resp_cap < 2u) {
        *resp_used = 0;
        return 0;
    }
    uint8_t count = req[1];
    (void)count;  /* We trust the dispatcher's byte-walk over the count
                     hint — count is for hosts that pre-allocate, not
                     for us to bound. */

    /* Reserve resp[0]=cmd_echo (already set by dispatcher) and
     * resp[1]=count_echo. Bundle responses go to resp[2..]. */
    if (resp_cap < 2u) {
        *resp_used = 0;
        return 0;
    }

    uint16_t inner_len = dap_dispatch(req + 2u, (uint16_t)(req_avail - 2u),
                                      resp + 2u, (uint16_t)(resp_cap - 2u));

    resp[1] = count;
    *resp_used = (uint16_t)(2u + inner_len);

    /* The atomic bundle consumes the rest of the packet. dap.c will
     * see this and not look for more commands after — there shouldn't
     * be any per spec. */
    return req_avail;
}
