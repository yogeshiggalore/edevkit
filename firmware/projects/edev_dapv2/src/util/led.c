/*
 * led.c — sign-of-life heartbeat LED.
 *
 * On Pico 2 W (default), the only on-board user LED is on the
 * CYW43439 wireless chip's GPIO 0. We use cyw43_arch_gpio_put — which
 * requires cyw43_arch_init() at boot — but we link the "none" variant
 * of pico_cyw43_arch so no WiFi/lwIP stack is pulled in. The cost is
 * ~30–50 KB of cyw43 driver to read the firmware blob into the chip.
 *
 * On stock Pico 2 (or any board that defines PICO_DEFAULT_LED_PIN
 * directly on an RP2350 GPIO), we just drive that GPIO.
 */

#include "util/led.h"

#include "pico/stdlib.h"
#include "pico/time.h"

#if defined(CYW43_WL_GPIO_LED_PIN)
#include "pico/cyw43_arch.h"
#endif

#define LED_HEARTBEAT_PERIOD_MS 500u

static absolute_time_t s_next_toggle;
static bool s_led_state;

static inline void led_drive(bool on)
{
#if defined(PICO_DEFAULT_LED_PIN)
    gpio_put(PICO_DEFAULT_LED_PIN, on);
#elif defined(CYW43_WL_GPIO_LED_PIN)
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
#else
    (void) on;
#endif
}

void led_init(void)
{
#if defined(PICO_DEFAULT_LED_PIN)
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#endif
    /* For the cyw43 path, the caller must have already called
     * cyw43_arch_init() in main(). The cyw43 GPIO is ready as soon as
     * that returns. */
    s_led_state = false;
    led_drive(false);
    s_next_toggle = make_timeout_time_ms(LED_HEARTBEAT_PERIOD_MS);
}

void led_set(bool on)
{
    s_led_state = on;
    led_drive(on);
}

void led_toggle(void)
{
    s_led_state = !s_led_state;
    led_drive(s_led_state);
}

void led_task(void)
{
    if (absolute_time_diff_us(get_absolute_time(), s_next_toggle) > 0) {
        return;
    }
    led_toggle();
    s_next_toggle = make_timeout_time_ms(LED_HEARTBEAT_PERIOD_MS);
}
