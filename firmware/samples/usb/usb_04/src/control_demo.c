/*
 * control_demo — vendor class, 1 interface, 4 vendor control requests on EP0.
 *
 * Wire-shape carry-forward from usb_03: same 1-iface / 2-bulk-EP descriptor
 * tree (the bulk endpoints still NAK forever — bringing them to life lands in
 * a future "endpoint buffers" sample). The new material is on EP0 only.
 *
 * Vendor requests (bmRequestType.type = VENDOR, recipient = INTERFACE):
 *
 *   bRequest 0x01  GET_UPTIME       IN,  3-stage,  wLength=8   → k_uptime_get()
 *   bRequest 0x02  SET_LOG_MSG      OUT, 3-stage,  wLength≤32  → log payload
 *   bRequest 0x03  SET_LED_BLINK    OUT, 2-stage,  wLength=0   → count in wValue
 *   bRequest 0x04  STALL_ME         either,        -           → -ENOTSUP → STALL
 *
 * See firmware_sample_usb_04.html §3 for the byte-level Setup packets and §4
 * for the EP0 state machine.
 */

#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/usb_ch9.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/net_buf.h>
#include <zephyr/logging/log.h>
#include <errno.h>

LOG_MODULE_REGISTER(control_demo, LOG_LEVEL_INF);


#define DEMO_CLASS_NAME "control_demo"

/* bRequest values — exposed for the host-side verification scripts in §12. */
#define VREQ_GET_UPTIME       0x01
#define VREQ_SET_LOG_MSG      0x02
#define VREQ_SET_LED_BLINK    0x03
#define VREQ_STALL_ME         0x04

#define LOG_MSG_MAX           32U

/* ===========================================================================
 * Descriptor structs — non-const (G2). iface.iInterface is patched at boot.
 * Identical wire shape to usb_03's strings_demo.
 * =========================================================================*/

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
    .bEndpointAddress = USB_EP_DIR_OUT | 0x01,
    .bmAttributes     = USB_EP_TYPE_BULK,
    .wMaxPacketSize   = sys_cpu_to_le16(64U),
    .bInterval        = 0,
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

void control_demo_set_iface_idx(uint8_t idx)
{
    iface.iInterface = idx;
}

/* ===========================================================================
 * Vendor request handlers.
 *
 * Core dispatch (see usbd_ch9.c:976+):
 *   recipient = INTERFACE → routed to the class owning interface wIndex.
 * The class still filters on RequestType.type itself; a recipient=INTERFACE
 * request can be standard / class / vendor, and our class only owns the
 * vendor subset. Anything else → -ENOTSUP (G16).
 *
 * Return-value convention (the one G4 + G16 together encode):
 *   0          → "I handled it." Core completes the transfer normally.
 *   -ENOTSUP   → "Not mine." Core STALLs EP0; host recovers via the usual
 *                CLEAR_FEATURE(ENDPOINT_HALT). This is what VREQ_STALL_ME
 *                returns on purpose.
 *
 * For host-IN requests, fill response bytes into `buf` via net_buf_add_*.
 * The core clamps to MIN(setup->wLength, what we added).
 * =========================================================================*/

static int control_to_host(struct usbd_class_data *const c_data,
                           const struct usb_setup_packet *const setup,
                           struct net_buf *const buf)
{
    ARG_UNUSED(c_data);

    /* G16 — only vendor requests are ours. */
    if (setup->RequestType.type != USB_REQTYPE_TYPE_VENDOR) {
        return -ENOTSUP;
    }

    switch (setup->bRequest) {
    case VREQ_GET_UPTIME: {
        /* 3-stage IN. Host expects 8 bytes; we provide k_uptime_get() in LE.
         * G17 — wLength is host's upper bound. If host requested fewer bytes,
         * the core truncates our 8-byte response. If host requested more, the
         * core appends a ZLP only when our response length is a multiple of
         * the EP0 MPS (64 here) — for an 8-byte payload it never is, so no
         * ZLP needed. */
        if (setup->wLength < sizeof(uint64_t)) {
            LOG_WRN("GET_UPTIME: host wLength=%u < 8 — will truncate",
                    setup->wLength);
        }
        int64_t up = k_uptime_get();
        net_buf_add_le64(buf, (uint64_t)up);
        LOG_INF("GET_UPTIME → %lld ms", (long long)up);
        return 0;
    }

    case VREQ_STALL_ME:
        /* Deliberately refuse. -ENOTSUP propagates → core STALLs EP0. */
        LOG_INF("STALL_ME (IN) → returning -ENOTSUP");
        return -ENOTSUP;

    default:
        LOG_WRN("control_to_host: unknown vendor bRequest 0x%02x",
                setup->bRequest);
        return -ENOTSUP;
    }
}

static int control_to_dev(struct usbd_class_data *const c_data,
                          const struct usb_setup_packet *const setup,
                          const struct net_buf *const buf)
{
    ARG_UNUSED(c_data);

    if (setup->RequestType.type != USB_REQTYPE_TYPE_VENDOR) {
        return -ENOTSUP;
    }

    switch (setup->bRequest) {
    case VREQ_SET_LOG_MSG: {
        /* 3-stage OUT. Host has already sent `setup->wLength` bytes; the core
         * collected them into buf and is calling us to consume. buf->len ==
         * actually-received bytes (could be < wLength if host short-packeted).
         * G18 — clamp to LOG_MSG_MAX to avoid spamming logs with megabytes. */
        size_t n = MIN(buf->len, LOG_MSG_MAX);
        LOG_HEXDUMP_INF(buf->data, n, "SET_LOG_MSG payload:");
        return 0;
    }

    case VREQ_SET_LED_BLINK: {
        /* 2-stage OUT. No DATA stage. Count is in wValue (16-bit).
         * USB allows up to 2 bytes of "data" packed into wValue/wIndex per
         * spec §9.3 — that's the entire point of the 2-stage shape: skip the
         * extra round-trip when payload fits in the Setup packet itself. */
        uint16_t count = setup->wValue;
        LOG_INF("SET_LED_BLINK count=%u (2-stage; no DATA)", count);
        return 0;
    }

    case VREQ_STALL_ME:
        LOG_INF("STALL_ME (OUT) → returning -ENOTSUP");
        return -ENOTSUP;

    default:
        LOG_WRN("control_to_dev: unknown vendor bRequest 0x%02x",
                setup->bRequest);
        return -ENOTSUP;
    }
}

/* ===========================================================================
 * Class API vtable. enable/disable/init carry over from usb_03.
 * =========================================================================*/

static int demo_init(struct usbd_class_data *const c_data)
{
    ARG_UNUSED(c_data);
    LOG_DBG("control_demo: init");
    return 0;
}

static void demo_enable(struct usbd_class_data *const c_data)
{
    ARG_UNUSED(c_data);
    LOG_INF("control_demo: configured (enabled)");
}

static void demo_disable(struct usbd_class_data *const c_data)
{
    ARG_UNUSED(c_data);
    LOG_INF("control_demo: deconfigured (disabled)");
}

static void *demo_get_desc(struct usbd_class_data *const c_data,
                           const enum usbd_speed speed)
{
    ARG_UNUSED(c_data);
    if (speed == USBD_SPEED_FS) {
        return fs_desc;
    }
    return NULL;
}

static const struct usbd_class_api demo_api = {
    .init             = demo_init,
    .enable           = demo_enable,
    .disable          = demo_disable,
    .get_desc         = demo_get_desc,
    .control_to_host  = control_to_host,
    .control_to_dev   = control_to_dev,
};

/* G3 — must match DEMO_CLASS_NAME in main.c. */
USBD_DEFINE_CLASS(control_demo, &demo_api, NULL, NULL);
