/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * edev_dapv2 (Zephyr port) — M1 entry point.
 *
 * Goal: bring up the board on Zephyr, prove USB CDC console + LED heartbeat
 * work end-to-end before any DAP code lands. Mirrors src/main.c in the pico-sdk
 * project (firmware/projects/edev_dapv2/src/main.c) — same boot order, same
 * banner content, same heartbeat cadence — so a side-by-side comparison while
 * porting is straightforward.
 */

/* Order matters: kernel.h must precede dap_link.h — the latter uses atomic_t
 * without including <zephyr/sys/atomic.h> itself (upstream header omission). */
#include <zephyr/kernel.h>

#include <zephyr/dap/dap_link.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/bos.h>
#include <zephyr/usb/msos_desc.h>
#include <zephyr/usb/usbd.h>

#include <sample_usbd.h>

#include <pico/bootrom.h>

LOG_MODULE_REGISTER(edev_dapv2, LOG_LEVEL_INF);

/* MS OS 2.0 BOS — pulled in *after* LOG_MODULE_REGISTER because the static
 * msosv2_to_host_cb in the header uses LOG_INF. The CMSIS-DAP v2 standard
 * device interface GUID it advertises is what makes Windows auto-bind WINUSB
 * (and what makes probe-rs / pyocd / OpenOCD discover the probe). */
#include "msosv2.h"

/* probe-rs gates CMSIS-DAP v2 discovery on bcdDevice >= 0x0220 — see the
 * memory note reference_probe_rs_bcd_device_gate. The Zephyr USBD default
 * uses Zephyr's kernel version; override at runtime to 0x0220 to satisfy the
 * gate without forking the framework. */
#define EDEV_DAPV2_BCD_DEVICE	0x0220

#define LED_NODE	DT_ALIAS(led_status)
#define HEARTBEAT_MS	1000

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET_OR(LED_NODE, gpios, {0});

/*
 * DAP Link context bound to the SWDP device declared in the board overlay.
 * The compatible string matches the binding in
 * dts/bindings/raspberrypi,pico-swdp-pio.yaml — the PIO-driven backend
 * preserves the 25 MHz SWD speed of the pico-sdk firmware.
 */
DAP_LINK_CONTEXT_DEFINE(edev_dap_ctx, DEVICE_DT_GET_ONE(zephyr_swdp_gpio));

static int setup_usb(void)
{
	struct usbd_context *ctx = sample_usbd_setup_device(NULL);

	if (ctx == NULL) {
		LOG_ERR("USBD setup failed");
		return -ENODEV;
	}

	int ret = usbd_add_descriptor(ctx, &bos_vreq_msosv2);
	if (ret) {
		LOG_ERR("Failed to add MS OS 2.0 BOS: %d", ret);
		return ret;
	}

	ret = usbd_device_set_bcd_device(ctx, EDEV_DAPV2_BCD_DEVICE);
	if (ret) {
		LOG_WRN("Could not set bcdDevice: %d (probe-rs may not list)",
			ret);
	}

	ret = usbd_init(ctx);
	if (ret) {
		LOG_ERR("usbd_init failed: %d", ret);
		return ret;
	}

	ret = usbd_enable(ctx);
	if (ret) {
		LOG_ERR("usbd_enable failed: %d", ret);
		return ret;
	}

	return 0;
}

static void wait_for_dtr(const struct device *console)
{
	uint32_t dtr = 0;

	while (!dtr) {
		uart_line_ctrl_get(console, UART_LINE_CTRL_DTR, &dtr);
		k_sleep(K_MSEC(100));
	}
}

/*
 * 1200-baud-to-BOOTSEL reboot trick.
 *
 * The pico-sdk firmware ports this via TinyUSB's tud_cdc_line_coding_cb. The
 * Zephyr USB-Device-Next CDC ACM driver doesn't expose a line-coding callback,
 * so we poll the console's baud rate every 250 ms; if the host opens at 1200
 * baud (e.g. `stty -f /dev/cu.usbmodem* 1200`) we hand off to the RP2350
 * bootrom to enter UF2 mass-storage mode. This is the same UX the host-side
 * `picotool` / pico-sdk firmware expects.
 */
#define EDEV_REBOOT_MAGIC_BAUD	1200u

static void bootsel_watch_entry(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	const struct device *console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

	while (!device_is_ready(console)) {
		k_sleep(K_MSEC(100));
	}

	while (1) {
		uint32_t baud = 0;

		uart_line_ctrl_get(console, UART_LINE_CTRL_BAUD_RATE, &baud);
		if (baud == EDEV_REBOOT_MAGIC_BAUD) {
			LOG_WRN("1200-baud — entering BOOTSEL");
			k_sleep(K_MSEC(50));	/* let the log line drain */
			reset_usb_boot(0, 0);
			/* not reached */
		}
		k_sleep(K_MSEC(250));
	}
}

K_THREAD_DEFINE(bootsel_watch, 1024, bootsel_watch_entry, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 1000);

static int setup_dap(void)
{
	int ret = dap_link_init(&edev_dap_ctx);

	if (ret) {
		LOG_ERR("dap_link_init failed: %d", ret);
		return ret;
	}

	ret = dap_link_backend_usb_init(&edev_dap_ctx);
	if (ret) {
		LOG_ERR("dap_link_backend_usb_init failed: %d", ret);
		return ret;
	}

	return 0;
}

int main(void)
{
	int ret;

	if (!gpio_is_ready_dt(&led)) {
		LOG_WRN("Status LED not ready");
	} else {
		gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	}

	ret = setup_dap();
	if (ret) {
		return ret;
	}

	ret = setup_usb();
	if (ret) {
		return ret;
	}

	/*
	 * Wait for the host to open the CDC ACM port before printing the boot
	 * banner — otherwise the first ~hundred ms of log lines vanish into a
	 * not-yet-enumerated endpoint. Same idea as the pico-sdk firmware,
	 * which calls tud_cdc_connected() before logging the banner.
	 */
	const struct device *console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	if (device_is_ready(console)) {
		wait_for_dtr(console);
	}

	LOG_INF("edev_dapv2 (Zephyr port) — boot ok");
	LOG_INF("Built: %s %s", __DATE__, __TIME__);

	/* SWCLK keep-alive is staged but LEFT OFF at boot. Live-fire
	 * testing 2026-06-29 showed that an asynchronous SWCLK pulse train
	 * (5 kHz isolated single-cycle pulses) breaks SWD protocol timing
	 * — the chip's frame aligner counts the keep-alive cycles, so
	 * probe-rs's subsequent DPIDR read lands at the wrong bit offset
	 * and returns Protocol Error.
	 *
	 * The right fix is to insert idle cycles INSIDE Zephyr's
	 * drivers/dp/swdp_bitbang.c sw_transfer (the WAIT-ACK retry path
	 * currently exits without clocking; idle cycles between retries
	 * would keep the debug clock alive in-protocol). That's an
	 * upstream patch — tracked in NRF5340_FIRMWARE_GAP.md and the
	 * memory note nrf5340-edev-vs-jlink-firmware-gap-2026-06-29.
	 *
	 * Leaving the timer scaffolding in place so the next session can
	 * call edev_swclk_keepalive_resume() once the in-protocol path is
	 * built. For now it stays gated off via keepalive_active=0. */

	while (1) {
		if (gpio_is_ready_dt(&led)) {
			gpio_pin_toggle_dt(&led);
		}
		k_sleep(K_MSEC(HEARTBEAT_MS));
	}

	return 0;
}
