/*
 * dap_internal.h — declarations shared between dap.c (dispatcher) and
 * the per-command-group handler files (dap_info.c, dap_general.c,
 * dap_swj.c, dap_swd.c, dap_jtag.c, dap_swo.c, dap_atomic.c).
 *
 * Handler signature contract:
 *
 *   uint16_t dap_handle_xxx(const uint8_t *req, uint16_t req_avail,
 *                           uint8_t *resp, uint16_t resp_cap,
 *                           uint16_t *resp_used);
 *
 *   req       — points at the command byte (req[0] == DAP_CMD_XXX).
 *   req_avail — number of bytes available starting at req[0], including
 *               the command byte itself.
 *   resp      — points at the start of the response slot for this cmd.
 *               resp[0] is pre-filled with the command echo by the
 *               dispatcher; the handler may overwrite it (e.g. for
 *               DAP_Invalid 0xFF).
 *   resp_cap  — bytes available starting at resp[0].
 *   *resp_used — out parameter: number of bytes written into resp[].
 *
 *   Return value — number of request bytes consumed (≥ 1 on success;
 *   0 if the request is malformed / too short). The dispatcher uses
 *   this to advance through atomic-command bundles.
 */

#ifndef EDEV_DAPV2_DAP_INTERNAL_H
#define EDEV_DAPV2_DAP_INTERNAL_H

#include <stdint.h>

#include "dap_config.h"

void dap_info_init   (void);
void dap_general_init(void);
void dap_swj_init    (void);
void dap_swd_init    (void);
void dap_jtag_init   (void);
void dap_swo_init    (void);

/* ----- 0x00 ----- */
uint16_t dap_handle_info(const uint8_t *req, uint16_t req_avail,
                         uint8_t *resp, uint16_t resp_cap,
                         uint16_t *resp_used);

/* ----- 0x01 / 0x02 / 0x03 / 0x09 / 0x0A ----- */
uint16_t dap_handle_host_status (const uint8_t *r, uint16_t ra, uint8_t *s, uint16_t sc, uint16_t *su);
uint16_t dap_handle_connect     (const uint8_t *r, uint16_t ra, uint8_t *s, uint16_t sc, uint16_t *su);
uint16_t dap_handle_disconnect  (const uint8_t *r, uint16_t ra, uint8_t *s, uint16_t sc, uint16_t *su);
uint16_t dap_handle_delay       (const uint8_t *r, uint16_t ra, uint8_t *s, uint16_t sc, uint16_t *su);
uint16_t dap_handle_reset_target(const uint8_t *r, uint16_t ra, uint8_t *s, uint16_t sc, uint16_t *su);

/* ----- 0x10 / 0x11 / 0x12 ----- */
uint16_t dap_handle_swj_pins    (const uint8_t *r, uint16_t ra, uint8_t *s, uint16_t sc, uint16_t *su);
uint16_t dap_handle_swj_clock   (const uint8_t *r, uint16_t ra, uint8_t *s, uint16_t sc, uint16_t *su);
uint16_t dap_handle_swj_sequence(const uint8_t *r, uint16_t ra, uint8_t *s, uint16_t sc, uint16_t *su);

/* ----- 0x13 / 0x1D / 0x04..0x08 (transfer family) ----- */
uint16_t dap_handle_swd_configure       (const uint8_t *r, uint16_t ra, uint8_t *s, uint16_t sc, uint16_t *su);
uint16_t dap_handle_swd_sequence        (const uint8_t *r, uint16_t ra, uint8_t *s, uint16_t sc, uint16_t *su);
uint16_t dap_handle_transfer_configure  (const uint8_t *r, uint16_t ra, uint8_t *s, uint16_t sc, uint16_t *su);
uint16_t dap_handle_transfer            (const uint8_t *r, uint16_t ra, uint8_t *s, uint16_t sc, uint16_t *su);
uint16_t dap_handle_transfer_block      (const uint8_t *r, uint16_t ra, uint8_t *s, uint16_t sc, uint16_t *su);
uint16_t dap_handle_transfer_abort      (const uint8_t *r, uint16_t ra, uint8_t *s, uint16_t sc, uint16_t *su);
uint16_t dap_handle_write_abort         (const uint8_t *r, uint16_t ra, uint8_t *s, uint16_t sc, uint16_t *su);

/* ----- 0x14 / 0x15 / 0x16 ----- */
uint16_t dap_handle_jtag_sequence (const uint8_t *r, uint16_t ra, uint8_t *s, uint16_t sc, uint16_t *su);
uint16_t dap_handle_jtag_configure(const uint8_t *r, uint16_t ra, uint8_t *s, uint16_t sc, uint16_t *su);
uint16_t dap_handle_jtag_idcode   (const uint8_t *r, uint16_t ra, uint8_t *s, uint16_t sc, uint16_t *su);

/* ----- 0x17..0x1E SWO ----- */
uint16_t dap_handle_swo(const uint8_t *r, uint16_t ra, uint8_t *s, uint16_t sc, uint16_t *su);

/* ----- 0x7F atomic ----- */
uint16_t dap_handle_execute_commands(const uint8_t *r, uint16_t ra, uint8_t *s, uint16_t sc, uint16_t *su);

/* ----- 0x80..0x9F vendor (edev_dapv2-specific) ----- */
uint16_t dap_handle_vendor(const uint8_t *r, uint16_t ra, uint8_t *s, uint16_t sc, uint16_t *su);

/* ----- low-level SWD primitive, exposed for vendor commands ----------
 *
 * Bits 0..3 of `request` encode APnDP / RnW / A2 / A3 (same layout as
 * the host-facing DAP_Transfer request byte). `data` is the value to
 * write, or where to put the read result. Returns the 3-bit SWD ACK,
 * optionally OR'd with DAP_TRANSFER_ERROR for parity / illegal-ACK.
 *
 * NOTE on AP reads: the first AP read returns the previous AP read's
 * value (pipelined). Read DP.RDBUFF (request = 0x0E) after to flush.
 */
uint8_t swd_transfer(uint32_t request, uint32_t *data);

/* ----- shared state owned by per-file modules ----- */

/* Current protocol port (set by DAP_Connect, read by transfer handlers). */
extern uint8_t  dap_active_port;

/* Transfer config — set by DAP_TransferConfigure, used by Transfer*. */
extern uint8_t  dap_tfr_idle_cycles;
extern uint16_t dap_tfr_wait_retry;
extern uint16_t dap_tfr_match_retry;

/* SWD config — set by DAP_SWD_Configure. */
extern uint8_t  dap_swd_turnaround_cycles;  /* 1..4 */
extern uint8_t  dap_swd_data_phase;         /* 0 or 1 */

#endif /* EDEV_DAPV2_DAP_INTERNAL_H */
