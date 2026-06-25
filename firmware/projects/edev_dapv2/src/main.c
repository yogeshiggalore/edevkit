/*
 * main.c — edev_dapv2 entry point and cooperative event loop.
 *
 * CMSIS-DAP is a strict request/response protocol: TinyUSB delivers a
 * packet, we dispatch, we fill a response, TinyUSB sends it back. A
 * single cooperative loop is the right shape — nothing here justifies
 * an RTOS.
 *
 * Tasks added milestone-by-milestone:
 *
 *   M1  led_task                    sign-of-life
 *   M2  + tud_task                  TinyUSB core: ISR drain, EP0 xfers
 *   M2  + edev_dap_class_task       DAP packet ring → dispatch → response
 *   M7+ + swo_task                  SWO DMA ring → bulk EP 0x82
 *
 * Each task must be non-blocking. If you find yourself wanting to call
 * sleep_ms() inside a task, you're holding it wrong — schedule via an
 * absolute_time_t deadline (the way led_task() does).
 */

#include "pico/stdlib.h"
#include "tusb.h"

#if defined(CYW43_WL_GPIO_LED_PIN)
#include "pico/cyw43_arch.h"
#endif

#include "util/led.h"
#include "util/log.h"
#include "util/safety.h"
#include "hw/chip_id.h"
#include "hw/probe.h"
#include "dap/dap.h"
#include "usb/usb_dap_class.h"

int main(void)
{
    /* CHECK FIRST: if previous reset was a watchdog timeout, jump
     * straight to BOOTSEL so the user can re-flash without physical
     * intervention. Must run before anything else (including stdio). */
    safety_init_check();

    stdio_init_all();

    /* Generate serial number string before TinyUSB queries it. */
    chip_id_init();

#if defined(CYW43_WL_GPIO_LED_PIN)
    /* Bring up the CYW43 chip (no WiFi — we link pico_cyw43_arch_none).
     * Synchronous: returns when the chip is ready to drive its GPIOs.
     * Must precede led_init() because the LED hangs off CYW43 GPIO 0. */
    (void) cyw43_arch_init();
#endif

    led_init();
    probe_init();
    dap_init();
    edev_dap_class_init();
    log_init();
    /* Boot banner with __DATE__ and __TIME__ so every fresh build has a
     * unique signature. After flashing, read CDC and verify this line
     * matches the build's compile time — proves the NEW firmware is
     * actually running, not stale code from a previous flash. */
    log_puts("\n=== edev_dapv2 v" EDEV_DAPV2_VERSION_STR
             " build " __DATE__ " " __TIME__ " ===\n");

    tusb_init();

    /* Arm watchdog AFTER init so init delays don't fire it. */
    safety_watchdog_start();

    while (true) {
        safety_watchdog_feed();
        tud_task();
        edev_dap_class_task();
        log_task();
        led_task();
        tight_loop_contents();
    }
}
