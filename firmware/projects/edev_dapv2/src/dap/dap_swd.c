/*
 * dap_swd.c — SWD-and-transfer commands. The meat of the CMSIS-DAP
 * protocol.
 *
 *   0x04 DAP_TransferConfigure  — save idle/retry counts
 *   0x05 DAP_Transfer           — N independent AP/DP transfers
 *   0x06 DAP_TransferBlock      — N transfers on one register, bulk
 *   0x07 DAP_TransferAbort      — async cancel (best-effort)
 *   0x08 DAP_WriteABORT         — write DP[0x00] ABORT register
 *   0x13 DAP_SWD_Configure      — turnaround cycles + data-phase
 *   0x1D DAP_SWD_Sequence       — arbitrary SWD bit sequences
 *
 * The transfer engine in swd_transfer() is the SWD line protocol:
 * 8-bit packet, turnaround, 3-bit ACK, optional 32-bit data + parity,
 * back-turnaround, idle cycles. Every other transfer command is a
 * loop over swd_transfer().
 *
 * Modelled on raspberrypi/debugprobe (Apache-2.0). The transfer
 * decision tree and parity handling are standard ARM Debug Interface
 * v5 / v6 protocol — there's no creative interpretation here.
 */

#include "dap_internal.h"

#include <stdbool.h>
#include <string.h>

#include "hw/probe.h"

/* ----- request byte bit fields (host's view, NOT the wire format) -- */
#define TFR_APnDP        (1u << 0)
#define TFR_RnW          (1u << 1)
#define TFR_A2           (1u << 2)
#define TFR_A3           (1u << 3)
#define TFR_VALUE_MATCH  (1u << 4)
#define TFR_MATCH_MASK   (1u << 5)
#define TFR_TIMESTAMP    (1u << 7)

/* ----- exported transfer/SWD config (declared extern in dap_internal.h) */
uint8_t  dap_tfr_idle_cycles       = 0;
uint16_t dap_tfr_wait_retry        = 64;
uint16_t dap_tfr_match_retry       = 0;
uint8_t  dap_swd_turnaround_cycles = 1;
uint8_t  dap_swd_data_phase        = 0;

/* ----- per-connection runtime state shared across transfers --------- */
static uint32_t s_match_mask = 0xFFFFFFFFu;

void dap_swd_init(void)
{
    dap_tfr_idle_cycles       = 0;
    dap_tfr_wait_retry        = 64;
    dap_tfr_match_retry       = 0;
    dap_swd_turnaround_cycles = 1;
    dap_swd_data_phase        = 0;
    s_match_mask              = 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ */
/*  0x04 DAP_TransferConfigure                                         */
/* ------------------------------------------------------------------ */

uint16_t dap_handle_transfer_configure(const uint8_t *req, uint16_t req_avail,
                                       uint8_t *resp, uint16_t resp_cap,
                                       uint16_t *resp_used)
{
    if (req_avail < 6u || resp_cap < 2u) {
        *resp_used = 0;
        return 0;
    }
    dap_tfr_idle_cycles = req[1];
    dap_tfr_wait_retry  = (uint16_t)((uint16_t)req[2] | ((uint16_t)req[3] << 8));
    dap_tfr_match_retry = (uint16_t)((uint16_t)req[4] | ((uint16_t)req[5] << 8));

    resp[1] = 0;
    *resp_used = 2u;
    return 6u;
}

/* ------------------------------------------------------------------ */
/*  0x13 DAP_SWD_Configure                                             */
/* ------------------------------------------------------------------ */

uint16_t dap_handle_swd_configure(const uint8_t *req, uint16_t req_avail,
                                  uint8_t *resp, uint16_t resp_cap,
                                  uint16_t *resp_used)
{
    if (req_avail < 2u || resp_cap < 2u) {
        *resp_used = 0;
        return 0;
    }
    uint8_t cfg = req[1];
    dap_swd_turnaround_cycles = (uint8_t)((cfg & 0x03u) + 1u);
    dap_swd_data_phase        = (uint8_t)((cfg >> 2) & 0x01u);

    resp[1] = 0;
    *resp_used = 2u;
    return 2u;
}

/* ------------------------------------------------------------------ */
/*  Issue trailing idle cycles after a successful transfer.            */
/* ------------------------------------------------------------------ */

static void emit_idle(void)
{
    uint32_t n = dap_tfr_idle_cycles;
    while (n > 0u) {
        uint32_t chunk = n > 32u ? 32u : n;
        probe_write_bits(chunk, 0u);
        n -= chunk;
    }
}

/* ------------------------------------------------------------------ */
/*  Single SWD transfer — the protocol primitive.                      */
/*                                                                      */
/*  request bits 0..3 = APnDP / RnW / A2 / A3                          */
/*  *data is the value to write (for write) or where to put the read   */
/*  result (for read). May be NULL on read if the caller doesn't care. */
/*                                                                      */
/*  Returns the 3-bit SWD ACK, or DAP_TRANSFER_ERROR on parity/protocol*/
/*  failure.                                                            */
/* ------------------------------------------------------------------ */

uint8_t swd_transfer(uint32_t request, uint32_t *data)
{
    /* Build the 8-bit SWD packet:
     *   bit 0 = start (1)
     *   bit 1 = APnDP
     *   bit 2 = RnW
     *   bit 3 = A2
     *   bit 4 = A3
     *   bit 5 = parity (over bits 1..4)
     *   bit 6 = stop (0)
     *   bit 7 = park (1) */
    uint8_t prq = 0x81u;   /* start + park already set */
    uint8_t parity = 0;
    for (uint8_t n = 1; n < 5u; n++) {
        uint8_t bit = (uint8_t)((request >> (n - 1u)) & 0x1u);
        prq    |= (uint8_t)(bit << n);
        parity = (uint8_t)(parity + bit);
    }
    prq |= (uint8_t)((parity & 1u) << 5);

    probe_write_bits(8u, prq);

    /* Read turnaround + 3-bit ACK in one shot, then shift to extract
     * just the ACK in the LSBs. The turnaround clocks happen with
     * SWDIO already tristated by the PIO program (OE=0 for reads). */
    uint32_t ack_field = probe_read_bits((uint32_t)(dap_swd_turnaround_cycles + 3u));
    uint8_t  ack       = (uint8_t)((ack_field >> dap_swd_turnaround_cycles) & 0x07u);

    if (ack == DAP_TRANSFER_OK) {
        if (request & TFR_RnW) {
            /* Read: 32 data bits + 1 parity bit, then turnaround back
             * to drive. */
            uint32_t val      = probe_read_bits(32u);
            uint32_t par_bit  = probe_read_bits(1u);
            uint32_t computed = (uint32_t)__builtin_popcount(val) & 1u;
            if (par_bit != computed) {
                /* Wire ACK was OK but data parity failed. Keep ACK=OK
                 * in bits 0:2 and OR in ProtocolError (bit 3) — spec
                 * requires bits 0:2 to be a valid wire-ACK value. */
                ack = DAP_TRANSFER_OK | DAP_TRANSFER_ERROR;
            } else if (data) {
                *data = val;
            }
            probe_hiz_clocks(dap_swd_turnaround_cycles);
        } else {
            /* Write: turnaround back to drive, then 32 data + 1
             * parity. */
            probe_hiz_clocks(dap_swd_turnaround_cycles);
            uint32_t val = data ? *data : 0u;
            probe_write_bits(32u, val);
            probe_write_bits(1u, (uint32_t)__builtin_popcount(val) & 1u);
        }
        emit_idle();
        return ack;
    }

    /* WAIT or FAULT — target couldn't service the transfer. */
    if (ack == DAP_TRANSFER_WAIT || ack == DAP_TRANSFER_FAULT) {
        /* If the host explicitly asked for "always do the data phase"
         * (DAP_SWD_Configure config bit 2) we still have to consume
         * (read) or send (write) the 33 data+parity bits so the line
         * stays in sync. Without data-phase, the bus simply doesn't
         * transfer the data part and only the turnaround is left. */
        if (dap_swd_data_phase && (request & TFR_RnW)) {
            (void)probe_read_bits(33u);
        }
        probe_hiz_clocks(dap_swd_turnaround_cycles);
        if (dap_swd_data_phase && !(request & TFR_RnW)) {
            probe_write_bits(32u, 0u);
            probe_write_bits(1u, 0u);
        }
        return ack;
    }

    /* Wire ACK was NO_ACK (0b111 — target not responding) or an illegal
     * code (0/3/5/6 from timing glitch). Drain the line so the next
     * packet finds it clean. */
    uint32_t drain = (uint32_t)dap_swd_turnaround_cycles + 32u + 1u;
    while (drain > 0u) {
        uint32_t chunk = drain > 32u ? 32u : drain;
        (void)probe_read_bits(chunk);
        drain -= chunk;
    }
    /* Return NO_ACK (0x07) in the wire-ACK field, not 0x08 alone: spec
     * requires bits 0:2 ∈ {1,2,4,7}. Setting only bit 3 leaves bits 0:2
     * at 0, which pyocd rejects as "Unexpected ACK '0'". For illegal
     * codes we also OR in ProtocolError so the host knows it wasn't a
     * clean NO_ACK. */
    return (ack == 0x07u) ? DAP_TRANSFER_NO_ACK
                          : (DAP_TRANSFER_NO_ACK | DAP_TRANSFER_ERROR);
}

/* ------------------------------------------------------------------ */
/*  0x05 DAP_Transfer                                                  */
/* ------------------------------------------------------------------ */

uint16_t dap_handle_transfer(const uint8_t *req, uint16_t req_avail,
                             uint8_t *resp, uint16_t resp_cap,
                             uint16_t *resp_used)
{
    if (req_avail < 3u || resp_cap < 3u) {
        *resp_used = 0;
        return 0;
    }
    /* req[1] is the DAP_Index — for SWD it's ignored. JTAG would use
     * it to select a TAP. */
    (void)req[1];

    uint8_t req_count = req[2];
    uint16_t req_idx = 3u;
    uint16_t resp_idx = 3u;       /* leave [1]=count [2]=ack for end */
    uint8_t  done = 0;
    uint8_t  ack = DAP_TRANSFER_OK;

    if (dap_active_port != EDEV_DAP_PORT_SWD) {
        /* Spec is fuzzy about "what if no port" — openocd returns OK
         * with 0 transfers, probe-rs returns ERROR. We pick ERROR
         * because hosts treat that as "give up cleanly". */
        resp[1] = 0;
        resp[2] = DAP_TRANSFER_ERROR;
        *resp_used = 3u;
        return req_avail;
    }

    for (uint8_t i = 0; i < req_count; i++) {
        if (req_idx >= req_avail) {
            break;
        }
        uint8_t tr = req[req_idx++];
        bool is_read  = (tr & TFR_RnW) != 0;
        bool vm       = (tr & TFR_VALUE_MATCH) != 0;   /* read-with-match */
        bool mm_write = (tr & TFR_MATCH_MASK)  != 0;   /* write mask */

        if (mm_write && !is_read) {
            /* Special: DAP "write match mask" — the 32-bit value is
             * stored as the mask, not actually transferred. The host
             * uses this before issuing a VM read. */
            if ((uint16_t)(req_idx + 4u) > req_avail) {
                break;
            }
            s_match_mask = (uint32_t)req[req_idx]            |
                           ((uint32_t)req[req_idx + 1] << 8) |
                           ((uint32_t)req[req_idx + 2] << 16)|
                           ((uint32_t)req[req_idx + 3] << 24);
            req_idx += 4u;
            done++;
            continue;
        }

        uint32_t data_in = 0;
        uint32_t expected_match = 0;
        if (!is_read) {
            if ((uint16_t)(req_idx + 4u) > req_avail) { break; }
            data_in = (uint32_t)req[req_idx]            |
                      ((uint32_t)req[req_idx + 1] << 8) |
                      ((uint32_t)req[req_idx + 2] << 16)|
                      ((uint32_t)req[req_idx + 3] << 24);
            req_idx += 4u;
        } else if (vm) {
            if ((uint16_t)(req_idx + 4u) > req_avail) { break; }
            expected_match = (uint32_t)req[req_idx]            |
                             ((uint32_t)req[req_idx + 1] << 8) |
                             ((uint32_t)req[req_idx + 2] << 16)|
                             ((uint32_t)req[req_idx + 3] << 24);
            req_idx += 4u;
        }

        /* WAIT-retry loop: spec says the probe retries internally up
         * to dap_tfr_wait_retry times before reporting the final ACK. */
        uint32_t read_val = 0;
        uint32_t wait_left = dap_tfr_wait_retry;
        do {
            uint32_t arg = is_read ? read_val : data_in;
            ack = swd_transfer(tr, &arg);
            if (is_read) {
                read_val = arg;
            }
        } while (ack == DAP_TRANSFER_WAIT && (wait_left-- > 0u));

        if (ack != DAP_TRANSFER_OK) {
            /* Stop the batch on any non-OK ACK — that's what hosts
             * expect. The accumulated count + ack at the end of the
             * response tells the host where we stopped. */
            break;
        }

        if (vm) {
            /* MATCH_MASK semantics: keep re-reading until
             * (value & mask) == (expected & mask) or match_retry
             * exhausted. */
            uint32_t match_left = dap_tfr_match_retry;
            while ((read_val & s_match_mask) != (expected_match & s_match_mask)) {
                if (match_left == 0u) {
                    ack |= DAP_TRANSFER_MISMATCH;
                    break;
                }
                match_left--;
                wait_left = dap_tfr_wait_retry;
                uint32_t arg = read_val;
                do {
                    ack = swd_transfer(tr, &arg);
                } while (ack == DAP_TRANSFER_WAIT && (wait_left-- > 0u));
                if (ack != DAP_TRANSFER_OK) {
                    break;
                }
                read_val = arg;
            }
            if ((ack & ~DAP_TRANSFER_MISMATCH) != DAP_TRANSFER_OK) {
                break;
            }
            /* VM reads don't return the value (spec) — only an ACK. */
        } else if (is_read) {
            if ((uint16_t)(resp_idx + 4u) > resp_cap) {
                break;
            }
            resp[resp_idx++] = (uint8_t)(read_val);
            resp[resp_idx++] = (uint8_t)(read_val >> 8);
            resp[resp_idx++] = (uint8_t)(read_val >> 16);
            resp[resp_idx++] = (uint8_t)(read_val >> 24);
        }

        done++;
    }

    resp[1] = done;
    resp[2] = ack;
    *resp_used = resp_idx;
    return req_idx;
}

/* ------------------------------------------------------------------ */
/*  0x06 DAP_TransferBlock                                             */
/* ------------------------------------------------------------------ */

uint16_t dap_handle_transfer_block(const uint8_t *req, uint16_t req_avail,
                                   uint8_t *resp, uint16_t resp_cap,
                                   uint16_t *resp_used)
{
    if (req_avail < 5u || resp_cap < 4u) {
        *resp_used = 0;
        return 0;
    }
    (void)req[1];   /* dap_index — JTAG only */

    uint16_t count = (uint16_t)((uint16_t)req[2] | ((uint16_t)req[3] << 8));
    uint8_t  tr    = req[4];
    bool     is_read = (tr & TFR_RnW) != 0;
    uint16_t req_idx = 5u;
    uint16_t resp_idx = 4u;
    uint16_t done = 0;
    uint8_t  ack = DAP_TRANSFER_OK;

    if (dap_active_port != EDEV_DAP_PORT_SWD) {
        resp[1] = 0; resp[2] = 0;
        resp[3] = DAP_TRANSFER_ERROR;
        *resp_used = 4u;
        return req_avail;
    }

    for (uint16_t i = 0; i < count; i++) {
        uint32_t data = 0;
        if (!is_read) {
            if ((uint16_t)(req_idx + 4u) > req_avail) { break; }
            data = (uint32_t)req[req_idx]            |
                   ((uint32_t)req[req_idx + 1] << 8) |
                   ((uint32_t)req[req_idx + 2] << 16)|
                   ((uint32_t)req[req_idx + 3] << 24);
            req_idx += 4u;
        }

        uint32_t wait_left = dap_tfr_wait_retry;
        do {
            ack = swd_transfer(tr, &data);
        } while (ack == DAP_TRANSFER_WAIT && (wait_left-- > 0u));

        if (ack != DAP_TRANSFER_OK) {
            break;
        }

        if (is_read) {
            if ((uint16_t)(resp_idx + 4u) > resp_cap) { break; }
            resp[resp_idx++] = (uint8_t)(data);
            resp[resp_idx++] = (uint8_t)(data >> 8);
            resp[resp_idx++] = (uint8_t)(data >> 16);
            resp[resp_idx++] = (uint8_t)(data >> 24);
        }
        done++;
    }

    resp[1] = (uint8_t)(done & 0xFFu);
    resp[2] = (uint8_t)(done >> 8);
    resp[3] = ack;
    *resp_used = resp_idx;
    return req_idx;
}

/* ------------------------------------------------------------------ */
/*  0x07 DAP_TransferAbort                                             */
/* ------------------------------------------------------------------ */

uint16_t dap_handle_transfer_abort(const uint8_t *req, uint16_t req_avail,
                                   uint8_t *resp, uint16_t resp_cap,
                                   uint16_t *resp_used)
{
    /* Spec: TransferAbort cancels an in-flight DAP_Transfer/Block,
     * and is *async* — it shouldn't be bundled in an ExecuteCommands.
     * Our dispatcher is single-threaded; by the time we see the abort
     * command the previous transfer has already completed (we drained
     * the request ring sequentially). So this is effectively a no-op.
     * We emit a status byte for well-formedness. */
    (void)req;
    if (req_avail < 1u || resp_cap < 2u) {
        *resp_used = 0;
        return 0;
    }
    resp[1] = 0;
    *resp_used = 2u;
    return 1u;
}

/* ------------------------------------------------------------------ */
/*  0x08 DAP_WriteABORT                                                */
/* ------------------------------------------------------------------ */

uint16_t dap_handle_write_abort(const uint8_t *req, uint16_t req_avail,
                                uint8_t *resp, uint16_t resp_cap,
                                uint16_t *resp_used)
{
    if (req_avail < 6u || resp_cap < 2u) {
        *resp_used = 0;
        return 0;
    }
    (void)req[1];  /* dap_index — JTAG */
    uint32_t abort = (uint32_t)req[2]            |
                     ((uint32_t)req[3] << 8)     |
                     ((uint32_t)req[4] << 16)    |
                     ((uint32_t)req[5] << 24);

    if (dap_active_port != EDEV_DAP_PORT_SWD) {
        resp[1] = DAP_TRANSFER_ERROR;
        *resp_used = 2u;
        return 6u;
    }

    /* The DP ABORT register is at address 0 in the DP. A direct write
     * (no read of CTRL/STAT) per spec. We use DP, write, A2=0, A3=0
     * → request byte = 0 (all four lower bits zero, RnW=0, APnDP=0,
     * A2=0, A3=0). */
    uint32_t request = 0u;       /* DP / write / A2=0 / A3=0 */
    uint8_t ack = swd_transfer(request, &abort);

    resp[1] = (ack == DAP_TRANSFER_OK) ? 0u : DAP_TRANSFER_ERROR;
    *resp_used = 2u;
    return 6u;
}

/* ------------------------------------------------------------------ */
/*  0x1D DAP_SWD_Sequence                                              */
/* ------------------------------------------------------------------ */

uint16_t dap_handle_swd_sequence(const uint8_t *req, uint16_t req_avail,
                                 uint8_t *resp, uint16_t resp_cap,
                                 uint16_t *resp_used)
{
    if (req_avail < 2u || resp_cap < 2u) {
        *resp_used = 0;
        return 0;
    }
    uint8_t seq_count = req[1];
    uint16_t req_idx = 2u;
    uint16_t resp_idx = 2u;

    for (uint8_t i = 0; i < seq_count; i++) {
        if (req_idx >= req_avail) {
            *resp_used = 0;
            return 0;
        }
        uint8_t info = req[req_idx++];
        uint32_t bits = info & 0x3Fu;
        if (bits == 0u) { bits = 64u; }
        bool is_input = (info & 0x80u) != 0;
        uint32_t bytes = (bits + 7u) / 8u;

        if (is_input) {
            if ((uint16_t)(resp_idx + bytes) > resp_cap) {
                *resp_used = 0;
                return 0;
            }
            uint32_t remaining = bits;
            uint32_t out_offset = 0;
            while (remaining > 0u) {
                uint32_t chunk = remaining > 32u ? 32u : remaining;
                uint32_t v = probe_read_bits(chunk);
                /* Pack v back into resp bytes LSB-first. */
                uint32_t chunk_bytes = (chunk + 7u) / 8u;
                for (uint32_t b = 0; b < chunk_bytes; b++) {
                    resp[resp_idx + out_offset + b] = (uint8_t)(v >> (8u * b));
                }
                out_offset += chunk_bytes;
                remaining  -= chunk;
            }
            resp_idx = (uint16_t)(resp_idx + bytes);
        } else {
            if ((uint16_t)(req_idx + bytes) > req_avail) {
                *resp_used = 0;
                return 0;
            }
            uint32_t remaining = bits;
            uint32_t in_offset = 0;
            while (remaining > 0u) {
                uint32_t chunk = remaining > 32u ? 32u : remaining;
                uint32_t chunk_bytes = (chunk + 7u) / 8u;
                uint32_t word = 0;
                for (uint32_t b = 0; b < chunk_bytes; b++) {
                    word |= ((uint32_t)req[req_idx + in_offset + b]) << (8u * b);
                }
                probe_write_bits(chunk, word);
                in_offset += chunk_bytes;
                remaining -= chunk;
            }
            req_idx = (uint16_t)(req_idx + bytes);
        }
    }

    resp[1] = 0;
    *resp_used = resp_idx;
    return req_idx;
}
