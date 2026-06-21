/*
 * dap_swo.c — SWO trace commands. Wires up to src/hw/swo.* which owns
 * the PIO state machine and the ring buffer.
 *
 *   0x17 DAP_SWO_Transport         — DAP polled vs bulk-EP streamed
 *   0x18 DAP_SWO_Mode              — off / UART / Manchester
 *   0x19 DAP_SWO_Baudrate          — request / return achievable baud
 *   0x1A DAP_SWO_Control           — start / stop capture
 *   0x1B DAP_SWO_Status            — capture status + bytes available
 *   0x1C DAP_SWO_Data              — read trace bytes (polled mode)
 *   0x1E DAP_SWO_ExtendedStatus    — extended status + timestamp
 *
 * We support UART mode only (capabilities bit 2). Manchester (bit 3)
 * is reserved.
 *
 * Transport selection is informational on our side — the ring buffer
 * is always populated, and EP 0x82 only fires if the host asked for
 * streamed mode. Polled-mode hosts use DAP_SWO_Data and read the same
 * ring.
 */

#include "dap_internal.h"

#include <string.h>

#include "hw/swo.h"

/* Captured config — the spec is light on whether we need to actually
 * enforce e.g. mode=off blocking start; we honour the host's view and
 * report capture status accordingly. */
static struct {
    uint8_t  transport;     /* 0 = none, 1 = polled (DAP_SWO_Data), 2 = streamed (bulk EP) */
    uint8_t  mode;          /* 0 = off, 1 = UART, 2 = Manchester (unsupported) */
} s_swo_cfg = { 0 };

void dap_swo_init(void) { /* swo_init is called from main.c::main */ }

/* ------------------------------------------------------------------ */

static uint8_t status_byte(void)
{
    /* Bit 0 = capture active
     * Bit 6 = (reserved — stream error, set on bulk EP failure; unused)
     * Bit 7 = buffer overrun */
    uint8_t v = 0;
    if (swo_is_active())   v |= 0x01u;
    if (swo_has_overrun()) v |= 0x80u;
    return v;
}

/* ------------------------------------------------------------------ */

uint16_t dap_handle_swo(const uint8_t *req, uint16_t req_avail,
                        uint8_t *resp, uint16_t resp_cap,
                        uint16_t *resp_used)
{
    if (req_avail < 1u || resp_cap < 2u) {
        *resp_used = 0;
        return 0;
    }

    uint8_t cmd = req[0];

    switch (cmd) {
        case DAP_CMD_SWO_TRANSPORT:
            if (req_avail < 2u) { *resp_used = 0; return 0; }
            s_swo_cfg.transport = req[1];
            resp[1] = 0;
            *resp_used = 2u;
            return 2u;

        case DAP_CMD_SWO_MODE:
            if (req_avail < 2u) { *resp_used = 0; return 0; }
            /* Manchester is not supported — quietly accept anything
             * but only "1" actually enables capture. Hosts query
             * capabilities first, so they shouldn't ask for 2. */
            s_swo_cfg.mode = req[1];
            resp[1] = 0;
            *resp_used = 2u;
            return 2u;

        case DAP_CMD_SWO_BAUDRATE: {
            if (req_avail < 5u || resp_cap < 5u) { *resp_used = 0; return 0; }
            uint32_t requested = (uint32_t)req[1]            |
                                 ((uint32_t)req[2] << 8)     |
                                 ((uint32_t)req[3] << 16)    |
                                 ((uint32_t)req[4] << 24);
            uint32_t actual = swo_set_baudrate(requested);
            resp[1] = (uint8_t)(actual);
            resp[2] = (uint8_t)(actual >> 8);
            resp[3] = (uint8_t)(actual >> 16);
            resp[4] = (uint8_t)(actual >> 24);
            *resp_used = 5u;
            return 5u;
        }

        case DAP_CMD_SWO_CONTROL:
            if (req_avail < 2u) { *resp_used = 0; return 0; }
            if (req[1] == 1u) {
                if (s_swo_cfg.mode == 1u) {
                    swo_start();
                }
            } else {
                swo_stop();
            }
            resp[1] = 0;
            *resp_used = 2u;
            return 2u;

        case DAP_CMD_SWO_STATUS: {
            /* resp[1] = status, resp[2..5] = byte count LE32 */
            if (resp_cap < 6u) { *resp_used = 0; return 0; }
            uint32_t avail = swo_bytes_available();
            resp[1] = status_byte();
            resp[2] = (uint8_t)(avail);
            resp[3] = (uint8_t)(avail >> 8);
            resp[4] = (uint8_t)(avail >> 16);
            resp[5] = (uint8_t)(avail >> 24);
            *resp_used = 6u;
            return 1u;
        }

        case DAP_CMD_SWO_DATA: {
            /* req[1..2] = max bytes LE16
             * resp[1]    = status
             * resp[2..3] = byte count LE16
             * resp[4..]  = data */
            if (req_avail < 3u || resp_cap < 4u) { *resp_used = 0; return 0; }
            uint16_t max = (uint16_t)((uint16_t)req[1] | ((uint16_t)req[2] << 8));
            /* Cap by response slot capacity. */
            uint16_t slot = (uint16_t)(resp_cap - 4u);
            if (max > slot) {
                max = slot;
            }
            uint32_t got = swo_read(&resp[4], max);
            resp[1] = status_byte();
            resp[2] = (uint8_t)(got);
            resp[3] = (uint8_t)(got >> 8);
            *resp_used = (uint16_t)(4u + got);
            return 3u;
        }

        case DAP_CMD_SWO_EXTENDED_STATUS: {
            /* req[1] = control bits (we always send the full set —
             * status + trace count + index + timestamp).
             *
             * Response layout when all fields are present:
             *   resp[1]      = status
             *   resp[2..5]   = trace count LE32
             *   resp[6..9]   = trace index LE32  (running total of bytes captured)
             *   resp[10..13] = TD timestamp LE32 (we have no TD timer; 0) */
            if (req_avail < 2u || resp_cap < 14u) { *resp_used = 0; return 0; }
            uint32_t avail = swo_bytes_available();
            resp[1] = status_byte();
            resp[2] = (uint8_t)(avail);       resp[3] = (uint8_t)(avail >> 8);
            resp[4] = (uint8_t)(avail >> 16); resp[5] = (uint8_t)(avail >> 24);
            /* Use the same value for "index" — we don't track total
             * captured bytes separately; some hosts inspect the gap
             * between status calls to gauge throughput so a monotonic
             * value would be nicer, but spec just says "trace index"
             * with no strict definition. */
            resp[6] = resp[2]; resp[7] = resp[3];
            resp[8] = resp[4]; resp[9] = resp[5];
            resp[10] = 0; resp[11] = 0; resp[12] = 0; resp[13] = 0;
            *resp_used = 14u;
            return 2u;
        }

        default:
            resp[0] = 0xFFu;
            *resp_used = 1u;
            return 1u;
    }
}
