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

/* Keep-alive cadence. We send ONE narrow SWCLK pulse per period (below the
 * 50-cycle ADIv5 line-reset threshold, so the chip's DP state machine is
 * unaffected) just often enough to keep the debug clock domain awake.
 *
 * 5 kHz tick → 200 µs between pulses; well under any reasonable debug-clock
 * gate timeout, well under the per-transfer wall-time budget (a 200 kHz SWD
 * transfer is ~250 µs, so during a packet of 8 back-to-back transfers our
 * timer is held off by irq_lock for ~2 ms — we miss ~10 ticks, the timer
 * fires once on resume, no harm done).
 */
#define KEEPALIVE_PERIOD_US 200

static struct k_timer keepalive_timer;
static atomic_t keepalive_active;

/* Expiry runs from the system-clock ISR. Cortex-M PRIMASK (set by
 * irq_lock() in swdp_bitbang during a transfer) blocks all maskable
 * interrupts including SysTick, so we cannot pre-empt a transfer in
 * progress. Between transfers we get a fresh tick window.
 *
 * We deliberately pulse rather than toggle: a toggle leaves SWCLK in an
 * unknown state when the bit-bang takes over (sw_port_on doesn't drive
 * SWCLK level until the first SW_CLOCK_CYCLE). A pulse leaves SWCLK low
 * at the end of every tick, which matches the bit-bang's idle level.
 */
static void keepalive_expiry(struct k_timer *t)
{
	ARG_UNUSED(t);

	if (!atomic_get(&keepalive_active)) {
		return;
	}

	/* Two MMIO stores: SIO.SET then SIO.CLR. ~3 CPU cycles each →
	 * ~40 ns SWCLK high time at 150 MHz. Well above min SWCLK pulse
	 * width on every target SoC we care about (typ. >5 ns). */
	gpio_pin_set_dt(&swclk, 1);
	gpio_pin_set_dt(&swclk, 0);
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

	/* Pin direction is owned by swdp_bitbang's sw_port_on(): SWCLK is
	 * GPIO_DISCONNECTED until DAP_Connect, then GPIO_OUTPUT_INACTIVE.
	 * Our pulse on a DISCONNECTED pin is a no-op (gpio_pin_set_dt
	 * silently returns without driving), so it's safe to arm the
	 * timer unconditionally. When sw_port_on() drives SWCLK output,
	 * the pulses start landing on the wire automatically.
	 *
	 * keepalive_active starts at 0 — the resume hook below flips it on
	 * when the swdp_api consumer signals that a debug session is open.
	 */
	k_timer_start(&keepalive_timer,
		      K_USEC(KEEPALIVE_PERIOD_US),
		      K_USEC(KEEPALIVE_PERIOD_US));

	LOG_INF("SWCLK keep-alive timer armed (%u µs period, gated off)",
		KEEPALIVE_PERIOD_US);
	return 0;
}

/* Public hooks called by the swdp_bitbang sw_port_on/sw_port_off integration.
 * pause() is also called by sw_transfer's caller around batched DAP_Transfer
 * commands; the irq_lock inside sw_transfer takes care of masking the timer
 * during the actual bit-bang, so pause() here is conceptually for the gaps
 * BETWEEN sessions, not BETWEEN transfers. */

void edev_swclk_keepalive_pause(void)
{
	atomic_set(&keepalive_active, 0);
}

void edev_swclk_keepalive_resume(void)
{
	atomic_set(&keepalive_active, 1);
}

SYS_INIT(swclk_keepalive_init, APPLICATION, 90);
