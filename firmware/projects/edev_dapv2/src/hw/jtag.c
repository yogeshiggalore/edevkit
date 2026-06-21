/*
 * jtag.c — JTAG bit-bang via SIO. See jtag.h for the rationale.
 *
 * Speed: at -O2 the inner loop is ~6 instructions per half-bit, so
 * each full bit is ~12 instructions = ~80 ns at 150 MHz clk_sys
 * → upper bound ~12 MHz on a pure GPIO loop. Real-world targets
 * need cable settling so we cap at 5 MHz and back off via a busy-wait
 * loop for lower requested rates.
 */

#include "jtag.h"
#include "probe.h"
#include "probe_pins.h"

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"

static struct {
    bool     attached;
    uint32_t freq_khz;
    uint32_t half_period_cycles;
} s = { .attached = false, .freq_khz = 1000u };

/* Crude busy-wait of `n` Cortex-M33 clock cycles. Used to slow the
 * TCK down for lower-frequency requests. */
static inline void cycle_delay(uint32_t n)
{
    while (n--) {
        __asm__ volatile("nop");
    }
}

void jtag_init(void)
{
    /* Pin pads — set up TDI as output (with pull-up off), TDO as
     * input (with pull-up so an unconnected TDO doesn't float). TCK
     * and TMS are shared with SWCLK/SWDIO so probe_init() has
     * already configured them. */
    gpio_init(EDEV_PROBE_PIN_TDI);
    gpio_set_dir(EDEV_PROBE_PIN_TDI, GPIO_OUT);
    gpio_put(EDEV_PROBE_PIN_TDI, 0);

    gpio_init(EDEV_PROBE_PIN_TDO);
    gpio_set_dir(EDEV_PROBE_PIN_TDO, GPIO_IN);
    gpio_pull_up(EDEV_PROBE_PIN_TDO);
}

void jtag_attach(void)
{
    if (s.attached) {
        return;
    }
    /* Steal TCK + TMS from PIO and become their owner. probe.c's
     * release_pio_pins() / ensure_pio_pin_ownership() pattern handles
     * the reverse direction. */
    probe_drive_swclk(false);   /* TCK = SWCLK pin, idle low */
    probe_drive_swdio(false);   /* TMS = SWDIO pin           */

    /* TDI was already configured by jtag_init; force a known state. */
    gpio_put(EDEV_PROBE_PIN_TDI, 0);
    s.attached = true;
}

void jtag_detach(void)
{
    s.attached = false;
    /* The next probe_*() call will re-attach PIO ownership of
     * SWCLK/SWDIO automatically via ensure_pio_pin_ownership(). */
}

void jtag_set_tck_freq_khz(uint32_t freq_khz)
{
    if (freq_khz == 0u) {
        freq_khz = 1u;
    }
    s.freq_khz = freq_khz;

    /* The natural unloaded loop runs at ~10 MHz. To go slower, we add
     * a NOP-cycle busy-wait per half-period. Formula:
     *
     *    target half period (cycles) = clk_sys_hz / (2 × target_hz)
     *    natural overhead per half period ≈ 8 cycles
     *
     * So cycles_to_delay = max(0, target - 8). */
    uint32_t clk_sys_hz = clock_get_hz(clk_sys);
    uint32_t target = clk_sys_hz / (2u * freq_khz * 1000u);
    s.half_period_cycles = (target > 8u) ? (target - 8u) : 0u;
}

/* The core bit transfer. Inlined for the loop in jtag_shift. */
static inline bool jtag_clock_one(bool tdi_bit, bool tms_high, bool capture)
{
    gpio_put(EDEV_PROBE_PIN_SWCLK, 0);     /* TCK low */
    gpio_put(EDEV_PROBE_PIN_SWDIO, tms_high);
    gpio_put(EDEV_PROBE_PIN_TDI,   tdi_bit);
    cycle_delay(s.half_period_cycles);
    gpio_put(EDEV_PROBE_PIN_SWCLK, 1);     /* TCK high — target samples TDI here */
    bool tdo = capture ? gpio_get(EDEV_PROBE_PIN_TDO) : false;
    cycle_delay(s.half_period_cycles);
    return tdo;
}

uint64_t jtag_shift(uint8_t bit_count, uint64_t tdi, bool tms_high, bool capture)
{
    if (!s.attached) {
        return 0;
    }
    if (bit_count == 0u) {
        return 0;
    }

    uint64_t tdo = 0;
    for (uint8_t i = 0; i < bit_count; i++) {
        bool tdi_bit = (tdi & 1ull) != 0;
        tdi >>= 1;
        bool tdo_bit = jtag_clock_one(tdi_bit, tms_high, capture);
        if (capture && tdo_bit) {
            tdo |= (1ull << i);
        }
    }
    /* Leave TCK low after the sequence — TAPs sample TDI on rising
     * edge and update outputs on falling, so a low TCK is the
     * universal "quiescent" state. */
    gpio_put(EDEV_PROBE_PIN_SWCLK, 0);
    return tdo;
}
