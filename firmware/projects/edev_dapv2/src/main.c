/*
 * main.c — edev_dapv2 entry point and cooperative event loop.
 *
 * The loop is single-threaded by design. CMSIS-DAP is a strict
 * request/response protocol — TinyUSB delivers a packet, we dispatch
 * and fill a response, TinyUSB sends it back. Nothing here justifies
 * an RTOS.
 *
 * Each milestone wires its own task() into the loop:
 *   M1: led_task                             (sign of life)
 *   M2: + tud_task + edev_dap_class_task       (USB + DAP packet ring)
 *   M5+: + probe_task                        (PIO completion + nRESET timing)
 *   M7+: + swo_task                          (DMA ring → bulk EP 0x82)
 *
 * Each task is non-blocking. If you find yourself wanting to call
 * sleep_ms() inside a task, you're holding it wrong — schedule via
 * absolute_time_t deadlines instead, the way led_task() does.
 */

#include "pico/stdlib.h"
#include "tusb.h"

#include "hw/chip_id.h"
#include "hw/probe.h"
#include "hw/swo.h"
#include "util/led.h"
#include "dap/dap.h"
#include "usb/usb_dap_class.h"

int main(void)
{
    /* stdio_init_all configures UART0 (per pico_enable_stdio_uart in
     * CMakeLists) so printf goes to GP0/GP1 — useful for development. */
    stdio_init_all();

    /* Bring up the serial number string ASAP so anything that wants it
     * later (USB iSerialNumber descriptor, log prefixes) finds it ready. */
    chip_id_init();

    led_init();
    probe_init();
    swo_init();
    dap_init();

    /* TinyUSB device stack — descriptors + standard requests + EP0. */
    tusb_init();

    /* Per-iteration: each task runs once and returns quickly. The
     * tight_loop_contents() hint tells the optimiser this isn't a
     * spin we'd want vectorised. */
    while (true) {
        tud_task();              /* TinyUSB core: ISR drain, control xfers */
        edev_dap_class_task();     /* DAP packet drain → dispatch → response */
        swo_task();              /* PIO RX FIFO → ring → bulk EP 0x82     */
        led_task();              /* heartbeat blink                       */
        tight_loop_contents();
    }
}
