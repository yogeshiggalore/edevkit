/*
 * probe.c — SWD bit-bang shim. Sits on top of probe.pio (loaded via
 * pio_add_program) and exposes the four primitives the DAP transfer
 * code needs:
 *
 *   probe_write_bits  → "drive SWDIO out, toggle SWCLK, shift N bits"
 *   probe_read_bits   → "tristate SWDIO, toggle SWCLK, sample N bits"
 *   probe_hiz_clocks  → "drive nothing, toggle SWCLK N times"
 *   probe_set_swclk_freq_khz → "retune the SM clock divider"
 *
 * The PIO program reads 32-bit control words from the TX FIFO; this
 * file translates the high-level calls into the right control-word
 * format. See probe.pio for the bit layout.
 */

#include "probe.h"
#include "jtag.h"
#include "probe_pins.h"

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"

#include "probe.pio.h"
#include "probe_keepalive.pio.h"

/* ------------------------------------------------------------------ */
/*  Keep-alive SM — independent of EDEV_PROBE_SM (which runs probe.pio).  */
/* ------------------------------------------------------------------ */

#ifndef EDEV_PROBE_KEEPALIVE_SM
#define EDEV_PROBE_KEEPALIVE_SM   1u
#endif

/* ------------------------------------------------------------------ */

static struct {
    uint     pio_offset;
    bool     initialised;
    bool     pio_owns_pins;
    uint32_t current_freq_khz;
} s = {
    .initialised = false,
    .pio_owns_pins = false,
};

static struct {
    uint     pio_offset;
    bool     loaded;             /* PIO program is in memory */
    bool     globally_enabled;   /* set via probe_keepalive_set_enabled */
    bool     running;            /* SM currently driving SWCLK */
} s_ka = {
    .loaded = false,
    .globally_enabled = true,    /* default ON */
    .running = false,
};

/* Command-word builder. The dispatch addresses come from the generated
 * pio header (probe_offset_*). bit_count == 256 wraps to 0 and is
 * fine because the SM does `out x, 8` (the top bits past 256 are not
 * encodable). count == 0 is forbidden — caller checks. */
typedef enum {
    CMD_WRITE,
    CMD_READ,
    CMD_TURNAROUND,
    CMD_FETCH_ONLY,
} cmd_kind_t;

static inline uint32_t make_cmd(uint32_t bit_count, bool drive, cmd_kind_t kind)
{
    uint dst;
    switch (kind) {
        case CMD_WRITE:      dst = s.pio_offset + probe_offset_write_cmd;     break;
        case CMD_READ:       dst = s.pio_offset + probe_offset_read_cmd;      break;
        case CMD_TURNAROUND: dst = s.pio_offset + probe_offset_turnaround_cmd;break;
        default:             dst = s.pio_offset + probe_offset_fetch_cmd;     break;
    }
    /* The PIO program does `out x, 8` consuming the bottom 8 bits as
     * count - 1. So we encode (bit_count - 1) in the low byte. */
    uint32_t count_field = (bit_count - 1u) & 0xFFu;
    uint32_t oe_field    = drive ? (1u << 8) : 0u;
    uint32_t pc_field    = ((uint32_t)dst & 0x1Fu) << 9;
    return count_field | oe_field | pc_field;
}

/* ------------------------------------------------------------------ */
/*  init / deinit                                                      */
/* ------------------------------------------------------------------ */

void probe_init(void)
{
    if (s.initialised) {
        return;
    }

    /* GPIO + PIO program load, courtesy of the c-sdk helpers emitted
     * into probe.pio.h. */
    edev_probe_gpio_init();
    s.pio_offset = pio_add_program(pio0, &probe_program);

    pio_sm_config cfg = probe_program_get_default_config(s.pio_offset);
    edev_probe_sm_init(&cfg);
    pio_sm_init(pio0, EDEV_PROBE_SM, s.pio_offset, &cfg);

    probe_set_swclk_freq_khz(EDEV_PROBE_DEFAULT_SWCLK_KHZ);

    /* Start the SM at fetch_cmd — the first thing it should do on boot
     * is wait for a command word from the FIFO. */
    pio_sm_exec(pio0, EDEV_PROBE_SM, s.pio_offset + probe_offset_fetch_cmd);
    pio_sm_set_enabled(pio0, EDEV_PROBE_SM, true);

    s.pio_owns_pins = true;
    s.initialised   = true;

    /* Load keep-alive PIO program into the same PIO0 bank (probe.pio
     * uses ~10 instruction slots; keepalive uses 2 — plenty of room).
     * The SM stays DISABLED until the first probe_*_bits call returns,
     * after which keepalive_takeover() activates it. */
    if (!s_ka.loaded) {
        s_ka.pio_offset = pio_add_program(pio0, &probe_keepalive_program);
        edev_probe_keepalive_sm_init(pio0, EDEV_PROBE_KEEPALIVE_SM, s_ka.pio_offset);
        s_ka.loaded = true;
    }

    /* JTAG pad init (TDI/TDO) — pins are SIO-owned even when SWD is
     * active, so the host can probe them via DAP_SWJ_Pins safely. */
    jtag_init();
}

void probe_deinit(void)
{
    if (!s.initialised) {
        return;
    }
    pio_sm_set_enabled(pio0, EDEV_PROBE_SM, false);
    pio_remove_program(pio0, &probe_program, s.pio_offset);
    s.initialised = false;
    s.pio_owns_pins = false;
}

/* ------------------------------------------------------------------ */
/*  clock divider                                                      */
/* ------------------------------------------------------------------ */

void probe_set_swclk_freq_khz(uint32_t freq_khz)
{
    if (!s.initialised) {
        s.current_freq_khz = freq_khz;
        return;
    }
    if (freq_khz == 0u) {
        freq_khz = 1u;
    }

    /* The PIO program emits one SWCLK transition every 2 PIO cycles
     * (out + delay [1] = 2 cycles for SWCLK low, then jmp + delay [1]
     * = 2 cycles for SWCLK high) → SWCLK period = 4 PIO cycles. So
     * the desired PIO clock is 4 × SWCLK frequency. */
    uint32_t clk_sys_khz = clock_get_hz(clk_sys) / 1000u;
    uint32_t divider = (clk_sys_khz + (freq_khz * 4u) - 1u) / (freq_khz * 4u);
    if (divider == 0u)     divider = 1u;
    if (divider > 65535u)  divider = 65535u;

    pio_sm_set_clkdiv_int_frac(pio0, EDEV_PROBE_SM, (uint16_t)divider, 0);
    s.current_freq_khz = freq_khz;
}

/* ------------------------------------------------------------------ */
/*  bit transfer primitives                                            */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  Keep-alive helpers — see probe_keepalive.pio for theory of ops.    */
/* ------------------------------------------------------------------ */

static void keepalive_relinquish(void)
{
    /* Stop the keep-alive SM (if running) and leave SWCLK in a known
     * LOW state. Called whenever we're about to hand SWCLK back to the
     * probe SM for a real transaction.
     *
     * The SM might have been paused mid-`nop side 1` (SWCLK high). To
     * guarantee LOW regardless, briefly switch SWCLK to SIO output and
     * drive it low directly. The next call to ensure_pio_pin_ownership
     * reassigns the pin back to PIO via pio_gpio_init. */
    if (!s_ka.running) {
        return;
    }
    pio_sm_set_enabled(pio0, EDEV_PROBE_KEEPALIVE_SM, false);
    gpio_set_function(EDEV_PROBE_PIN_SWCLK, GPIO_FUNC_SIO);
    gpio_set_dir(EDEV_PROBE_PIN_SWCLK, GPIO_OUT);
    gpio_put(EDEV_PROBE_PIN_SWCLK, 0);
    s_ka.running = false;
}

static void wait_probe_sm_idle(void)
{
    /* Drain the probe SM back to fetch_cmd (its idle state, side-set
     * holds SWCLK low while it stalls on `pull`). After this returns,
     * it's safe to disable the SM and hand off to keep-alive. */
    if (!s.pio_owns_pins) {
        return;
    }
    while (pio_sm_get_tx_fifo_level(pio0, EDEV_PROBE_SM) > 0) {
        tight_loop_contents();
    }
    /* Spin briefly for the SM to reach fetch_cmd. Bounded — if the SM
     * is wedged elsewhere we don't want to hang the USB stack. */
    uint32_t fetch_pc = s.pio_offset + probe_offset_fetch_cmd;
    for (uint32_t spin = 0; spin < 10000u; ++spin) {
        if (pio_sm_get_pc(pio0, EDEV_PROBE_SM) == fetch_pc) {
            break;
        }
        tight_loop_contents();
    }
}

static void keepalive_takeover(void)
{
    /* Disable the probe SM, leave SWCLK at side-set 0 (its fetch_cmd
     * state), then enable the keep-alive SM. This is the inverse of
     * keepalive_relinquish + ensure_pio_pin_ownership combined. */
    if (!s_ka.globally_enabled || !s_ka.loaded) {
        return;
    }
    /* Make sure the probe SM finished its last transaction. */
    wait_probe_sm_idle();
    /* Disable the probe SM so it stops driving SWCLK low. */
    pio_sm_set_enabled(pio0, EDEV_PROBE_SM, false);
    s.pio_owns_pins = false;
    /* Reset keep-alive SM to start of program, then enable. */
    pio_sm_restart(pio0, EDEV_PROBE_KEEPALIVE_SM);
    pio_sm_clkdiv_restart(pio0, EDEV_PROBE_KEEPALIVE_SM);
    pio_sm_exec(pio0, EDEV_PROBE_KEEPALIVE_SM,
                pio_encode_jmp(s_ka.pio_offset));
    pio_sm_set_enabled(pio0, EDEV_PROBE_KEEPALIVE_SM, true);
    s_ka.running = true;
}

void probe_keepalive_set_enabled(bool enable)
{
    s_ka.globally_enabled = enable;
    if (!enable && s_ka.running) {
        keepalive_relinquish();
        /* No probe transaction in flight, so leave SWCLK low quietly. */
    }
}

bool probe_keepalive_is_enabled(void)
{
    return s_ka.globally_enabled;
}

/* ------------------------------------------------------------------ */

static inline void ensure_pio_pin_ownership(void)
{
    /* Always pause keep-alive at the start of any real PIO transaction.
     * This is the inverse of keepalive_takeover() called at the end. */
    keepalive_relinquish();

    if (!s.pio_owns_pins) {
        /* The host has been driving GPIOs via DAP_SWJ_Pins (or we just
         * bit-banged DAP_SWJ_Sequence via SIO); hand them back to PIO.
         *
         * Critically, we also FORCE the SM to restart at fetch_cmd.
         * Without this, if the SM was paused mid-bitloop by a previous
         * release_pio_pins() (which can race with an in-flight write),
         * resuming would pick up half-way through a transaction with a
         * stale FIFO state — producing the "No ACK" signature pyocd
         * was hitting on its first DAP_Transfer after DAP_SWJ_Pins. */
        pio_gpio_init(pio0, EDEV_PROBE_PIN_SWCLK);
        pio_gpio_init(pio0, EDEV_PROBE_PIN_SWDIO);
        pio_sm_set_consecutive_pindirs(pio0, EDEV_PROBE_SM, EDEV_PROBE_PIN_SWCLK, 2, true);

        /* Drain any stale FIFO entries from before the disable. */
        pio_sm_clear_fifos(pio0, EDEV_PROBE_SM);
        /* Restart at fetch_cmd — known clean state. */
        pio_sm_restart       (pio0, EDEV_PROBE_SM);
        pio_sm_clkdiv_restart(pio0, EDEV_PROBE_SM);
        pio_sm_exec          (pio0, EDEV_PROBE_SM,
                              s.pio_offset + probe_offset_fetch_cmd);

        pio_sm_set_enabled(pio0, EDEV_PROBE_SM, true);
        s.pio_owns_pins = true;
    }
}

void probe_write_bits(uint32_t bit_count, uint32_t data)
{
    if (bit_count == 0u) {
        return;
    }
    ensure_pio_pin_ownership();
    pio_sm_put_blocking(pio0, EDEV_PROBE_SM, make_cmd(bit_count, true, CMD_WRITE));
    pio_sm_put_blocking(pio0, EDEV_PROBE_SM, data);
    keepalive_takeover();
}

uint32_t probe_read_bits(uint32_t bit_count)
{
    if (bit_count == 0u) {
        return 0;
    }
    ensure_pio_pin_ownership();
    pio_sm_put_blocking(pio0, EDEV_PROBE_SM, make_cmd(bit_count, false, CMD_READ));
    uint32_t v = pio_sm_get_blocking(pio0, EDEV_PROBE_SM);
    /* PIO reads shift right then push. For a < 32 bit read, the bits
     * land in the upper part of the word — right-align them. */
    if (bit_count < 32u) {
        v >>= (32u - bit_count);
    }
    keepalive_takeover();
    return v;
}

void probe_hiz_clocks(uint32_t bit_count)
{
    if (bit_count == 0u) {
        return;
    }
    ensure_pio_pin_ownership();
    pio_sm_put_blocking(pio0, EDEV_PROBE_SM, make_cmd(bit_count, false, CMD_TURNAROUND));
    pio_sm_put_blocking(pio0, EDEV_PROBE_SM, 0);
    keepalive_takeover();
}

/* ------------------------------------------------------------------ */
/*  Direct-GPIO bit-bang for DAP_SWJ_Sequence                          */
/* ------------------------------------------------------------------ */

static void release_pio_pins(void);   /* forward decl */

void probe_swj_bitbang(uint32_t bit_count, const uint8_t *data)
{
    if (bit_count == 0u) {
        return;
    }

    /* Release PIO ownership (disables the SM) and take both pins via
     * SIO. SWCLK starts LOW, SWDIO output. The next call into PIO will
     * re-init via ensure_pio_pin_ownership(). */
    release_pio_pins();
    gpio_set_function(EDEV_PROBE_PIN_SWCLK, GPIO_FUNC_SIO);
    gpio_set_function(EDEV_PROBE_PIN_SWDIO, GPIO_FUNC_SIO);
    gpio_set_dir(EDEV_PROBE_PIN_SWCLK, GPIO_OUT);
    gpio_set_dir(EDEV_PROBE_PIN_SWDIO, GPIO_OUT);
    gpio_put(EDEV_PROBE_PIN_SWCLK, 0);

    /* Half-period delay for SWCLK. SWJ_Sequence isn't latency-sensitive
     * (called only at protocol switch / line reset), so use a fixed
     * conservative 1 µs half-period = 500 kHz wire rate. This matches
     * PIO's known-working rate range and avoids the ~10 MHz that
     * naked gpio_put() loops produce on RP2350 @ 150 MHz, which is
     * faster than many target wires can reliably propagate. Hosts
     * that ask for slower clocks via DAP_SWJ_Clock get even slower. */
    uint32_t half_us = 1u;
    if (s.current_freq_khz > 0u && s.current_freq_khz < 500u) {
        half_us = 500u / s.current_freq_khz;   /* 1/(2f) in µs */
        if (half_us == 0u) half_us = 1u;
    }

    uint32_t byte_val = 0;
    uint32_t bits_in_byte = 0;
    for (uint32_t i = 0; i < bit_count; i++) {
        if (bits_in_byte == 0u) {
            byte_val = *data++;
            bits_in_byte = 8u;
        }
        /* Set SWDIO before SWCLK rising edge — target samples on rise. */
        gpio_put(EDEV_PROBE_PIN_SWDIO, (byte_val & 1u) != 0u);
        byte_val >>= 1;
        bits_in_byte--;

        busy_wait_us(half_us);
        gpio_put(EDEV_PROBE_PIN_SWCLK, 1);
        busy_wait_us(half_us);
        gpio_put(EDEV_PROBE_PIN_SWCLK, 0);
    }

    /* Leave SWCLK LOW, SWDIO OUTPUT at last value. Subsequent PIO use
     * will reclaim ownership through ensure_pio_pin_ownership(). */
}

/* ------------------------------------------------------------------ */
/*  direct GPIO drive (DAP_SWJ_Pins)                                   */
/* ------------------------------------------------------------------ */

static void take_pin_for_sio(uint pin, bool drive_out, bool value)
{
    gpio_set_function(pin, GPIO_FUNC_SIO);
    gpio_set_dir(pin, drive_out);
    if (drive_out) {
        gpio_put(pin, value);
    }
}

static void release_pio_pins(void)
{
    /* Also disable keep-alive — the caller is about to take direct
     * SIO control of SWCLK (DAP_SWJ_Pins / direct bit-bang); two
     * drivers on one pin is electrical contention. */
    keepalive_relinquish();
    if (!s.pio_owns_pins) {
        return;
    }
    pio_sm_set_enabled(pio0, EDEV_PROBE_SM, false);
    s.pio_owns_pins = false;
}

void probe_drive_swclk(bool high)
{
    release_pio_pins();
    take_pin_for_sio(EDEV_PROBE_PIN_SWCLK, true, high);
}

void probe_drive_swdio(bool high)
{
    release_pio_pins();
    take_pin_for_sio(EDEV_PROBE_PIN_SWDIO, true, high);
}

void probe_drive_nreset(bool high)
{
    /* nRESET is open-drain. "high" means tristate (target's pull-up
     * brings it up); "low" means drive a hard ground. */
    if (high) {
        gpio_set_dir(EDEV_PROBE_PIN_RESET, GPIO_IN);
    } else {
        gpio_set_dir(EDEV_PROBE_PIN_RESET, GPIO_OUT);
        gpio_put(EDEV_PROBE_PIN_RESET, 0);
    }
}

bool probe_read_swclk (void) { return gpio_get(EDEV_PROBE_PIN_SWCLK); }
bool probe_read_swdio (void) { return gpio_get(EDEV_PROBE_PIN_SWDIO); }
bool probe_read_nreset(void) { return gpio_get(EDEV_PROBE_PIN_RESET); }
