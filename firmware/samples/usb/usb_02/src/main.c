#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(usb_02, LOG_LEVEL_INF);

#define DESC_CLASS_NAME "descriptors_demo"

USBD_DESC_LANG_DEFINE(lang);
USBD_DESC_MANUFACTURER_DEFINE(mfr,     "edevkit");
USBD_DESC_PRODUCT_DEFINE     (product, "usb_02");           /* changed from usb_01 */
USBD_DESC_SERIAL_NUMBER_DEFINE(serial); 

/*
 * Configuration descriptor:
 *   bmAttributes = 0xA0  (bus-powered + remote-wakeup)
 *   bMaxPower    = 50    (in 2-mA units → 100 mA)
 *   iConfig      = NULL  (no per-config string)
 *
 * The wTotalLength field is filled in by the stack at usbd_init() time
 * after walking every registered class's fs_desc[] for this configuration.
 */
USBD_CONFIGURATION_DEFINE(cfg, USB_SCD_REMOTE_WAKEUP, 50, NULL);

/*
 * Device-level identity.
 *   VID 0x1209 — pid.codes umbrella
 *   PID 0xede2 — sample 2 of the edevkit USB Book series  (changed from usb_01)
 *   The board DTS exposes the UDC node as &usbd; do not guess "usb_dev0".
 */
USBD_DEVICE_DEFINE(usbd_ctx, DEVICE_DT_GET(DT_NODELABEL(usbd)), 0x1209, 0xede2);

int main(void)
{
    int err;

    /* (1)–(4) Add string descriptors. Order matters — index slots in the
     * device descriptor are assigned in the order we add them.            */
    err = usbd_add_descriptor(&usbd_ctx, &lang);
    if (err) { LOG_ERR("add lang: %d", err);    return -1; }
    err = usbd_add_descriptor(&usbd_ctx, &mfr);
    if (err) { LOG_ERR("add mfr: %d", err);     return -1; }
    err = usbd_add_descriptor(&usbd_ctx, &product);
    if (err) { LOG_ERR("add product: %d", err); return -1; }
    err = usbd_add_descriptor(&usbd_ctx, &serial);
    if (err) { LOG_ERR("add serial: %d", err);  return -1; }

    /* (5) Add the configuration. We're FS-only on RP2350. */
    err = usbd_add_configuration(&usbd_ctx, USBD_SPEED_FS, &cfg);
    if (err) { LOG_ERR("add cfg: %d", err); return -1; }

    /* (6) Attach the descriptors_demo class instance to configuration 1.
     *     Class-name string MUST match USBD_DEFINE_CLASS in the class file. */
    err = usbd_register_class(&usbd_ctx, DESC_CLASS_NAME,
                              USBD_SPEED_FS, 1 /* config index */);
    if (err) { LOG_ERR("register class: %d", err); return -1; }

    /* (7) Validate + finalize. The stack walks every class's fs_desc[],
     *     computes wTotalLength, and writes bInterfaceNumber +
     *     bEndpointAddress back into our (non-const!) descriptor structs. */
    err = usbd_init(&usbd_ctx);
    if (err) { LOG_ERR("usbd_init: %d", err); return -1; }

    /* (8) Connect the pull-up. From this moment the host can see us. */
    err = usbd_enable(&usbd_ctx);
    if (err) { LOG_ERR("usbd_enable: %d", err); return -1; }

    LOG_INF("usb_02 descriptors_demo: enabled, idle on EP0");

    /* (9) Stack runs in its own work queue. Nothing for main thread to do. */
    while (true) {
        k_sleep(K_FOREVER);
    }
    return 0;
}
