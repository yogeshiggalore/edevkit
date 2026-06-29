/*
 * dap.h — CMSIS-DAP v2 dispatcher.
 *
 * Single entry point: feed it a packet from the OUT EP, get a response
 * packet to send on the IN EP.
 */

#ifndef EDEV_DAPV2_DAP_DAP_H
#define EDEV_DAPV2_DAP_DAP_H

#include <stdint.h>

void dap_init(void);

/* Run one DAP command (or an atomic bundle).
 *
 * Inputs:
 *   req      — request bytes received from the host
 *   req_len  — number of bytes available in req
 *
 * Outputs:
 *   resp     — buffer for the response packet
 *   resp_cap — capacity of `resp`
 *
 * Returns the number of bytes written to `resp`. 0 ⇒ no response. */
uint16_t dap_dispatch(const uint8_t *req, uint16_t req_len,
                      uint8_t *resp, uint16_t resp_cap);

#endif /* EDEV_DAPV2_DAP_DAP_H */
