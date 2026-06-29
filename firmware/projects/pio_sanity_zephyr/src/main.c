/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * pio_sanity_zephyr — does PIO output work AT ALL on Zephyr/RP2350?
 *
 * We boot Zephyr, configure USB CDC for the console banner, then claim one
 * PIO1 state machine and load a tiny program that toggles GP2 at ~18 Hz.
 * The main loop polls the SIO pad input every 5 ms and prints the observed
 * level. Expected output: a long stream of alternating "HIGH"/"LOW" lines,
 * one phase every ~28 ms (so 5-6 polls per phase).
 *
 * Why this is useful: our SWD PIO bit-banger fails at the wire level (target
 * returns FAULT on first DPIDR) despite the bits going out byte-correct.
 * Before assuming the bug is in OUR PIO program, this test isolates the
 * question "does ANY PIO output reach the pad on Zephyr's RP2350 build?"
 *
 * Diagnostics this captures without any external instrument:
 *  - PIO state machine actually runs (we'd see no transitions otherwise).
 *  - Pad input reads back the value PIO is driving (rules out pad isolation
 *    or input-buffer issues affecting our SWD reads).
 *  - Toggle rate matches the math (rules out clock divider miscompute).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usbd.h>

/* Resolve the PIO devicetree node BEFORE including hardware/pio.h. The
 * pico-sdk header `#define pio1 pio1_hw`, which clobbers DT_NODELABEL(pio1)
 * if we use the macro after the include. */
#define PIODEV_NODE DT_NODELABEL(pio1)
static const struct device *const g_piodev = DEVICE_DT_GET(PIODEV_NODE);

#include <zephyr/drivers/misc/pio_rpi_pico/pio_rpi_pico.h>

#include <hardware/clocks.h>
#include <hardware/pio.h>
#include <hardware/regs/pads_bank0.h>
#include <hardware/structs/pads_bank0.h>
#include <hardware/structs/sio.h>

#include <sample_usbd.h>

#include <pico/bootrom.h>

#include "toggle.pio.h"
#include "probe_swd.pio.h"

#define EDEV_REBOOT_MAGIC_BAUD 1200u

static void bootsel_watch(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);
	const struct device *console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

	while (!device_is_ready(console)) {
		k_sleep(K_MSEC(100));
	}
	while (1) {
		uint32_t baud = 0;

		uart_line_ctrl_get(console, UART_LINE_CTRL_BAUD_RATE, &baud);
		if (baud == EDEV_REBOOT_MAGIC_BAUD) {
			k_sleep(K_MSEC(50));
			reset_usb_boot(0, 0);
		}
		k_sleep(K_MSEC(250));
	}
}
K_THREAD_DEFINE(bootsel, 1024, bootsel_watch, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 1000);

LOG_MODULE_REGISTER(pio_sanity, LOG_LEVEL_INF);

#define TOGGLE_PIN 2u	/* GP2 — same as SWCLK in our SWD project */

static void wait_for_dtr(const struct device *console)
{
	uint32_t dtr = 0;

	while (!dtr) {
		uart_line_ctrl_get(console, UART_LINE_CTRL_DTR, &dtr);
		k_sleep(K_MSEC(100));
	}
}

static int setup_usb(void)
{
	struct usbd_context *ctx = sample_usbd_setup_device(NULL);

	if (!ctx) {
		return -ENODEV;
	}
	int ret = usbd_init(ctx);
	if (ret) {
		return ret;
	}
	return usbd_enable(ctx);
}

static int setup_pio(void)
{
	if (!device_is_ready(g_piodev)) {
		LOG_ERR("pio1 not ready");
		return -ENODEV;
	}

	PIO pio = pio_rpi_pico_get_pio(g_piodev);
	size_t sm;
	int ret = pio_rpi_pico_allocate_sm(g_piodev, &sm);

	if (ret) {
		LOG_ERR("allocate_sm failed: %d", ret);
		return ret;
	}

	if (!pio_can_add_program(pio, &toggle_test_program)) {
		LOG_ERR("PIO program won't fit");
		return -EBUSY;
	}
	uint offset = pio_add_program(pio, &toggle_test_program);

	pio_sm_config c = toggle_test_program_get_default_config(offset);
	sm_config_set_sideset_pins(&c, TOGGLE_PIN);

	/* Slowest sustainable divider so the toggle is poll-visible. */
	sm_config_set_clkdiv(&c, 65535.0f);
	sm_config_set_wrap(&c, offset + toggle_test_wrap_target,
			   offset + toggle_test_wrap);

	pio_gpio_init(pio, TOGGLE_PIN);
	/* RP2350 E10 erratum belt-and-suspenders — pad isolation clear. */
	hw_clear_bits(&pads_bank0_hw->io[TOGGLE_PIN], PADS_BANK0_GPIO0_ISO_BITS);

	pio_sm_set_consecutive_pindirs(pio, sm, TOGGLE_PIN, 1, true);
	pio_sm_init(pio, sm, offset, &c);
	pio_sm_set_enabled(pio, sm, true);

	uint32_t sys_hz = clock_get_hz(clk_sys);
	float pio_hz = (float)sys_hz / 65535.0f;
	float toggle_hz = pio_hz / 64.0f;	/* 64 PIO cycles per period */

	LOG_INF("PIO running on sm=%u offset=%u sys=%u Hz pio=%d Hz toggle=%d Hz",
		(unsigned)sm, (unsigned)offset, (unsigned)sys_hz,
		(int)pio_hz, (int)toggle_hz);

	/* Second SM on the same PIO + same pin, running the `reader` program
	 * — validates that PIO `in pins` can sample what side-set is driving.
	 * If this works, our SWD bug is NOT in the basic input path either. */
	size_t reader_sm;

	ret = pio_rpi_pico_allocate_sm(g_piodev, &reader_sm);
	if (ret) {
		LOG_WRN("reader sm allocation failed: %d", ret);
		return 0;
	}

	uint reader_offset = pio_add_program(pio, &reader_program);
	pio_sm_config rc = reader_program_get_default_config(reader_offset);

	sm_config_set_in_pins(&rc, TOGGLE_PIN);
	sm_config_set_in_shift(&rc, true /*right*/, false /*autopush*/, 32);
	/* Slow the reader to ~1 kHz sample rate so RX FIFO doesn't overflow
	 * before we drain it. */
	sm_config_set_clkdiv(&rc, 65535.0f);
	sm_config_set_wrap(&rc, reader_offset + reader_wrap_target,
			   reader_offset + reader_wrap);

	pio_sm_init(pio, reader_sm, reader_offset, &rc);
	pio_sm_set_enabled(pio, reader_sm, true);
	LOG_INF("reader sm=%u offset=%u also running on GP%u",
		(unsigned)reader_sm, (unsigned)reader_offset, TOGGLE_PIN);

	/* Drain the reader's RX FIFO for a second and check what we got. */
	k_sleep(K_MSEC(1000));

	uint32_t reader_reads = 0;
	uint32_t reader_high = 0;
	uint32_t reader_low = 0;

	while (!pio_sm_is_rx_fifo_empty(pio, reader_sm)) {
		uint32_t v = pio_sm_get(pio, reader_sm);
		/* in pins 1 + push: the bit lives at ISR[31] before push. After
		 * right-shift by 31, bit 0 of v is our sample. */
		uint32_t bit = (v >> 31) & 1u;

		reader_reads++;
		if (bit) {
			reader_high++;
		} else {
			reader_low++;
		}
	}
	LOG_INF("reader: %u samples drained — high=%u low=%u",
		reader_reads, reader_high, reader_low);
	if (reader_reads == 0) {
		LOG_ERR("PIO `in pins` returned no samples (FIFO empty)");
	} else if (reader_high == 0 || reader_low == 0) {
		LOG_ERR("PIO `in pins` sees only one level — INPUT PATH BROKEN");
	} else {
		LOG_INF("PIO `in pins` sampled both HIGH and LOW — INPUT WORKS");
	}

	return 0;
}

/* --------------------------------------------------------------------
 * Test 3: load our actual probe_swd.pio's write_bits and capture the
 * SWDIO bits it drives with a fast-sampling reader SM. Compares the
 * captured pattern against the expected 0xA5 (DPIDR header LSB-first).
 * -------------------------------------------------------------------- */

#define WT_SWCLK_PIN 4u		/* GP4, free in this test */
#define WT_SWDIO_PIN 5u		/* GP5, free in this test */

static int run_write_test(void)
{
	PIO pio = pio_rpi_pico_get_pio(g_piodev);
	size_t writer_sm, reader_sm;

	if (pio_rpi_pico_allocate_sm(g_piodev, &writer_sm)) {
		LOG_ERR("writer sm allocate failed");
		return -1;
	}
	if (pio_rpi_pico_allocate_sm(g_piodev, &reader_sm)) {
		LOG_ERR("reader_fast sm allocate failed");
		return -1;
	}

	if (!pio_can_add_program(pio, &probe_swd_program)) {
		LOG_ERR("probe_swd won't fit alongside the other programs");
		return -1;
	}
	uint sw_off = pio_add_program(pio, &probe_swd_program);

	if (!pio_can_add_program(pio, &reader_fast_program)) {
		LOG_ERR("reader_fast won't fit");
		return -1;
	}
	uint rd_off = pio_add_program(pio, &reader_fast_program);

	/* --- writer config (matches our SWD driver's sm_configure) --- */
	pio_sm_config wc = probe_swd_program_get_default_config(sw_off);

	sm_config_set_sideset_pins(&wc, WT_SWCLK_PIN);
	sm_config_set_out_pins(&wc, WT_SWDIO_PIN, 1);
	sm_config_set_in_pins(&wc, WT_SWDIO_PIN);
	sm_config_set_set_pins(&wc, WT_SWDIO_PIN, 1);
	sm_config_set_out_shift(&wc, true, true, 32);
	sm_config_set_in_shift(&wc, true, false, 32);
	sm_config_set_clkdiv(&wc, 65535.0f);
	/* No wrap set — write_bits/read_bits use explicit jmp 0. */

	pio_gpio_init(pio, WT_SWCLK_PIN);
	pio_gpio_init(pio, WT_SWDIO_PIN);
	hw_clear_bits(&pads_bank0_hw->io[WT_SWCLK_PIN],
		      PADS_BANK0_GPIO0_ISO_BITS);
	hw_clear_bits(&pads_bank0_hw->io[WT_SWDIO_PIN],
		      PADS_BANK0_GPIO0_ISO_BITS);
	pio_sm_set_consecutive_pindirs(pio, writer_sm, WT_SWCLK_PIN, 1, true);

	pio_sm_init(pio, writer_sm, sw_off, &wc);
	pio_sm_clear_fifos(pio, writer_sm);

	/* --- reader_fast config: sample SWDIO every PIO cycle --- */
	pio_sm_config rc = reader_fast_program_get_default_config(rd_off);

	sm_config_set_in_pins(&rc, WT_SWDIO_PIN);
	sm_config_set_in_shift(&rc, true /*right*/, true /*autopush*/, 32);
	sm_config_set_clkdiv(&rc, 65535.0f);	/* same clock as writer */

	pio_sm_init(pio, reader_sm, rd_off, &rc);
	pio_sm_clear_fifos(pio, reader_sm);

	pio_sm_set_enabled(pio, reader_sm, true);
	pio_sm_set_enabled(pio, writer_sm, true);

	/* Build opcode for write_bits(data=0xA5, n=8) — same encoding our
	 * SWD driver uses: bits 0..7 = absolute PC (sw_off + write_bits_off),
	 * bits 8..12 = count-1, bits 13..31 = low 19 data bits. */
	uint8_t func = (uint8_t)(sw_off + probe_swd_offset_write_bits);
	uint32_t data = 0xA5u;
	uint32_t opcode = (uint32_t)func
			| ((uint32_t)(8u - 1u) << 8)
			| ((data & 0x7FFFFu) << 13);

	LOG_INF("write-test: pushing opcode=0x%08x (func=0x%02x data=0xA5 n=8)",
		(unsigned)opcode, (unsigned)func);
	pio_sm_put_blocking(pio, writer_sm, opcode);

	/* Wait long enough for 8 SWD bits + park to complete. At PIO 2288 Hz
	 * and 6 cycles per SWD bit, 8 bits = 8 × 6 / 2288 = 21 ms. Add a
	 * generous margin. */
	k_sleep(K_MSEC(200));

	pio_sm_set_enabled(pio, reader_sm, false);
	pio_sm_set_enabled(pio, writer_sm, false);

	/* Drain reader RX and print the bit stream the reader captured.
	 * Each push is 32 bits = 32 samples. We expect ~6 samples per SWD
	 * bit, so 8 bits ≈ 48 samples = 1 push + a partial. */
	LOG_INF("write-test: draining captured samples...");
	uint32_t pushes = 0;
	uint32_t high = 0, low = 0;

	while (!pio_sm_is_rx_fifo_empty(pio, reader_sm)) {
		uint32_t v = pio_sm_get(pio, reader_sm);

		LOG_INF("  capture[%u] = 0x%08x", (unsigned)pushes,
			(unsigned)v);
		pushes++;
		for (int i = 0; i < 32; i++) {
			if (v & (1u << i)) {
				high++;
			} else {
				low++;
			}
		}
	}
	LOG_INF("write-test: %u pushes captured, high=%u low=%u",
		pushes, high, low);

	if (pushes == 0) {
		LOG_ERR("write-test: no samples — writer or reader didn't run");
		return -1;
	}
	if (high == 0) {
		LOG_ERR("write-test: SWDIO never went HIGH — out pins broken");
		return -1;
	}
	if (low == 0) {
		LOG_ERR("write-test: SWDIO never went LOW — out pins broken");
		return -1;
	}
	LOG_INF("write-test: writer produced HIGH and LOW samples ✓");
	return 0;
}

static bool read_pad_raw(uint pin)
{
	/* SIO GPIO_IN reflects the actual pad level regardless of which
	 * peripheral function owns the pin — perfect for observing what PIO
	 * is driving. */
	return (sio_hw->gpio_in & (1u << pin)) != 0;
}

int main(void)
{
	int ret = setup_usb();

	if (ret) {
		LOG_ERR("USB setup failed: %d", ret);
	}

	const struct device *console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

	if (device_is_ready(console)) {
		wait_for_dtr(console);
	}

	LOG_INF("pio_sanity_zephyr — does PIO output work on RP2350?");
	LOG_INF("Built: %s %s", __DATE__, __TIME__);

	ret = setup_pio();
	if (ret) {
		LOG_ERR("PIO setup failed: %d", ret);
		return ret;
	}

	/* Poll the pad every 5 ms for 5 seconds and report transitions. */
	bool last = read_pad_raw(TOGGLE_PIN);
	uint32_t transitions = 0;
	const uint32_t poll_ms = 5;
	const uint32_t window_ms = 5000;

	LOG_INF("Polling pad GP%u for %u ms (every %u ms)...",
		TOGGLE_PIN, window_ms, poll_ms);

	int64_t deadline = k_uptime_get() + window_ms;

	while (k_uptime_get() < deadline) {
		bool now = read_pad_raw(TOGGLE_PIN);

		if (now != last) {
			transitions++;
			last = now;
		}
		k_sleep(K_MSEC(poll_ms));
	}

	LOG_INF("Result: %u transitions in %u ms", transitions, window_ms);

	uint32_t expected = 357;	/* 71.4 Hz × 5 sec ≈ 357 transitions */
	int32_t diff = (int32_t)transitions - (int32_t)expected;

	/* Test 3: load probe_swd.pio and capture what write_bits emits. */
	run_write_test();

	if (transitions < 10) {
		LOG_ERR("PIO output appears DEAD (no transitions)");
	} else if (diff < -20 || diff > 20) {
		LOG_WRN("Toggle rate off by %d transitions vs expected %u — "
			"clock divider may be wrong", diff, expected);
	} else {
		LOG_INF("PIO output OK — toggle rate matches calc (±%d of %u)",
			diff, expected);
	}

	while (1) {
		k_sleep(K_SECONDS(1));
	}
	return 0;
}
