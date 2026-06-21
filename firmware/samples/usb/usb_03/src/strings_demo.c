/*
 * strings_demo — vendor class, 1 interface (now named), 2 bulk endpoints.
 *
 * Identical wire shape to usb_02's descriptors_demo. The only difference is
 * iface.iInterface is settable from main.c via strings_demo_set_iface_idx()
 * after the interface-name string has been registered (G13).
 */

#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/usb_ch9.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(strings_demo, LOG_LEVEL_INF);


#define DEMO_CLASS_NAME "strings_demo"

static struct usb_if_descriptor iface = {
    .bLength            = sizeof(struct usb_if_descriptor),
    .bDescriptorType    = USB_DESC_INTERFACE,
    .bInterfaceNumber   = 0,                       /* written by stack */
    .bAlternateSetting  = 0,
    .bNumEndpoints      = 2,
    .bInterfaceClass    = USB_BCC_VENDOR,
    .bInterfaceSubClass = 0xFF,
    .bInterfaceProtocol = 0xFF,
    .iInterface         = 0,                       /* patched at boot — G13 */
};

static struct usb_ep_descriptor ep_out = {
    .bLength          = sizeof(struct usb_ep_descriptor),
    .bDescriptorType  = USB_DESC_ENDPOINT,
    .bEndpointAddress = USB_EP_DIR_OUT | 0x01,     /* G10 */
    .bmAttributes     = USB_EP_TYPE_BULK,
    .wMaxPacketSize   = sys_cpu_to_le16(64U),      /* G8 */
    .bInterval        = 0,                          /* G9 */
};

static struct usb_ep_descriptor ep_in = {
    .bLength          = sizeof(struct usb_ep_descriptor),
    .bDescriptorType  = USB_DESC_ENDPOINT,
    .bEndpointAddress = USB_EP_DIR_IN  | 0x01,
    .bmAttributes     = USB_EP_TYPE_BULK,
    .wMaxPacketSize   = sys_cpu_to_le16(64U),
    .bInterval        = 0,
};

static struct usb_desc_header *fs_desc[] = {
    (struct usb_desc_header *) &iface,
    (struct usb_desc_header *) &ep_out,
    (struct usb_desc_header *) &ep_in,
    NULL,
};

void strings_demo_set_iface_idx(uint8_t idx)
{
    iface.iInterface = idx;
}

/* ===========================================================================
 * Class API callbacks. Identical to usb_02 — strings are out-of-band.
 * =========================================================================*/

static int demo_init(struct usbd_class_data *const c_data)
{
    LOG_DBG("strings_demo: init");
    return 0;
}

static void demo_enable(struct usbd_class_data *const c_data)
{
    LOG_INF("strings_demo: configured (enabled)");
}

static void demo_disable(struct usbd_class_data *const c_data)
{
    LOG_INF("strings_demo: deconfigured (disabled)");
}

static void *demo_get_desc(struct usbd_class_data *const c_data,
                           const enum usbd_speed speed)
{
    if (speed == USBD_SPEED_FS) {
        return fs_desc;
    }
    return NULL;
}

static const struct usbd_class_api demo_api = {
    .init     = demo_init,
    .enable   = demo_enable,
    .disable  = demo_disable,
    .get_desc = demo_get_desc,
};

/* G3 — must match DEMO_CLASS_NAME in main.c. */
USBD_DEFINE_CLASS(strings_demo, &demo_api, NULL, NULL);