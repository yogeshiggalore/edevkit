/*
 * safety.c — auto-BOOTSEL recovery on firmware fault or hang.
 *
 * Three recovery paths so a misbehaving build never strands the user
 * with "unplug USB + hold BOOTSEL + replug":
 *
 *   1. HardFault / MemManage / BusFault / UsageFault → reset_usb_boot()
 *   2. Watchdog timeout (main loop stuck >10s) → reboot
 *      On reboot, if previous reset was watchdog → reset_usb_boot()
 *   3. Normal 1200-baud CDC trick (already in util/log.c)
 *
 * After any of (1) or (2), `/Volumes/RP2350` re-mounts and picotool
 * can flash a new build without physical button press.
 */

#include "util/safety.h"

#include "pico/bootrom.h"
#include "pico/stdlib.h"
#include "hardware/watchdog.h"

#define WATCHDOG_TIMEOUT_MS  10000u

void safety_init_check(void)
{
    /* NO-OP for now. The watchdog_caused_reboot() check was triggering
     * spurious BOOTSEL entries — picotool's "reboot into application"
     * apparently sets the same flag the watchdog uses. Stick with the
     * HardFault handler as the only auto-BOOTSEL path. */
}

void safety_watchdog_start(void)
{
    /* Watchdog disabled for now — same reason as above. The HardFault
     * handler is sufficient auto-recovery for the bugs we've seen so
     * far (the device crashes hard, not hangs). */
}

void safety_watchdog_feed(void)
{
    /* No-op. */
}

/* Cortex-M fault handlers — weak symbols in the SDK that we override.
 * Calling reset_usb_boot from a fault is safe; it doesn't depend on
 * heap, stack-pointer integrity is not critical (bootrom uses its own). */
void __attribute__((noreturn)) isr_hardfault(void)
{
    reset_usb_boot(0, 0);
    while (1) { }
}

void __attribute__((noreturn)) isr_memmanage(void)
{
    reset_usb_boot(0, 0);
    while (1) { }
}

void __attribute__((noreturn)) isr_busfault(void)
{
    reset_usb_boot(0, 0);
    while (1) { }
}

void __attribute__((noreturn)) isr_usagefault(void)
{
    reset_usb_boot(0, 0);
    while (1) { }
}
