/*
 * dap.h — CMSIS-DAP v2 command dispatcher entry point.
 *
 * Single function: given a request packet and a response buffer, run
 * the requested command(s) and return the number of bytes written into
 * the response buffer. The class driver in usb_dap_class.c calls this
 * for every USB OUT URB.
 *
 * The dispatcher is single-threaded and re-entrant only via
 * DAP_ExecuteCommands (0x7F) — i.e. one bulk OUT URB completes one
 * dispatcher invocation, and inside that the dispatcher may recurse
 * for atomic-command bundles.
 */

#ifndef EDEV_DAPV2_DAP_H
#define EDEV_DAPV2_DAP_H

#include <stdint.h>

/* Initialise dispatcher state (capabilities, default config). Call once
 * at boot before tud_init. */
void dap_init(void);

/* Run one DAP packet through the dispatcher.
 *
 *   req      — request bytes, byte 0 is the command ID
 *   req_len  — total bytes available (host wrote this many)
 *   resp     — caller-allocated response buffer
 *   resp_cap — bytes available in resp
 *
 * Returns the number of bytes written into resp. Always ≥ 1 (the
 * command ID echo); the dispatcher never returns 0.
 *
 * If the command isn't recognised, the dispatcher writes the standard
 * single-byte 0xFF (DAP_Invalid) response — preserving the host's
 * expectation that "request → response" is a 1:1 mapping. */
uint16_t dap_dispatch(const uint8_t *req, uint16_t req_len,
                      uint8_t *resp, uint16_t resp_cap);

#endif /* EDEV_DAPV2_DAP_H */
