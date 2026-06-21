/*
 * descriptors_demo — vendor class, 1 interface, 2 bulk endpoints.
 *
 * Declares a complete descriptor tree (device → cfg → iface → 2 ep)
 * for teaching purposes. Endpoints are advertised but no buffers are
 * ever enqueued — host bulk reads/writes will NAK until usb_04 wires
 * up the data path.
 *
 * Pattern derived from Zephyr's own subsys/usb/device_next/class/loopback.c
 * (Apache-2.0). See attribution at the end of firmware_sample_usb_02.html.
 */

#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/usb_ch9.h>
#include <zephyr/sys/byteorder.h>       /* sys_cpu_to_le16  */
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(descriptors_demo, LOG_LEVEL_INF);

/* G3 defense: same string as in main.c. If the two ever drift, registration
 * silently returns -ENOENT and the device never enumerates. Keep both
 * usages anchored to a single literal. */
#define DESC_CLASS_NAME "descriptors_demo"

/* ===========================================================================
 * Descriptor structs — non-const because the stack writes into them at init.
 * (G2 defense: don't const these. Don't even const the array.)
 * =========================================================================*/

static struct usb_if_descriptor iface = {
    .bLength            = sizeof(struct usb_if_descriptor),
    .bDescriptorType    = USB_DESC_INTERFACE,        /* 0x04             */
    .bInterfaceNumber   = 0,                         /* written by stack */
    .bAlternateSetting  = 0,
    .bNumEndpoints      = 2,                         /* CHANGED vs usb_01 */
    .bInterfaceClass    = USB_BCC_VENDOR,            /* 0xFF             */
    .bInterfaceSubClass = 0xFF,
    .bInterfaceProtocol = 0xFF,
    .iInterface         = 0,                         /* no string        */
};

/*
 * Endpoint addresses follow the convention IN and OUT share the same EP
 * number, only the direction bit differs. EP1 OUT = 0x01, EP1 IN = 0x81.
 * (G10 defense — pair on the same EP number to conserve UDC resources.)
 *
 * bInterval = 0 is mandatory for bulk per USB 2.0 §9.6.6 Table 9-13. (G9)
 *
 * wMaxPacketSize wrapped in sys_cpu_to_le16() to be byte-order-correct on
 * any architecture. (G8) On RP2350 (little-endian) this is a no-op, but
 * documents intent and matches Zephyr's loopback.c reference pattern.
 */

static struct usb_ep_descriptor ep_out = {
    .bLength          = sizeof(struct usb_ep_descriptor),
    .bDescriptorType  = USB_DESC_ENDPOINT,           /* 0x05             */
    .bEndpointAddress = USB_EP_DIR_OUT | 0x01,       /* 0x01 — bulk OUT  */
    .bmAttributes     = USB_EP_TYPE_BULK,            /* 0x02             */
    .wMaxPacketSize   = sys_cpu_to_le16(64U),        /* FS bulk max      */
    .bInterval        = 0,
};

static struct usb_ep_descriptor ep_in = {
    .bLength          = sizeof(struct usb_ep_descriptor),
    .bDescriptorType  = USB_DESC_ENDPOINT,
    .bEndpointAddress = USB_EP_DIR_IN  | 0x01,       /* 0x81 — bulk IN   */
    .bmAttributes     = USB_EP_TYPE_BULK,
    .wMaxPacketSize   = sys_cpu_to_le16(64U),
    .bInterval        = 0,
};

/* ===========================================================================
 * The NULL-terminated descriptor pointer array. (G1 defense.)
 *
 * Order on the wire mirrors order in this array, AFTER the configuration
 * descriptor that the stack prepends:
 *
 *     [ cfg ]  [ iface ]  [ ep_out ]  [ ep_in ]
 *      9         9          7           7        =  32 bytes (wTotalLength)
 *
 * Each entry is cast to the base header type so the core can walk them
 * uniformly via bLength.
 * =========================================================================*/

static struct usb_desc_header *fs_desc[] = {
    (struct usb_desc_header *) &iface,
    (struct usb_desc_header *) &ep_out,
    (struct usb_desc_header *) &ep_in,
    NULL,                                            /* terminator */
};

/* ===========================================================================
 * Class API callbacks — only the four we need; the rest stay NULL and the
 * core checks for NULL before calling. (G4 defense: we add no half-stubs.)
 * =========================================================================*/

static int demo_init(struct usbd_class_data *const c_data)
{
    LOG_DBG("descriptors_demo: init");
    return 0;
}

static void demo_enable(struct usbd_class_data *const c_data)
{
    /* Fired on SET_CONFIGURATION(1). Host considers us "configured" now. */
    LOG_INF("descriptors_demo: configured (enabled)");
}

static void demo_disable(struct usbd_class_data *const c_data)
{
    /* SET_CONFIGURATION(0) or unplug. */
    LOG_INF("descriptors_demo: deconfigured (disabled)");
}

static void *demo_get_desc(struct usbd_class_data *const c_data,
                           const enum usbd_speed speed)
{
    if (speed == USBD_SPEED_FS) {
        return fs_desc;
    }
    /* HS / SS not supported on RP2350. Returning NULL is correct. */
    return NULL;
}

/* ===========================================================================
 * Vtable. Optional callbacks deliberately left as NULL:
 *   feature_halt, update, control_to_dev, control_to_host, request,
 *   suspended, resumed, sof, shutdown
 * The core treats NULL as "not implemented" and STALLs / no-ops as
 * appropriate — which is what we want for a descriptor-only sample.
 * =========================================================================*/
static const struct usbd_class_api demo_api = {
    .init     = demo_init,
    .enable   = demo_enable,
    .disable  = demo_disable,
    .get_desc = demo_get_desc,
};

/* G3 defense: literal string MUST match DESC_CLASS_NAME in main.c. */
USBD_DEFINE_CLASS(descriptors_demo, &demo_api, NULL, NULL);