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
#include <zephyr/usb/usbd.h>

#include <sample_usbd.h>

LOG_MODULE_REGISTER(edev_dapv2, LOG_LEVEL_INF);

#define LED_NODE	DT_ALIAS(led_status)
#define HEARTBEAT_MS	1000

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET_OR(LED_NODE, gpios, {0});

/*
 * DAP Link context bound to the SWDP device declared in the board overlay.
 * M2 uses the upstream zephyr,swdp-gpio bit-banger. M3 swaps the DT
 * compatible to our zephyr,swdp-pio-rpi-pico driver — no code change here.
 */
DAP_LINK_CONTEXT_DEFINE(edev_dap_ctx, DEVICE_DT_GET_ONE(zephyr_swdp_gpio));

static int setup_usb(void)
{
	struct usbd_context *ctx = sample_usbd_setup_device(NULL);

	if (ctx == NULL) {
		LOG_ERR("USBD setup failed");
		return -ENODEV;
	}

	int ret = usbd_init(ctx);
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

	while (1) {
		if (gpio_is_ready_dt(&led)) {
			gpio_pin_toggle_dt(&led);
		}
		k_sleep(K_MSEC(HEARTBEAT_MS));
	}

	return 0;
}
