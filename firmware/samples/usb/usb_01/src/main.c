#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(usb_01, LOG_LEVEL_INF);

USBD_DESC_LANG_DEFINE(lang);
USBD_DESC_MANUFACTURER_DEFINE(mfr, "edevkit");
USBD_DESC_PRODUCT_DEFINE(product, "usb_01");
USBD_DESC_SERIAL_NUMBER_DEFINE(serial);

/*
 * USB_SCD_REMOTE_WAKEUP = bit 5 of bmAttributes
 * 50 (decimal) = 100 mA in 2-mA units
 * Last argument is an optional configuration-string descriptor (NULL = none)
 */
USBD_CONFIGURATION_DEFINE(cfg, USB_SCD_REMOTE_WAKEUP, 50, NULL);

/* idVendor — pid.codes umbrella */
/* idProduct — usb_01 */
USBD_DEVICE_DEFINE(usbd_ctx, DEVICE_DT_GET(DT_NODELABEL(usbd)), 0x1209, 0xede1);

int main(void)
{
	int err;

	/* (1) Add string descriptors to the context.
	 *     Order must match the index slots in the device descriptor.
	 */
	err = usbd_add_descriptor(&usbd_ctx, &lang);
	if (err)
	{
		LOG_ERR("add lang: %d", err);
		return -1;
	}
	err = usbd_add_descriptor(&usbd_ctx, &mfr);
	if (err)
	{
		LOG_ERR("add mfr: %d", err);
		return -1;
	}
	err = usbd_add_descriptor(&usbd_ctx, &product);
	if (err)
	{
		LOG_ERR("add prod: %d", err);
		return -1;
	}
	err = usbd_add_descriptor(&usbd_ctx, &serial);
	if (err)
	{
		LOG_ERR("add ser: %d", err);
		return -1;
	}

	/* (2) Add the configuration to the context, declaring its speed
	 *     (we're FS-only on RP2350).  The macro hidden in step (3) of
	 *     the class file is what attaches the interface to this config.
	 */
	err = usbd_add_configuration(&usbd_ctx, USBD_SPEED_FS, &cfg);
	if (err)
	{
		LOG_ERR("add cfg: %d", err);
		return -1;
	}

	/* (3) Register the no-op vendor class instance into the configuration.
	 *     The string "hello_enum" must match the name passed to
	 *     USBD_DEFINE_CLASS in src/hello_enum.c.
	 */
	err = usbd_register_class(&usbd_ctx, "hello_enum",
							  USBD_SPEED_FS, 1 /* config index */);
	if (err)
	{
		LOG_ERR("register class: %d", err);
		return -1;
	}

	/* (4) Finalize the descriptor tree (validates, links wTotalLength, etc.)
	 */
	err = usbd_init(&usbd_ctx);
	if (err)
	{
		LOG_ERR("usbd_init: %d", err);
		return -1;
	}

	/* (5) Connect the pull-up — from this moment the host can see us.
	 */
	err = usbd_enable(&usbd_ctx);
	if (err)
	{
		LOG_ERR("usbd_enable: %d", err);
		return -1;
	}

	LOG_INF("usb_01 hello_enum: USB enabled, idle");

	/* (6) Nothing for main thread to do.  The USB stack runs in its
	 *     own work queue; events come in via the class callbacks.
	 */
	while (true)
	{
		k_sleep(K_FOREVER);
	}
	return 0;
}