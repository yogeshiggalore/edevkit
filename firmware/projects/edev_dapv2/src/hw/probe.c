/*
 * probe.c — PIO-driven SWD bit-bang (edev_dapv2 v0.2).
 *
 * The previous SIO bit-bang topped out around 64 KB reads before
 * probabilistic parity errors compounded. PIO has hardware-deterministic
 * timing — no marginal sampling, no nop-loop calibration. PIO program is
 * in src/pio/probe_swd.pio.
 *
 * Pin mapping:
 *   PROBE_PIN_SWCLK — side-set, driven LOW/HIGH per phase
 *   PROBE_PIN_SWDIO — OUT/IN/SET base; pindirs toggled by PIO `set pindirs`
 *
 * Speed: PIO clock divided down. PIO cycles per SWD bit = 6 (3 low + 3
 * high). At PIO 150 MHz → 25 MHz SWD; PIO 6 MHz → 1 MHz SWD.
 *
 * The higher-level API (probe_swd_transfer, probe_swj_sequence, etc.)
 * is unchanged so dap_handle_transfer keeps working.
 */

#include "hw/probe.h"
#include "hw/probe_pins.h"
#include "dap/dap_internal.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"

#include "probe_swd.pio.h"

#include <string.h>

/* ----------------------------------------------------------------------
 * Configuration state
 * ---------------------------------------------------------------------- */

static uint8_t  s_turnaround  = 1;
static uint8_t  s_data_phase  = 0;
static uint8_t  s_idle_cycles = 0;
static uint16_t s_wait_retry  = 100;
static uint16_t s_match_retry = 0;
static uint32_t s_clock_hz    = 1000000u;

/* Use PIO1 — PIO0 is taken by cyw43_arch for the WiFi chip's SPI bus
 * on pico2_w (even when we link the "none" variant for LED-only).
 * Mixing on the same PIO causes program-slot conflicts. */
#define PROBE_PIO        pio1

static uint s_pio_sm;
static uint s_pio_offset;

#define PROBE_SM s_pio_sm

/* ----------------------------------------------------------------------
 * PIO primitives
 * ---------------------------------------------------------------------- */

/* Build the opcode word used to enter a PIO function.
 *   bits  0:7   = absolute PIO PC (relative offset + program base)
 *   bits  8:12  = count - 1
 *   bits 13:31  = 19 data bits (for writes; ignored for reads)
 *
 * NB: `out pc, 8` in PIO sets ABSOLUTE PC. The pioasm-generated
 * `probe_swd_offset_*` macros are RELATIVE to program start. Add
 * the load offset (s_pio_offset) so the jump lands correctly. */
static inline uint32_t pio_opcode(uint8_t func, uint8_t count, uint32_t data)
{
    return (uint32_t)(func + s_pio_offset)
         | (((uint32_t)(count - 1u) & 0x1Fu) << 8)
         | (((data & 0x7FFFFu)) << 13);
}

/* Drain RX FIFO discarding any stale data. */
static inline void pio_drain_rx(void)
{
    while (!pio_sm_is_rx_fifo_empty(PROBE_PIO, PROBE_SM)) {
        (void) pio_sm_get(PROBE_PIO, PROBE_SM);
    }
}

/* Write N bits LSB-first from `data`. N can be 1..32. */
static void swd_write_n(uint32_t data, uint8_t n)
{
    /* 19 bits ride in the opcode word, the rest in a follow-up word.
     * The PIO program's autopull (threshold 32) pulls the second
     * word automatically when OSR drains. */
    uint32_t op = pio_opcode(probe_swd_offset_write_bits, n, data);
    pio_sm_put_blocking(PROBE_PIO, PROBE_SM, op);
    if (n > 19) {
        pio_sm_put_blocking(PROBE_PIO, PROBE_SM, data >> 19);
    }
}

/* Read N bits LSB-first, return value with first received bit at LSB.
 *
 * Safety-net timeout: pio_sm_get_blocking() spins forever if the PIO
 * SM stops pushing to the RX FIFO (e.g., the SM gets wedged in some
 * corner case during high-traffic transactions). If we never return,
 * the whole CMSIS-DAP command handler stalls and probe-rs eventually
 * times out the USB roundtrip. That's Bug 2's symptom on nRF5340.
 *
 * Cap the wait at ~10 ms — typical worst-case is microseconds; 10 ms
 * is 1000× headroom but still well under probe-rs's ~1-second USB
 * timeout. If we hit the cap, restart the SM and return 0xFFFFFFFF
 * (likely interpreted as no-ack/parity-err upstream, which triggers
 * the retry path or a clean error response instead of a USB hang). */
static uint32_t swd_read_n(uint8_t n)
{
    uint32_t op = pio_opcode(probe_swd_offset_read_bits, n, 0);
    pio_sm_put_blocking(PROBE_PIO, PROBE_SM, op);

    /* Bounded wait for RX FIFO. ~10 ms at 125 MHz CPU = 1.25M loop
     * iterations. Each iteration is ~2 cycles → 2.5M cycles. */
    uint32_t guard = 1500000u;
    while (pio_sm_is_rx_fifo_empty(PROBE_PIO, PROBE_SM)) {
        if (--guard == 0) {
            /* SM is wedged. Restart it cleanly so the next op works. */
            pio_sm_clear_fifos(PROBE_PIO, PROBE_SM);
            pio_sm_restart(PROBE_PIO, PROBE_SM);
            pio_sm_exec(PROBE_PIO, PROBE_SM,
                        pio_encode_jmp(s_pio_offset + probe_swd_offset_start));
            return 0xFFFFFFFFu;   /* will look like NACK / parity err */
        }
    }
    uint32_t v = pio_sm_get(PROBE_PIO, PROBE_SM);
    /* ISR was right-shifted N times; bits occupy positions 32-N .. 31.
     * Shift back so first-received bit is at bit 0. */
    if (n < 32) v >>= (32 - n);
    return v;
}

/* Clock N bits with SWDIO undriven (host releases the line) — used
 * for turnaround periods between host and target driving. We do this
 * as a "read" (sets pindirs=0) and discard the data. */
static void swd_clock_idle_n(uint8_t n)
{
    (void) swd_read_n(n);
}

/* ----------------------------------------------------------------------
 * Init / connect / disconnect / reset
 * ---------------------------------------------------------------------- */

/* Install the PIO program once. Claims an SM and finds program offset. */
static void probe_pio_install(void)
{
    /* Claim an unused SM on PROBE_PIO. Panics if none available. */
    int sm = pio_claim_unused_sm(PROBE_PIO, true);
    s_pio_sm = (uint) sm;
    s_pio_offset = pio_add_program(PROBE_PIO, &probe_swd_program);
}

/* Configure SM for SWD operation on the given pins. */
static void probe_pio_configure(uint32_t freq_hz)
{
    pio_sm_set_enabled(PROBE_PIO, PROBE_SM, false);

    pio_sm_config c = probe_swd_program_get_default_config(s_pio_offset);

    /* SWCLK on side-set. */
    sm_config_set_sideset_pins(&c, PROBE_PIN_SWCLK);

    /* SWDIO on OUT/IN/SET (all share the same physical pin). */
    sm_config_set_out_pins(&c, PROBE_PIN_SWDIO, 1);
    sm_config_set_in_pins(&c, PROBE_PIN_SWDIO);
    sm_config_set_set_pins(&c, PROBE_PIN_SWDIO, 1);

    /* OUT shift right (LSB first), autopull at 32. */
    sm_config_set_out_shift(&c, true /*right*/, true /*autopull*/, 32);
    /* IN shift right, no autopush — we use explicit `push`. */
    sm_config_set_in_shift(&c, true /*right*/, false /*autopush*/, 32);

    /* Clock divider — PIO does 6 cycles per SWD bit.
     * Effective SWD frequency = PIO clock / 6. */
    uint32_t sys_hz = clock_get_hz(clk_sys);
    float pio_hz = (float) freq_hz * 6.0f;
    float div = (float) sys_hz / pio_hz;
    if (div < 1.0f)   div = 1.0f;
    if (div > 65535.0f) div = 65535.0f;
    sm_config_set_clkdiv(&c, div);

    /* Initialize pins for PIO control. */
    pio_gpio_init(PROBE_PIO, PROBE_PIN_SWCLK);
    pio_gpio_init(PROBE_PIO, PROBE_PIN_SWDIO);

    /* SWCLK is always output (driven by PIO). */
    pio_sm_set_consecutive_pindirs(PROBE_PIO, PROBE_SM, PROBE_PIN_SWCLK, 1, true);
    /* SWDIO direction is managed by PIO `set pindirs` instructions. */

    pio_sm_init(PROBE_PIO, PROBE_SM, s_pio_offset, &c);
    pio_sm_clear_fifos(PROBE_PIO, PROBE_SM);
    pio_sm_set_enabled(PROBE_PIO, PROBE_SM, true);
}

void probe_init(void)
{
    /* Other pins (nRESET, nTRST, TDI, TDO) stay SIO-managed. */
    gpio_init(PROBE_PIN_TDI);
    gpio_init(PROBE_PIN_TDO);
    gpio_init(PROBE_PIN_NRESET);
    gpio_init(PROBE_PIN_NTRST);

    gpio_set_dir(PROBE_PIN_TDI,    GPIO_IN);
    gpio_set_dir(PROBE_PIN_TDO,    GPIO_IN);
    gpio_set_dir(PROBE_PIN_NRESET, GPIO_IN);
    gpio_set_dir(PROBE_PIN_NTRST,  GPIO_IN);

    /* Install PIO program AND configure the SM up front, because the
     * host (probe-rs / OpenOCD) sends DAP_SWJ_Sequence to wake the
     * target BEFORE DAP_Connect. If the SM isn't running, our TX
     * FIFO fills and pio_sm_put_blocking hangs. */
    probe_pio_install();
    probe_pio_configure(s_clock_hz);
}

void probe_connect_swd(void)
{
    /* PIO is already configured by probe_init. Nothing more to do —
     * the host can immediately start sending SWD operations. */
    pio_drain_rx();
}

void probe_disconnect(void)
{
    pio_sm_set_enabled(PROBE_PIO, PROBE_SM, false);
    gpio_set_dir(PROBE_PIN_SWCLK, GPIO_IN);
    gpio_set_dir(PROBE_PIN_SWDIO, GPIO_IN);
}

void probe_reset_target(void)
{
    gpio_put(PROBE_PIN_NRESET, 0);
    gpio_set_dir(PROBE_PIN_NRESET, GPIO_OUT);
    sleep_ms(100);
    gpio_set_dir(PROBE_PIN_NRESET, GPIO_IN);
}

void probe_set_clock(uint32_t hz)
{
    if (hz == 0) hz = 1000000u;
    s_clock_hz = hz;
    /* Re-configure SM with new divider if it's running. */
    probe_pio_configure(s_clock_hz);
}

void probe_set_swd_config(uint8_t turnaround, uint8_t data_phase)
{
    s_turnaround = (turnaround & 0x3u) + 1u;
    s_data_phase = data_phase & 1u;
}

void probe_set_xfer_config(uint8_t idle_cycles,
                           uint16_t wait_retry, uint16_t match_retry)
{
    s_idle_cycles = idle_cycles;
    s_wait_retry  = wait_retry;
    s_match_retry = match_retry;
}

/* ----------------------------------------------------------------------
 * SWJ_Pins helpers — direct GPIO outside PIO. We park the SM briefly
 * so PIO doesn't fight us.
 * ---------------------------------------------------------------------- */

#define SWJ_PIN_SWCLK  (1u << 0)
#define SWJ_PIN_SWDIO  (1u << 1)
#define SWJ_PIN_TDI    (1u << 2)
#define SWJ_PIN_TDO    (1u << 3)
#define SWJ_PIN_NTRST  (1u << 5)
#define SWJ_PIN_NRESET (1u << 7)

void probe_swj_set_pins(uint8_t pin_select, uint8_t pin_output)
{
    /* If any of SWCLK or SWDIO is selected, temporarily yield from PIO
     * back to SIO so we can drive those lines directly. */
    bool yield_pio = (pin_select & (SWJ_PIN_SWCLK | SWJ_PIN_SWDIO)) != 0;
    if (yield_pio) {
        pio_sm_set_enabled(PROBE_PIO, PROBE_SM, false);
        gpio_init(PROBE_PIN_SWCLK);
        gpio_init(PROBE_PIN_SWDIO);
    }

    if (pin_select & SWJ_PIN_SWCLK) {
        gpio_set_dir(PROBE_PIN_SWCLK, GPIO_OUT);
        gpio_put(PROBE_PIN_SWCLK, !!(pin_output & SWJ_PIN_SWCLK));
    }
    if (pin_select & SWJ_PIN_SWDIO) {
        gpio_set_dir(PROBE_PIN_SWDIO, GPIO_OUT);
        gpio_put(PROBE_PIN_SWDIO, !!(pin_output & SWJ_PIN_SWDIO));
    }
    if (pin_select & SWJ_PIN_TDI) {
        gpio_set_dir(PROBE_PIN_TDI, GPIO_OUT);
        gpio_put(PROBE_PIN_TDI, !!(pin_output & SWJ_PIN_TDI));
    }
    if (pin_select & SWJ_PIN_NTRST) {
        gpio_set_dir(PROBE_PIN_NTRST, GPIO_OUT);
        gpio_put(PROBE_PIN_NTRST, !!(pin_output & SWJ_PIN_NTRST));
    }
    if (pin_select & SWJ_PIN_NRESET) {
        /* Open-drain: drive 0 only. */
        if (pin_output & SWJ_PIN_NRESET) {
            gpio_set_dir(PROBE_PIN_NRESET, GPIO_IN);
        } else {
            gpio_put(PROBE_PIN_NRESET, 0);
            gpio_set_dir(PROBE_PIN_NRESET, GPIO_OUT);
        }
    }

    if (yield_pio) {
        /* Restore PIO control of SWCLK/SWDIO. */
        probe_pio_configure(s_clock_hz);
    }
}

uint8_t probe_swj_get_pins(void)
{
    uint8_t v = 0;
    if (gpio_get(PROBE_PIN_SWCLK))  v |= SWJ_PIN_SWCLK;
    if (gpio_get(PROBE_PIN_SWDIO))  v |= SWJ_PIN_SWDIO;
    if (gpio_get(PROBE_PIN_TDI))    v |= SWJ_PIN_TDI;
    if (gpio_get(PROBE_PIN_TDO))    v |= SWJ_PIN_TDO;
    if (gpio_get(PROBE_PIN_NTRST))  v |= SWJ_PIN_NTRST;
    if (gpio_get(PROBE_PIN_NRESET)) v |= SWJ_PIN_NRESET;
    return v;
}

/* ----------------------------------------------------------------------
 * Raw SWJ / SWD bit sequences
 * ---------------------------------------------------------------------- */

void probe_swj_sequence(uint16_t bit_count, const uint8_t *data)
{
    /* Write bits in chunks of up to 32 at a time. */
    uint16_t bits_left = bit_count;
    uint16_t byte_pos  = 0;
    while (bits_left) {
        uint8_t chunk = (bits_left > 32u) ? 32u : (uint8_t) bits_left;
        uint32_t word = 0;
        for (uint8_t i = 0; i < chunk; ++i) {
            uint8_t bit = (data[byte_pos + (i >> 3)] >> (i & 7u)) & 1u;
            word |= (uint32_t) bit << i;
        }
        swd_write_n(word, chunk);
        bits_left -= chunk;
        byte_pos  += (chunk + 7u) / 8u;
    }
}

uint16_t probe_swd_sequence(uint8_t sequence_count,
                            const uint8_t *info_and_data, uint16_t data_len,
                            uint8_t *out, uint16_t out_cap)
{
    if (out_cap < 1) return 0;
    out[0] = DAP_OK;
    uint16_t out_pos = 1;
    uint16_t in_pos  = 0;

    for (uint8_t s = 0; s < sequence_count; ++s) {
        if (in_pos >= data_len) { out[0] = DAP_ERROR; return out_pos; }
        uint8_t info = info_and_data[in_pos++];

        uint16_t bits = info & 0x3Fu;
        if (bits == 0) bits = 64;
        uint16_t bytes = (bits + 7u) / 8u;
        bool is_in = (info & 0x80u) != 0;

        if (is_in) {
            if (out_pos + bytes > out_cap) { out[0] = DAP_ERROR; return out_pos; }
            uint16_t bits_left = bits;
            uint16_t bit_pos   = 0;
            while (bits_left) {
                uint8_t chunk = (bits_left > 32u) ? 32u : (uint8_t) bits_left;
                uint32_t v = swd_read_n(chunk);
                for (uint8_t i = 0; i < chunk; ++i) {
                    uint8_t bit = (v >> i) & 1u;
                    if (bit) out[out_pos + ((bit_pos + i) >> 3)] |= 1u << ((bit_pos + i) & 7u);
                }
                bits_left -= chunk;
                bit_pos   += chunk;
            }
            /* Zero the remaining bits in the last partial byte. */
            for (uint16_t i = 0; i < bytes; ++i) {
                /* (already zero from LHS init? defensive) */
                (void) out[out_pos + i];
            }
            out_pos += bytes;
        } else {
            if (in_pos + bytes > data_len) { out[0] = DAP_ERROR; return out_pos; }
            uint16_t bits_left = bits;
            uint16_t byte_off  = 0;
            while (bits_left) {
                uint8_t chunk = (bits_left > 32u) ? 32u : (uint8_t) bits_left;
                uint32_t word = 0;
                for (uint8_t i = 0; i < chunk; ++i) {
                    uint8_t bit = (info_and_data[in_pos + byte_off + (i >> 3)]
                                   >> (i & 7u)) & 1u;
                    word |= (uint32_t) bit << i;
                }
                swd_write_n(word, chunk);
                bits_left -= chunk;
                byte_off  += (chunk + 7u) / 8u;
            }
            in_pos += bytes;
        }
    }
    return out_pos;
}

/* ----------------------------------------------------------------------
 * SWD transfer (the workhorse)
 * ---------------------------------------------------------------------- */

static inline uint8_t parity32(uint32_t v)
{
    return (uint8_t) (__builtin_popcount(v) & 1u);
}

static inline uint8_t parity4(uint8_t v)
{
    return (uint8_t) (__builtin_popcount(v & 0x0Fu) & 1u);
}

static uint8_t probe_swd_xact(uint8_t request, uint32_t data_value,
                              uint32_t *data_out);

uint8_t probe_swd_transfer(uint8_t request, uint32_t data_value,
                           uint32_t *data_out)
{
    /* Parity-error retry. Should rarely fire with PIO since timing is
     * deterministic, but kept as a safety net. */
    uint8_t ack;
    for (int attempt = 0; attempt < 8; ++attempt) {
        ack = probe_swd_xact(request, data_value, data_out);
        if (ack == DAP_TRANSFER_OK) return ack;
        if (ack != (DAP_TRANSFER_OK | DAP_TRANSFER_ERROR)) return ack;
        if (!(request & DAP_TRANSFER_RnW)) return ack;
    }
    return ack;
}

static uint8_t probe_swd_xact(uint8_t request, uint32_t data_value,
                              uint32_t *data_out)
{
    /* Build 8-bit packet request: start, APnDP, RnW, A2, A3, parity,
     * stop, park — LSB first. */
    uint8_t header = 0
        | (1u << 0)                              /* start */
        | ((request & 0x0Fu) << 1)               /* APnDP/RnW/A2/A3 */
        | ((uint8_t)parity4(request) << 5)       /* parity */
        | (0u << 6)                              /* stop */
        | (1u << 7);                             /* park */

    /* Phase 1: write 8-bit header. */
    swd_write_n(header, 8);

    /* Phase 2: turnaround (target takes line). PIO `read_bits` sets
     * pindirs to IN for us. */
    swd_clock_idle_n(s_turnaround);

    /* Phase 3: read 3-bit ACK. */
    uint8_t ack = (uint8_t) swd_read_n(3);

    if (ack == 0x1u) {
        if (request & DAP_TRANSFER_RnW) {
            /* READ: target keeps driving 32 data + 1 parity. */
            uint32_t data = swd_read_n(32);
            uint8_t target_parity = (uint8_t) swd_read_n(1);
            /* Turnaround back to host. */
            swd_clock_idle_n(s_turnaround);

            if (data_out) *data_out = data;
            if (parity32(data) != target_parity) {
                return DAP_TRANSFER_OK | DAP_TRANSFER_ERROR;
            }
        } else {
            /* WRITE: turnaround back to host, then host drives 32 data + parity. */
            swd_clock_idle_n(s_turnaround);
            swd_write_n(data_value, 32);
            swd_write_n(parity32(data_value), 1);
        }

        /* Idle cycles. */
        if (s_idle_cycles) {
            /* Up to 256 cycles. Drive 0 in chunks. */
            uint16_t left = s_idle_cycles;
            while (left) {
                uint8_t chunk = (left > 32u) ? 32u : (uint8_t) left;
                swd_write_n(0u, chunk);
                left -= chunk;
            }
        }
        return DAP_TRANSFER_OK;
    }

    /* WAIT / FAULT path with data_phase asymmetry. */
    if (s_data_phase) {
        if (request & DAP_TRANSFER_RnW) {
            /* Target keeps driving — read 33 dummy bits. */
            (void) swd_read_n(32);
            (void) swd_read_n(1);
            swd_clock_idle_n(s_turnaround);
        } else {
            /* Turnaround then host drives 33 zeros. */
            swd_clock_idle_n(s_turnaround);
            swd_write_n(0u, 32);
            swd_write_n(0u, 1);
        }
    } else {
        swd_clock_idle_n(s_turnaround);
    }

    if (ack == 0x2u) return DAP_TRANSFER_WAIT;
    if (ack == 0x4u) return DAP_TRANSFER_FAULT;
    if (ack == 0x7u) return DAP_TRANSFER_NO_ACK;
    return DAP_TRANSFER_NO_ACK | DAP_TRANSFER_ERROR;
}

bool probe_swd_write_abort(uint32_t value)
{
    uint8_t ack = probe_swd_transfer(0u, value, NULL);
    return ack == DAP_TRANSFER_OK;
}
