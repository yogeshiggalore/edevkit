/*
 * led.h — sign-of-life LED.
 *
 * On a stock Pico 2 the LED is on GPIO 25. Boards that use a WS2812 RGB
 * LED (XIAO RP2350) need a different path — handle in v0.2.
 */

#ifndef EDEV_DAPV2_LED_H
#define EDEV_DAPV2_LED_H

#include <stdbool.h>

/* Initialise the LED pin. Call once at boot. */
void led_init(void);

/* Drive the LED directly. Tied to a host-controlled state via
 * DAP_HostStatus in later milestones; for now main.c calls these. */
void led_set(bool on);
void led_toggle(void);

/* Should be called every iteration of the main loop. Drives a 1 Hz
 * heartbeat blink so we can see the firmware is alive without needing
 * any USB or serial console. */
void led_task(void);

#endif /* EDEV_DAPV2_LED_H */
