/*
 * swo.c — SWO UART RX, polled drain from main loop, 4 KB ring buffer.
 *
 * The PIO program lives in src/pio/swo_uart.pio (assembled by pioasm
 * into swo_uart.pio.h via pico_generate_pio_header in CMakeLists.txt).
 *
 * The ring buffer is power-of-two sized so we can use a bitmask instead
 * of modulo. wptr/rptr are uint32_t monotonically increasing — SPSC
 * discipline (PIO drain in main loop is producer; either USB IN or
 * DAP_SWO_Data is consumer). The two indices live in the same thread
 * for now, but the volatile keeps the door open for IRQ-driven drain.
 */

#include "swo.h"
#include "probe_pins.h"
#include "usb/usb_dap_class.h"

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"

#include "swo_uart.pio.h"

#define SWO_PIO              pio0
#define SWO_SM               1u           /* SM0 is the SWD probe; SM1 is ours */

#define SWO_RING_SIZE        4096u        /* must be power of two */
#define SWO_RING_MASK        (SWO_RING_SIZE - 1u)

#define SWO_TX_CHUNK_BYTES   64u          /* one bulk EP packet at a time */

static uint8_t  s_ring[SWO_RING_SIZE];
static volatile uint32_t s_wptr;
static volatile uint32_t s_rptr;

static struct {
    bool     initialised;
    bool     active;
    bool     overrun;
    uint32_t pio_offset;
    uint32_t current_baud;
} s = { 0 };

static inline uint32_t ring_used(void) { return s_wptr - s_rptr; }
static inline uint32_t ring_free(void) { return SWO_RING_SIZE - ring_used(); }

/* ------------------------------------------------------------------ */

void swo_init(void)
{
    if (s.initialised) {
        return;
    }
    s.pio_offset = pio_add_program(SWO_PIO, &swo_uart_rx_program);
    s.initialised = true;
    s.active      = false;
    s.overrun     = false;
    s_wptr = 0;
    s_rptr = 0;
}

uint32_t swo_set_baudrate(uint32_t requested_baud)
{
    if (!s.initialised) {
        return 0;
    }
    if (requested_baud < 9600u) {
        return 0;                  /* below this we lose timing accuracy */
    }
    /* PIO clock = 8 × baud. Highest baud is bounded by clk_sys / 8 —
     * at 150 MHz that's 18.75 Mbaud; we cap at 5 Mbaud for safety. */
    if (requested_baud > 5000000u) {
        return 0;
    }

    /* Stop the SM before changing config (changing clkdiv on a running
     * SM produces undefined behaviour during the transition). */
    bool was_active = s.active;
    if (s.active) {
        pio_sm_set_enabled(SWO_PIO, SWO_SM, false);
    }

    edev_swo_pio_program_init(SWO_PIO, SWO_SM, s.pio_offset, requested_baud);

    /* Compute the actual achievable baud given the integer+frac
     * divider quantisation, so we report it back to the host. */
    uint32_t clk_sys_hz = clock_get_hz(clk_sys);
    /* clkdiv = clk_sys / (8 × baud). With the 16+8 fractional
     * divider, the actual achievable rate is clk_sys / (8 × round(div))
     * — for our purposes the difference is < 1% at typical bauds. */
    float    div = (float)clk_sys_hz / (float)(8u * requested_baud);
    if (div < 1.0f) {
        div = 1.0f;
    }
    uint32_t actual = (uint32_t)((float)clk_sys_hz / (8.0f * div));
    s.current_baud = actual;

    if (was_active) {
        /* Flush any stale FIFO + ring before resuming. */
        while (!pio_sm_is_rx_fifo_empty(SWO_PIO, SWO_SM)) {
            (void)pio_sm_get(SWO_PIO, SWO_SM);
        }
        s_wptr = s_rptr;
        s.overrun = false;
        pio_sm_set_enabled(SWO_PIO, SWO_SM, true);
    }

    return actual;
}

void swo_start(void)
{
    if (!s.initialised || s.current_baud == 0u) {
        return;
    }
    /* Spec says DAP_SWO_Control(start) flushes the buffer first. */
    while (!pio_sm_is_rx_fifo_empty(SWO_PIO, SWO_SM)) {
        (void)pio_sm_get(SWO_PIO, SWO_SM);
    }
    s_wptr = s_rptr;
    s.overrun = false;

    pio_sm_restart       (SWO_PIO, SWO_SM);
    pio_sm_clkdiv_restart(SWO_PIO, SWO_SM);
    pio_sm_exec          (SWO_PIO, SWO_SM,
                          s.pio_offset + swo_uart_rx_offset_start);
    pio_sm_set_enabled   (SWO_PIO, SWO_SM, true);
    s.active = true;
}

void swo_stop(void)
{
    if (!s.initialised) {
        return;
    }
    pio_sm_set_enabled(SWO_PIO, SWO_SM, false);
    s.active = false;
}

bool     swo_is_active      (void) { return s.active;  }
bool     swo_has_overrun    (void) { return s.overrun; }
uint32_t swo_bytes_available(void) { return ring_used(); }

uint32_t swo_read(uint8_t *out, uint32_t max)
{
    uint32_t avail = ring_used();
    uint32_t n = (max < avail) ? max : avail;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = s_ring[(s_rptr + i) & SWO_RING_MASK];
    }
    s_rptr += n;
    return n;
}

/* ------------------------------------------------------------------ */
/*  Main-loop drain                                                    */
/* ------------------------------------------------------------------ */

static void drain_pio_fifo(void)
{
    /* PIO byte sits in the high 8 bits of each RX FIFO entry — see
     * swo_uart.pio's c-sdk block for why. */
    while (!pio_sm_is_rx_fifo_empty(SWO_PIO, SWO_SM)) {
        uint32_t v = pio_sm_get(SWO_PIO, SWO_SM);
        uint8_t  b = (uint8_t)(v >> 24);

        if (ring_free() == 0u) {
            s.overrun = true;
            /* Drop the byte — host learns about the overrun on the
             * next DAP_SWO_Status. */
            continue;
        }
        s_ring[s_wptr & SWO_RING_MASK] = b;
        s_wptr++;
    }
}

void swo_task(void)
{
    if (!s.active) {
        return;
    }

    drain_pio_fifo();

    /* If the host opted into streamed bulk SWO (DAP_SWO_Transport=2),
     * push chunks via the bulk endpoint. We only push if the ring
     * has at least one full chunk OR if the ring has been sitting on
     * data for a while — but tracking "a while" requires a timer; for
     * v0.1 we just push whenever the EP is idle and the ring is
     * non-empty.
     *
     * If the host is using the polled path (DAP_SWO_Data), don't
     * push anything here — the ring gets drained via swo_read() when
     * the host asks. The two paths cohabit because they read the same
     * ring; if the host polls, the EP stays idle and this never
     * fires; if the host streams, polling reads see the leftovers. */
    if (edev_dap_class_swo_is_busy()) {
        return;
    }

    static uint8_t s_tx_scratch[SWO_TX_CHUNK_BYTES];
    uint32_t avail = ring_used();
    if (avail == 0u) {
        return;
    }
    uint32_t chunk = (avail < SWO_TX_CHUNK_BYTES) ? avail : SWO_TX_CHUNK_BYTES;
    for (uint32_t i = 0; i < chunk; i++) {
        s_tx_scratch[i] = s_ring[(s_rptr + i) & SWO_RING_MASK];
    }
    if (edev_dap_class_swo_submit(s_tx_scratch, (uint16_t)chunk)) {
        s_rptr += chunk;
    }
}
