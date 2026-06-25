/*
 * dap_swd.c — DAP_TransferConfigure, DAP_Transfer, DAP_TransferBlock,
 * DAP_SWD_Configure, DAP_SWD_Sequence.
 *
 * These five commands implement the actual host-driven ADIv5 state
 * machine. Most of the cleverness lives in `probe_swd_transfer()`;
 * here we parse CMSIS-DAP wire-format and drive that function.
 */

#include "dap/dap_internal.h"
#include "hw/probe.h"
#include "util/log.h"

#include <stdbool.h>
#include <string.h>

/* Per-xact tracing — VERY heavy at runtime. Off by default; enable
 * only for SWD-level debugging. Each xact adds ~10 chars to the log
 * ring and slows DAP processing during large reads. */
#ifndef EDEV_LOG_TRANSFER
#define EDEV_LOG_TRANSFER 0  /* off for speed */
#endif

#if EDEV_LOG_TRANSFER
#  define LX_PUTC(c)      log_putc(c)
#  define LX_PUTS(s)      log_puts(s)
#  define LX_HEX(p, n)    log_hex((p), (n))
#else
#  define LX_PUTC(c)      ((void)0)
#  define LX_PUTS(s)      ((void)0)
#  define LX_HEX(p, n)    ((void)0)
#endif

/* TransferConfigure state — shared across Transfer / TransferBlock. */
static uint16_t s_wait_retry  = 100;
static uint16_t s_match_retry = 0;
static uint8_t  s_idle_cycles = 0;

uint16_t dap_handle_xfer_config(const uint8_t *req, uint16_t req_len,
                                uint8_t *resp, uint16_t resp_cap)
{
    if (req_len < 5 || resp_cap < 1) return 0;
    s_idle_cycles = req[0];
    s_wait_retry  = (uint16_t)(req[1] | (req[2] << 8));
    s_match_retry = (uint16_t)(req[3] | (req[4] << 8));
    probe_set_xfer_config(s_idle_cycles, s_wait_retry, s_match_retry);
    resp[0] = DAP_OK;
    return 1;
}

uint16_t dap_handle_swd_config(const uint8_t *req, uint16_t req_len,
                               uint8_t *resp, uint16_t resp_cap)
{
    if (req_len < 1 || resp_cap < 1) return 0;
    uint8_t cfg = req[0];
    /* cfg bits: 0..1 turnaround (0..3 → 1..4), 2 data_phase */
    probe_set_swd_config(cfg & 0x3u, (cfg >> 2) & 0x1u);
    resp[0] = DAP_OK;
    return 1;
}

uint16_t dap_handle_swd_seq(const uint8_t *req, uint16_t req_len,
                            uint8_t *resp, uint16_t resp_cap)
{
    if (req_len < 1) return 0;
    uint8_t count = req[0];
    return probe_swd_sequence(count, req + 1, (uint16_t)(req_len - 1),
                              resp, resp_cap);
}

/* DAP_Transfer (0x05) — list of per-xact ops with optional match-mask.
 *
 * Request payload (after command byte 0x05):
 *   req[0]    DAP index (irrelevant in SWD; always 0)
 *   req[1]    transfer count
 *   req[2..]  per-xact: 1 byte request, then optional u32 (write data
 *             or match value), maybe match-mask u32 if set in xact byte
 *
 * Response payload:
 *   resp[0]   transfers actually executed
 *   resp[1]   last ACK (3 LSB) + status bits
 *   resp[2..] u32 read values for each read xact (concatenated)
 */
/* DP register encoding for the RDBUFF read request byte:
 *   bit0 APnDP=0 (DP), bit1 RnW=1 (read), bit2 A2=1, bit3 A3=1 → 0x0E
 * RDBUFF is at DP address 0x0C (A[3:2]=11). */
#define DP_READ_RDBUFF_REQ  0x0Eu

uint16_t dap_handle_transfer(const uint8_t *req, uint16_t req_len,
                             uint8_t *resp, uint16_t resp_cap)
{
    if (req_len < 2 || resp_cap < 2) return 0;

    /* req[0] is DAP index — ignored. */
    uint8_t count = req[1];

    LX_PUTC('[');
    LX_HEX(&count, 1);

    uint16_t in_pos  = 2;
    uint16_t out_pos = 2;        /* reserve resp[0], resp[1] */
    uint8_t  executed = 0;
    uint8_t  last_ack = 0;

    uint32_t match_mask = 0;

    /* Posted-AP-read pipeline tracking. Per CMSIS-DAP reference
     * DAP.c — an AP read is "posted": we issue the wire transaction
     * but ignore the data (it's pipeline junk). The actual data is
     * captured on the NEXT transaction, either another AP read
     * (which returns the previous AP read's value via the chip
     * pipeline) OR an explicit DP.RDBUFF read.
     *
     * `post_read` = true means we have an AP read pending whose data
     * we still need to capture. */
    bool post_read = false;

    for (uint8_t i = 0; i < count; ++i) {
        if (in_pos >= req_len) { last_ack = DAP_TRANSFER_ERROR; break; }
        uint8_t xact = req[in_pos++];

        bool is_read = (xact & DAP_TRANSFER_RnW) != 0;
        bool is_ap   = (xact & DAP_TRANSFER_APnDP) != 0;

        /* Update match_mask — separate op, doesn't count as a transfer. */
        if (xact & DAP_TRANSFER_MATCH_MASK) {
            if (in_pos + 4u > req_len) { last_ack = DAP_TRANSFER_ERROR; break; }
            match_mask = (uint32_t)req[in_pos]
                       | ((uint32_t)req[in_pos+1] <<  8)
                       | ((uint32_t)req[in_pos+2] << 16)
                       | ((uint32_t)req[in_pos+3] << 24);
            in_pos += 4;
            last_ack = DAP_TRANSFER_OK;
            continue;
        }

        uint32_t data_in = 0;
        if (!is_read) {
            if (in_pos + 4u > req_len) { last_ack = DAP_TRANSFER_ERROR; break; }
            data_in = (uint32_t)req[in_pos]
                    | ((uint32_t)req[in_pos+1] <<  8)
                    | ((uint32_t)req[in_pos+2] << 16)
                    | ((uint32_t)req[in_pos+3] << 24);
            in_pos += 4;
        }

        uint32_t match_value = 0;
        bool match_mode = (xact & DAP_TRANSFER_MATCH_VALUE) && is_read;
        if (match_mode) {
            if (in_pos + 4u > req_len) { last_ack = DAP_TRANSFER_ERROR; break; }
            match_value = (uint32_t)req[in_pos]
                        | ((uint32_t)req[in_pos+1] <<  8)
                        | ((uint32_t)req[in_pos+2] << 16)
                        | ((uint32_t)req[in_pos+3] << 24);
            in_pos += 4;
        }

        uint8_t ack = DAP_TRANSFER_OK;
        uint32_t data_out = 0;
        uint16_t retries;

        /* Flush a pending posted AP read BEFORE we touch a non-AP
         * transaction or a write, since changing DP.SELECT or doing
         * any non-AP op would lose the AP pipeline. */
        if (post_read && (!is_ap || !is_read || match_mode)) {
            retries = s_wait_retry;
            uint32_t prev_data = 0;
            do {
                ack = probe_swd_transfer(DP_READ_RDBUFF_REQ, 0, &prev_data);
            } while (ack == DAP_TRANSFER_WAIT && retries-- > 0);
            LX_PUTS(" F");
            LX_HEX(&ack, 1);
            if (ack != DAP_TRANSFER_OK) {
                last_ack = ack;
                post_read = false;
                break;
            }
            /* Push the pending AP read's value into response. */
            if (out_pos + 4u > resp_cap) { post_read = false; break; }
            resp[out_pos++] = (uint8_t)(prev_data      );
            resp[out_pos++] = (uint8_t)(prev_data >>  8);
            resp[out_pos++] = (uint8_t)(prev_data >> 16);
            resp[out_pos++] = (uint8_t)(prev_data >> 24);
            ++executed;
            post_read = false;
        }

        /* Now execute THIS transaction with WAIT retry. For posted AP
         * reads, the first one passes NULL for data (ignore wire junk);
         * subsequent AP reads capture the pipeline data into data_out. */
        retries = s_wait_retry;
        if (is_ap && is_read && !match_mode) {
            if (!post_read) {
                /* First AP read in a sequence — POST it (ignore data). */
                do {
                    ack = probe_swd_transfer((uint8_t)(xact & 0x0Fu),
                                             data_in, NULL);
                } while (ack == DAP_TRANSFER_WAIT && retries-- > 0);
                last_ack = ack;
                LX_PUTC('p'); LX_HEX(&xact, 1); LX_PUTC(':'); LX_HEX(&ack, 1);
                if (ack != DAP_TRANSFER_OK) break;
                post_read = true;
                /* No data to push for the posted read yet. */
                continue;
            } else {
                /* Sequel AP read: chip returns the PREVIOUS AP read's
                 * value in data_out via the pipeline. Capture and push. */
                do {
                    ack = probe_swd_transfer((uint8_t)(xact & 0x0Fu),
                                             data_in, &data_out);
                } while (ack == DAP_TRANSFER_WAIT && retries-- > 0);
                last_ack = ack;
                LX_PUTC('r'); LX_HEX(&xact, 1); LX_PUTC(':'); LX_HEX(&ack, 1);
                if (ack == DAP_TRANSFER_OK) {
                    uint8_t b[4] = {
                        (uint8_t)data_out, (uint8_t)(data_out >> 8),
                        (uint8_t)(data_out >> 16), (uint8_t)(data_out >> 24),
                    };
                    LX_PUTC('='); LX_HEX(b, 4);
                }
                if (ack != DAP_TRANSFER_OK) break;
                if (out_pos + 4u > resp_cap) break;
                resp[out_pos++] = (uint8_t)(data_out      );
                resp[out_pos++] = (uint8_t)(data_out >>  8);
                resp[out_pos++] = (uint8_t)(data_out >> 16);
                resp[out_pos++] = (uint8_t)(data_out >> 24);
                ++executed;
                /* post_read stays true — this AP read is now itself
                 * posted, awaiting its own flush. */
                continue;
            }
        }

        /* DP read, write, or match-mode read. Just do it. */
        do {
            ack = probe_swd_transfer((uint8_t)(xact & 0x0Fu),
                                     data_in, &data_out);
        } while (ack == DAP_TRANSFER_WAIT && retries-- > 0);
        last_ack = ack;

        LX_PUTC(is_read ? 'r' : 'w');
        LX_HEX(&xact, 1);
        LX_PUTC(':');
        LX_HEX(&ack, 1);
        if (ack == DAP_TRANSFER_OK && (is_read || !is_read)) {
            uint32_t v = is_read ? data_out : data_in;
            uint8_t b[4] = {
                (uint8_t)(v), (uint8_t)(v >> 8),
                (uint8_t)(v >> 16), (uint8_t)(v >> 24),
            };
            LX_PUTC('='); LX_HEX(b, 4);
        }

        if (ack != DAP_TRANSFER_OK) break;

        if (match_mode) {
            uint16_t mretries = s_match_retry;
            while (((data_out & match_mask) != match_value) && mretries > 0) {
                ack = probe_swd_transfer((uint8_t)(xact & 0x0Fu), 0, &data_out);
                if (ack != DAP_TRANSFER_OK) { last_ack = ack; break; }
                --mretries;
            }
            if ((data_out & match_mask) != match_value && last_ack == DAP_TRANSFER_OK) {
                last_ack = DAP_TRANSFER_OK | DAP_TRANSFER_MISMATCH;
            }
            ++executed;
            continue;
        }

        if (is_read) {
            if (out_pos + 4u > resp_cap) break;
            resp[out_pos++] = (uint8_t)(data_out      );
            resp[out_pos++] = (uint8_t)(data_out >>  8);
            resp[out_pos++] = (uint8_t)(data_out >> 16);
            resp[out_pos++] = (uint8_t)(data_out >> 24);
        }
        ++executed;
    }

    /* End of batch: if there's a posted AP read pending, flush via
     * DP.RDBUFF to capture its value. */
    if (post_read && last_ack == DAP_TRANSFER_OK) {
        uint32_t prev_data = 0;
        uint8_t ack = DAP_TRANSFER_OK;
        uint16_t retries = s_wait_retry;
        do {
            ack = probe_swd_transfer(DP_READ_RDBUFF_REQ, 0, &prev_data);
        } while (ack == DAP_TRANSFER_WAIT && retries-- > 0);
        LX_PUTS(" Fend"); LX_HEX(&ack, 1);
        if (ack == DAP_TRANSFER_OK && out_pos + 4u <= resp_cap) {
            resp[out_pos++] = (uint8_t)(prev_data      );
            resp[out_pos++] = (uint8_t)(prev_data >>  8);
            resp[out_pos++] = (uint8_t)(prev_data >> 16);
            resp[out_pos++] = (uint8_t)(prev_data >> 24);
            ++executed;
        } else if (ack != DAP_TRANSFER_OK) {
            last_ack = ack;
        }
    }

    resp[0] = executed;
    resp[1] = last_ack;
    LX_PUTC(']');
    LX_PUTC('\n');
    return out_pos;
}

/* DAP_TransferBlock (0x06) — bulk read/write to same register.
 *
 * Request:
 *   req[0]    DAP index (ignored)
 *   req[1..2] transfer count (u16)
 *   req[3]    transfer request (same as Transfer, but applies to all)
 *   req[4..]  for writes: count × u32 data
 *
 * Response:
 *   resp[0..1] transfers actually executed (u16)
 *   resp[2]    last ACK + status
 *   resp[3..]  read data (only for reads)
 */
uint16_t dap_handle_xfer_block(const uint8_t *req, uint16_t req_len,
                               uint8_t *resp, uint16_t resp_cap)
{
    if (req_len < 4 || resp_cap < 3) return 0;
    /* req[0] ignored */
    uint16_t count = (uint16_t)(req[1] | (req[2] << 8));
    uint8_t  xact  = req[3];
    bool is_read = (xact & DAP_TRANSFER_RnW) != 0;
    bool is_ap   = (xact & DAP_TRANSFER_APnDP) != 0;

    LX_PUTS("BLK ");
    LX_HEX(&xact, 1);
    LX_PUTC('x');
    {
        uint8_t cc[2] = { (uint8_t)count, (uint8_t)(count >> 8) };
        LX_HEX(cc, 2);
    }

    uint16_t in_pos  = 4;
    uint16_t out_pos = 3;
    uint16_t done    = 0;
    uint8_t  last_ack = DAP_TRANSFER_OK;
    uint8_t  ack = DAP_TRANSFER_OK;
    uint32_t data_out = 0;
    uint16_t retries;

    if (is_read && is_ap) {
        /* CMSIS-DAP posted-AP-read flow for bulk reads:
         *   - Issue first AP read (post, ignore data)
         *   - For N-1 more iterations: issue AP read, capture pipelined value
         *   - Final DP.RDBUFF read to capture the last value
         * Total wire ops = count + 1, total captured values = count. */
        retries = s_wait_retry;
        do {
            ack = probe_swd_transfer((uint8_t)(xact & 0x0Fu), 0, NULL);
        } while (ack == DAP_TRANSFER_WAIT && retries-- > 0);
        last_ack = ack;
        if (ack != DAP_TRANSFER_OK) goto out;

        /* Middle N-1 reads: each returns the previous AP read's value. */
        for (uint16_t i = 1; i < count; ++i) {
            retries = s_wait_retry;
            do {
                ack = probe_swd_transfer((uint8_t)(xact & 0x0Fu), 0, &data_out);
            } while (ack == DAP_TRANSFER_WAIT && retries-- > 0);
            last_ack = ack;
            if (ack != DAP_TRANSFER_OK) goto out;
            if (out_pos + 4u > resp_cap) goto out;
            resp[out_pos++] = (uint8_t)(data_out      );
            resp[out_pos++] = (uint8_t)(data_out >>  8);
            resp[out_pos++] = (uint8_t)(data_out >> 16);
            resp[out_pos++] = (uint8_t)(data_out >> 24);
            ++done;
        }

        /* Final DP.RDBUFF read flushes the last AP read's value. */
        retries = s_wait_retry;
        do {
            ack = probe_swd_transfer(DP_READ_RDBUFF_REQ, 0, &data_out);
        } while (ack == DAP_TRANSFER_WAIT && retries-- > 0);
        last_ack = ack;
        if (ack != DAP_TRANSFER_OK) goto out;
        if (out_pos + 4u > resp_cap) goto out;
        resp[out_pos++] = (uint8_t)(data_out      );
        resp[out_pos++] = (uint8_t)(data_out >>  8);
        resp[out_pos++] = (uint8_t)(data_out >> 16);
        resp[out_pos++] = (uint8_t)(data_out >> 24);
        ++done;
    } else {
        /* DP read or any write — straightforward loop, no posted-read magic. */
        for (uint16_t i = 0; i < count; ++i) {
            uint32_t data_in = 0;
            if (!is_read) {
                if (in_pos + 4u > req_len) {
                    last_ack = DAP_TRANSFER_ERROR; goto out;
                }
                data_in = (uint32_t)req[in_pos]
                        | ((uint32_t)req[in_pos+1] <<  8)
                        | ((uint32_t)req[in_pos+2] << 16)
                        | ((uint32_t)req[in_pos+3] << 24);
                in_pos += 4;
            }
            retries = s_wait_retry;
            do {
                ack = probe_swd_transfer((uint8_t)(xact & 0x0Fu),
                                         data_in, &data_out);
            } while (ack == DAP_TRANSFER_WAIT && retries-- > 0);
            last_ack = ack;
            if (ack != DAP_TRANSFER_OK) goto out;

            if (is_read) {
                if (out_pos + 4u > resp_cap) goto out;
                resp[out_pos++] = (uint8_t)(data_out      );
                resp[out_pos++] = (uint8_t)(data_out >>  8);
                resp[out_pos++] = (uint8_t)(data_out >> 16);
                resp[out_pos++] = (uint8_t)(data_out >> 24);
            }
            ++done;
        }
    }

out:
    LX_PUTC('=');
    {
        uint8_t dd[2] = { (uint8_t)done, (uint8_t)(done >> 8) };
        LX_HEX(dd, 2);
    }
    LX_PUTC(':');
    LX_HEX(&last_ack, 1);
    LX_PUTC('\n');

    resp[0] = (uint8_t)(done       & 0xFFu);
    resp[1] = (uint8_t)((done >> 8) & 0xFFu);
    resp[2] = last_ack;
    return out_pos;
}
