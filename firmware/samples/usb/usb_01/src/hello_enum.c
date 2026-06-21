/*
 * usb_01_hello_enum — minimal vendor class implementation
 * --------------------------------------------------------
 * Exposes ONE interface, ZERO data endpoints, vendor class 0xFF/0xFF/0xFF.
 * The device enumerates and idles; no driver loads on the host side; no
 * data flows.  Total descriptor footprint: 9-byte interface descriptor
 * inside a 9-byte configuration wrapper = wTotalLength of 18.
 *
 * Pairs with USB Book Ch 05 — every byte the host reads here matches the
 * worked example in the chapter.
 */

#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/usb_ch9.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(hello_enum, LOG_LEVEL_INF);

/* ---------------------------------------------------------------------------
 * Interface descriptor — 9 bytes, vendor-specific, no endpoints.
 * ------------------------------------------------------------------------- */
static struct usb_if_descriptor hello_enum_iface = {
	.bLength = sizeof(struct usb_if_descriptor),
	.bDescriptorType = USB_DESC_INTERFACE, /* 0x04 */
	.bInterfaceNumber = 0,				   /* assigned at init time   */
	.bAlternateSetting = 0,
	.bNumEndpoints = 0,				   /* the whole point         */
	.bInterfaceClass = USB_BCC_VENDOR, /* 0xFF                    */
	.bInterfaceSubClass = 0xFF,
	.bInterfaceProtocol = 0xFF,
	.iInterface = 0, /* no string               */
};

/* ---------------------------------------------------------------------------
 * NULL-terminated array of descriptor pointers — what get_desc returns.
 *
 * The USB-Device-Next core walks this array as `struct usb_desc_header **`,
 * stopping when it sees NULL.  Each entry must be castable to that base
 * struct, so we cast our concrete usb_if_descriptor here.
 * ------------------------------------------------------------------------- */
static struct usb_desc_header *hello_enum_fs_desc[] = {
	(struct usb_desc_header *)&hello_enum_iface,
	NULL,
};

/* ---------------------------------------------------------------------------
 * Class-API callbacks.  Only the four we actually need are populated; the
 * rest of the slots are left NULL and the core handles that.
 * ------------------------------------------------------------------------- */

static int hello_enum_init(struct usbd_class_data *const c_data)
{
	LOG_DBG("hello_enum: init");
	return 0;
}

static void hello_enum_enable(struct usbd_class_data *const c_data)
{
	/* Called when host completes SET_CONFIGURATION(1) — we are now
	 * "configured" in spec parlance.  This is the Ch-05 milestone. */
	LOG_INF("hello_enum: configured (enabled)");
}

static void hello_enum_disable(struct usbd_class_data *const c_data)
{
	/* SET_CONFIGURATION(0) or unplug. */
	LOG_INF("hello_enum: deconfigured (disabled)");
}

static void *hello_enum_get_desc(struct usbd_class_data *const c_data,
								 const enum usbd_speed speed)
{
	if (speed == USBD_SPEED_FS)
	{
		return hello_enum_fs_desc;
	}
	return NULL;
}

/* ---------------------------------------------------------------------------
 * Vtable.  Optional callbacks left as NULL:
 *   feature_halt, update, control_to_dev, control_to_host, request,
 *   suspended, resumed, sof, shutdown
 * The core checks these for NULL before calling.
 * ------------------------------------------------------------------------- */
static const struct usbd_class_api hello_enum_api = {
	.init = hello_enum_init,
	.enable = hello_enum_enable,
	.disable = hello_enum_disable,
	.get_desc = hello_enum_get_desc,
};

/* ---------------------------------------------------------------------------
 * Class registration.
 *
 *   class_name : the string used by main.c's usbd_register_class() call —
 *                must match exactly: "hello_enum".
 *   api        : pointer to the vtable above.
 *   priv       : per-instance private data; we have none.
 *   v_reqs     : vendor-request handler; we have none.
 * ------------------------------------------------------------------------- */
USBD_DEFINE_CLASS(hello_enum, &hello_enum_api, NULL, NULL);