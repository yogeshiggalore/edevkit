/*
 * led.c — sign-of-life heartbeat LED.
 */

#include "led.h"

#include "pico/stdlib.h"
#include "pico/time.h"

#ifndef PICO_DEFAULT_LED_PIN
#error "PICO_DEFAULT_LED_PIN must be defined by the board header (it is for pico2)"
#endif

#define LED_PIN PICO_DEFAULT_LED_PIN

/* 1 Hz blink → 500 ms on, 500 ms off. */
#define LED_HEARTBEAT_PERIOD_MS 500u

static absolute_time_t s_next_toggle;
static bool s_led_state;

void led_init(void)
{
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);
    s_led_state = false;
    s_next_toggle = make_timeout_time_ms(LED_HEARTBEAT_PERIOD_MS);
}

void led_set(bool on)
{
    s_led_state = on;
    gpio_put(LED_PIN, on);
}

void led_toggle(void)
{
    s_led_state = !s_led_state;
    gpio_put(LED_PIN, s_led_state);
}

void led_task(void)
{
    if (absolute_time_diff_us(get_absolute_time(), s_next_toggle) > 0) {
        return;
    }
    led_toggle();
    s_next_toggle = make_timeout_time_ms(LED_HEARTBEAT_PERIOD_MS);
}
