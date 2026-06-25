/*
 * safety.h — auto-BOOTSEL recovery.
 */

#ifndef EDEV_DAPV2_UTIL_SAFETY_H
#define EDEV_DAPV2_UTIL_SAFETY_H

/* Call at the very start of main() — before any other init.
 * If previous reset was watchdog-triggered, jumps straight to
 * BOOTSEL instead of restarting the (presumed-buggy) firmware. */
void safety_init_check(void);

/* Call after main init has completed. Arms the watchdog with a
 * 10-second timeout. */
void safety_watchdog_start(void);

/* Call every iteration of the main loop. Resets the watchdog timer. */
void safety_watchdog_feed(void);

#endif /* EDEV_DAPV2_UTIL_SAFETY_H */
