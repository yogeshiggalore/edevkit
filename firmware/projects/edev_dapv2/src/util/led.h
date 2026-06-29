/*
 * led.h — sign-of-life LED.
 *
 * On a stock Pico 2, the LED is GPIO 25 (`PICO_DEFAULT_LED_PIN`).
 *
 * On a Pico 2 W (default board for v0.1), the on-board LED is wired
 * to the CYW43439 wireless chip's GPIO 0, not an RP2350 GPIO. Driving
 * it requires bringing up `cyw43_arch`, which pulls in the entire
 * WiFi stack — out of proportion for a heartbeat blink. The LED API
 * therefore becomes a no-op on pico2_w. Wire an external LED to GP22
 * if you want a visual heartbeat (see README).
 */

#ifndef EDEV_DAPV2_UTIL_LED_H
#define EDEV_DAPV2_UTIL_LED_H

#include <stdbool.h>

void led_init(void);
void led_set(bool on);
void led_toggle(void);

/* Call every iteration of the main loop. Drives a 1 Hz heartbeat
 * (500 ms on / 500 ms off). No-op when no LED GPIO is available. */
void led_task(void);

#endif /* EDEV_DAPV2_UTIL_LED_H */
