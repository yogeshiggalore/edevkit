/*
 * usb_04 — control transfers in depth.
 *
 * Boot flow is byte-identical to usb_03 (5 explicit string adds → iface_str
 * idx patch → usbd_add_configuration → register_class → init → enable). The
 * new material is entirely inside control_demo.c, where control_to_host /
 * control_to_dev handle four vendor requests on EP0.
 *
 * See firmware_sample_usb_04.html for the design narrative + byte-level
 * Setup packets + EP0 state machine.
 */

#include <stdio.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(usb_04, LOG_LEVEL_INF);

#define DEMO_CLASS_NAME "control_demo"

extern void control_demo_set_iface_idx(uint8_t idx);

/* ===========================================================================
 * Static string descriptors. Order assigns indices (G7, G11).
 * =========================================================================*/

USBD_DESC_LANG_DEFINE(lang_desc);                                          /* G11 */
USBD_DESC_MANUFACTURER_DEFINE(mfr_desc,     "edevkit");
USBD_DESC_PRODUCT_DEFINE     (product_desc, "usb_04 - control_demo");      /* G12: ASCII7 only */
USBD_DESC_CONFIG_DEFINE      (cfg_str,      "Default config");
USBD_DESC_STRING_DEFINE      (iface_str,    "Vendor (control demo, 4 EP0 requests)",
                              USBD_DUT_STRING_INTERFACE);

/* ===========================================================================
 * Custom serial-number string descriptor (G14). Same hand-rolled pattern as
 * usb_03 — writable buffer, populated from hwinfo at boot.
 * =========================================================================*/

#define SERIAL_TEMPLATE   "EDV-0000-0000-0000"
#define SERIAL_BUF_SIZE   sizeof(SERIAL_TEMPLATE)

static char serial_buf[SERIAL_BUF_SIZE] = SERIAL_TEMPLATE;

static struct usbd_desc_node serial_node = {
    .str = {
        .utype  = USBD_DUT_STRING_SERIAL_NUMBER,
        .ascii7 = true,
    },
    .ptr             = serial_buf,
    .bLength         = USB_STRING_DESCRIPTOR_LENGTH(SERIAL_TEMPLATE),
    .bDescriptorType = USB_DESC_STRING,
};

static int serial_init(void)
{
    uint8_t id[8] = {0};
    ssize_t n = hwinfo_get_device_id(id, sizeof(id));

    if (n < 6) {
        LOG_ERR("hwinfo_get_device_id returned %d (need >= 6)", (int)n);
        return -EIO;
    }

    int written = snprintf(serial_buf, sizeof(serial_buf),
                           "EDV-%02X%02X-%02X%02X-%02X%02X",
                           id[0], id[1], id[2], id[3], id[4], id[5]);
    if (written != (int)(sizeof(serial_buf) - 1)) {
        LOG_ERR("serial format truncated (wrote %d, expected %d)",
                written, (int)(sizeof(serial_buf) - 1));
        return -ENOSPC;
    }

    LOG_INF("serial: %s", serial_buf);
    return 0;
}

/* ===========================================================================
 * Configuration. cfg_str auto-registered by usbd_add_configuration() — G15.
 * =========================================================================*/

USBD_CONFIGURATION_DEFINE(cfg, USB_SCD_REMOTE_WAKEUP, 50, &cfg_str);

/* ===========================================================================
 * Device-level identity. PID 0xede4, bcdDevice 0x0104.
 * =========================================================================*/

USBD_DEVICE_DEFINE(usbd_ctx, DEVICE_DT_GET(DT_NODELABEL(usbd)),
                   0x1209, 0xede4);

int main(void)
{
    int err;

    (void)serial_init();

    err = usbd_add_descriptor(&usbd_ctx, &lang_desc);
    if (err) { LOG_ERR("add lang: %d", err);          return -1; }
    err = usbd_add_descriptor(&usbd_ctx, &mfr_desc);
    if (err) { LOG_ERR("add mfr: %d", err);           return -1; }
    err = usbd_add_descriptor(&usbd_ctx, &product_desc);
    if (err) { LOG_ERR("add product: %d", err);       return -1; }
    err = usbd_add_descriptor(&usbd_ctx, &serial_node);
    if (err) { LOG_ERR("add serial: %d", err);        return -1; }
    err = usbd_add_descriptor(&usbd_ctx, &iface_str);
    if (err) { LOG_ERR("add iface_str: %d", err);     return -1; }

    uint8_t iface_idx = usbd_str_desc_get_idx(&iface_str);
    LOG_INF("iface_str registered at idx %u", iface_idx);
    control_demo_set_iface_idx(iface_idx);

    err = usbd_add_configuration(&usbd_ctx, USBD_SPEED_FS, &cfg);
    if (err) { LOG_ERR("add cfg: %d", err); return -1; }

    err = usbd_register_class(&usbd_ctx, DEMO_CLASS_NAME,
                              USBD_SPEED_FS, 1);
    if (err) { LOG_ERR("register class: %d", err); return -1; }

    err = usbd_init(&usbd_ctx);
    if (err) { LOG_ERR("usbd_init: %d", err); return -1; }

    err = usbd_enable(&usbd_ctx);
    if (err) { LOG_ERR("usbd_enable: %d", err); return -1; }

    LOG_INF("usb_04 control_demo: enabled, waiting on EP0 vendor requests");

    while (true) {
        k_sleep(K_FOREVER);
    }
    return 0;
}
