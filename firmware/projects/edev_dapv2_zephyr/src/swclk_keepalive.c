/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * SWCLK keep-alive for edev_dapv2 (Zephyr port).
 *
 * Some targets (notably nRF5340 in a deep-wedge state) gate their debug
 * clock domain after a brief idle period on SWCLK. J-Link works around
 * this by continuously toggling SWCLK between debug transactions — it's
 * the "SWCLK keep-alive" or "continuous SWCLK" feature. Without it our
 * probe sees indefinite WAIT-ACK responses on AHB-AP reads, the chip's
 * NVMC ERASEALL never completes, etc.
 *
 * Our pico-sdk firmware had this (PIO0 SM1 driving SWCLK at ~5 kHz, see
 * memory project_swclk_keepalive_pio_2026_06_19) until the v0.2 PIO
 * rebuild dropped it. This Zephyr-side reimplementation uses a CPU-side
 * k_timer firing from the system clock ISR.
 *
 * Why this works without explicit synchronisation with swdp_bitbang:
 *
 *   drivers/dp/swdp_bitbang.c::sw_transfer (and sw_output_sequence and
 *   sw_input_sequence) wrap their inner bit-bang loop in irq_lock(),
 *   which sets PRIMASK=1 on Cortex-M and masks ALL maskable interrupts
 *   including SysTick. Our k_timer expiry runs from SysTick → it's
 *   blocked during the transfer and only fires between transfers, which
 *   is exactly when we want SWCLK kept alive. No race, no extra lock.
 *
 * Tuning: nRF5340's debug clock gate timeout is on the order of
 * milliseconds; 5 kHz (= 200 µs toggle period, 100 µs each level) gives
 * us comfortable margin without burning meaningful CPU.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(swclk_keepalive, LOG_LEVEL_INF);

/* SWCLK pin lives on the dp0 node's clk-gpios property (board overlay
 * keeps these identical to probe_pins.h: GP2 = SWCLK). Resolve at compile
 * time so we don't pay a DT-lookup cost on every timer tick. */
#define DP0_NODE DT_NODELABEL(dp0)
#if !DT_NODE_HAS_PROP(DP0_NODE, clk_gpios)
#error "dp0 node missing clk-gpios — keep-alive can't resolve SWCLK"
#endif

static const struct gpio_dt_spec swclk =
	GPIO_DT_SPEC_GET(DP0_NODE, clk_gpios);

#define KEEPALIVE_HALF_PERIOD_US 100	/* → 5 kHz square wave on SWCLK */

static struct k_timer keepalive_timer;

/* Expiry runs from the system clock ISR — fast, no allocations, no
 * Zephyr API that could sleep. gpio_pin_toggle_dt is a thin wrapper
 * around the hal_rpi_pico SIO GPIO_OUT_XOR register write — single
 * MMIO store, atomic at the hardware level. */
static void keepalive_expiry(struct k_timer *t)
{
	ARG_UNUSED(t);
	(void)gpio_pin_toggle_dt(&swclk);
}

static int swclk_keepalive_init(void)
{
	if (!gpio_is_ready_dt(&swclk)) {
		LOG_ERR("SWCLK GPIO not ready — keep-alive disabled");
		return -ENODEV;
	}

	/* Don't reconfigure direction here — swdp_bitbang's sw_port_on()
	 * owns the SWCLK direction (output when a debug session is active,
	 * disconnected when not). Our timer just toggles via the XOR
	 * register, which is a no-op on an input pin. So keep-alive ticks
	 * are harmless when no session is active and effective when one is.
	 */

	k_timer_init(&keepalive_timer, keepalive_expiry, NULL);

	/* IMPORTANT: timer NOT started here. The SWCLK keep-alive is only
	 * useful between SWD transactions during an active debug session.
	 * Starting it unconditionally toggles SWCLK while swdp_bitbang's
	 * sw_port_off() has the pin as GPIO_DISCONNECTED — harmless, but
	 * also pointless. Worse, in a session it can race with SWD's wire
	 * timing in ways that confuse probe-rs/pyocd's state machine.
	 *
	 * The clean integration point is sw_port_on()/sw_port_off() inside
	 * Zephyr's drivers/dp/swdp_bitbang.c — pause keep-alive at the start
	 * of a transfer, resume between. That requires patching upstream
	 * (out-of-tree), or wrapping swdp_bitbang with a custom swdp_api
	 * driver that adds the hooks.
	 *
	 * Hooks API (TODO):
	 *    void edev_swclk_keepalive_pause(void);   // call before SWD work
	 *    void edev_swclk_keepalive_resume(void);  // call after
	 *
	 * Until those hooks are wired in, this file just defines the timer
	 * so next session can flip the switch with the right integration in
	 * place. */
	(void)keepalive_timer;

	LOG_INF("SWCLK keep-alive scaffolded (NOT armed — needs swdp hooks)");
	return 0;
}

/* Public hooks for swdp wrapping. Will be called by a future
 * swdp_keepalive_wrapper driver that delegates to swdp_bitbang. */

void edev_swclk_keepalive_pause(void)
{
	k_timer_stop(&keepalive_timer);
}

void edev_swclk_keepalive_resume(void)
{
	k_timer_start(&keepalive_timer,
		      K_USEC(KEEPALIVE_HALF_PERIOD_US),
		      K_USEC(KEEPALIVE_HALF_PERIOD_US));
}

SYS_INIT(swclk_keepalive_init, APPLICATION, 90);
