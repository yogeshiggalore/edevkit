/*
 * usb_03 — string descriptors and per-unit identity.
 *
 * Builds on usb_02's descriptor tree with a complete set of standard string
 * descriptors (LANGID, Manufacturer, Product, Serial, Configuration, Interface)
 * and a runtime-formatted EDV-XXXX-YYYY-ZZZZ serial derived from the RP2350
 * 64-bit chip ID. Demonstrates that descriptor-index references are late-bound:
 * the iface descriptor's iInterface field is patched after string registration,
 * before usbd_init() validates the tree.
 *
 * See firmware_sample_usb_03.html for the design narrative + byte-level layout.
 */

#include <stdio.h>                      /* snprintf */
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/drivers/hwinfo.h>       /* hwinfo_get_device_id (G14) */
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(usb_03, LOG_LEVEL_INF);

/* G3 defense: single source of truth for the class-registration string. */
#define DEMO_CLASS_NAME "strings_demo"

/* Exported by strings_demo.c — patches iface.iInterface (G13). */
extern void strings_demo_set_iface_idx(uint8_t idx);

/* ===========================================================================
 * Static string descriptors (compile-time literals).
 * Order of registration in main() determines index assignment.
 * =========================================================================*/

USBD_DESC_LANG_DEFINE(lang_desc);                                          /* G11 */
USBD_DESC_MANUFACTURER_DEFINE(mfr_desc,     "edevkit");
USBD_DESC_PRODUCT_DEFINE     (product_desc, "usb_03 - strings_demo");      /* G12: ASCII7 only */
USBD_DESC_CONFIG_DEFINE      (cfg_str,      "Default config");
USBD_DESC_STRING_DEFINE      (iface_str,    "Vendor (descriptor demo, no I/O)",
                              USBD_DUT_STRING_INTERFACE);

/* ===========================================================================
 * Custom serial-number string descriptor (G14).
 *
 * Hand-rolled struct usbd_desc_node + writable buffer. The descriptor's
 * .ptr is initialised once at file scope to point at serial_buf; the buffer
 * itself is overwritten at boot with the chip-ID-derived value.
 *
 * USB_STRING_DESCRIPTOR_LENGTH(s) = sizeof(s) * 2  (verified in usbd.h:56).
 * For "EDV-XXXX-YYYY-ZZZZ" sizeof = 19, bLength = 38, layout: 2-byte header
 * + 18 UTF-16LE code units.
 * =========================================================================*/

#define SERIAL_TEMPLATE   "EDV-0000-0000-0000"        /* placeholder, same length as runtime value */
#define SERIAL_BUF_SIZE   sizeof(SERIAL_TEMPLATE)     /* 19 incl. NUL */

static char serial_buf[SERIAL_BUF_SIZE] = SERIAL_TEMPLATE;

static struct usbd_desc_node serial_node = {
    .str = {
        .utype  = USBD_DUT_STRING_SERIAL_NUMBER,
        .ascii7 = true,                               /* G12 */
    },
    .ptr             = serial_buf,
    .bLength         = USB_STRING_DESCRIPTOR_LENGTH(SERIAL_TEMPLATE),
    .bDescriptorType = USB_DESC_STRING,
};

/* Read the RP2350 64-bit chip ID and format the first 6 bytes (12 hex chars)
 * into the serial_buf. Same sizeof, same bLength — only the bytes change. */
static int serial_init(void)
{
    uint8_t id[8] = {0};
    ssize_t n = hwinfo_get_device_id(id, sizeof(id));

    if (n < 6) {
        LOG_ERR("hwinfo_get_device_id returned %d (need >= 6)", (int)n);
        /* Don't fail boot — the placeholder "EDV-0000-0000-0000" is still a
         * valid string. Useful in CI / emulator where HWINFO might be stub. */
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
 * Configuration descriptor.
 *
 * Third arg is now &cfg_str (was NULL in usb_02). It is stored as
 * cfg.str_desc_nd; usbd_add_configuration() will register cfg_str for us and
 * write back iConfiguration with the resulting index.
 *
 * G15 — do NOT also call usbd_add_descriptor(&cfg_str) yourself. The stack
 * does it inside usbd_add_configuration(); a manual pre-add lands cfg_str on
 * the descriptors dlist, and the stack's second add returns -EALREADY (-120
 * in Zephyr's minimal libc) — surfaced as "Failed to add configuration
 * string descriptor" + "add cfg: -120".
 * =========================================================================*/

USBD_CONFIGURATION_DEFINE(cfg, USB_SCD_REMOTE_WAKEUP, 50, &cfg_str);

/* ===========================================================================
 * Device-level identity. PID bumped to 0xede3; bcdDevice bumped to 0x0103
 * to invalidate the macOS descriptor cache after coming from usb_02.
 * =========================================================================*/

USBD_DEVICE_DEFINE(usbd_ctx, DEVICE_DT_GET(DT_NODELABEL(usbd)),
                   0x1209, 0xede3);

int main(void)
{
    int err;

    /* (0) Generate the per-unit serial BEFORE registering the descriptor. */
    (void)serial_init();        /* placeholder is a valid fallback if this errors */

    /* (1)–(5) Register string descriptors that have no auto-add path.
     *         Order assigns indices. G7 — order matters; G11 — LANGID first.
     *         cfg_str is intentionally NOT added here: usbd_add_configuration()
     *         registers it on our behalf (see G15 next to USBD_CONFIGURATION_DEFINE). */
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

    /* (5a) G13 — patch the iface descriptor's iInterface field to whatever
     *      index the stack just assigned to iface_str. MUST happen before
     *      usbd_init() validates the tree.                                   */
    uint8_t iface_idx = usbd_str_desc_get_idx(&iface_str);
    LOG_INF("iface_str registered at idx %u", iface_idx);
    strings_demo_set_iface_idx(iface_idx);

    /* (6) Add the configuration. The stack auto-registers cfg_str (next idx)
     *     and writes back iConfiguration in cfg_desc — we never touch either. */
    err = usbd_add_configuration(&usbd_ctx, USBD_SPEED_FS, &cfg);
    if (err) { LOG_ERR("add cfg: %d", err); return -1; }

    /* (7) Attach the strings_demo class instance to configuration 1. */
    err = usbd_register_class(&usbd_ctx, DEMO_CLASS_NAME,
                              USBD_SPEED_FS, 1);
    if (err) { LOG_ERR("register class: %d", err); return -1; }

    /* (8) Validate + finalize. The stack walks every class's fs_desc[] AND
     *     re-validates every iInterface / iConfiguration index reference now. */
    err = usbd_init(&usbd_ctx);
    if (err) { LOG_ERR("usbd_init: %d", err); return -1; }

    /* (9) Connect the pull-up. Host can now see us. */
    err = usbd_enable(&usbd_ctx);
    if (err) { LOG_ERR("usbd_enable: %d", err); return -1; }

    LOG_INF("usb_03 strings_demo: enabled, idle on EP0");

    while (true) {
        k_sleep(K_FOREVER);
    }
    return 0;
}