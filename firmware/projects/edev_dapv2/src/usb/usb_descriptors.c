/*
 * usb_descriptors.c — USB descriptors for edev_dapv2.
 *
 * Surfaces:
 *   - Device descriptor             tud_descriptor_device_cb()
 *   - Configuration descriptor      tud_descriptor_configuration_cb()
 *   - String descriptors            tud_descriptor_string_cb()
 *   - BOS chain (MS OS 2.0)         tud_descriptor_bos_cb()
 *
 * Topology (v0.1):
 *   Configuration 1
 *   └── Interface 0  Vendor (FF/00/00)  "edev_dapv2 CMSIS-DAP v2 …"
 *       ├── EP 0x01 OUT bulk 64B
 *       └── EP 0x81 IN  bulk 64B
 *
 * The interface string MUST contain literal "CMSIS-DAP" — every host
 * (probe-rs, pyocd, OpenOCD) substring-scans for it as a fallback
 * when VID/PID isn't on its whitelist.
 *
 * The BOS chain advertises MS OS 2.0 → Compatible ID "WINUSB" so
 * Windows binds WinUSB without an .inf install. The actual
 * MS OS 2.0 descriptor set is returned by the custom class driver's
 * control_xfer_cb on a device-level vendor IN request with
 * wIndex = MS_OS_20_DESCRIPTOR_INDEX.
 */

#include "usb/usb_descriptors.h"

#include "dap/dap_config.h"
#include "hw/chip_id.h"

#include "tusb.h"

#include <string.h>

/* ----------------------------------------------------------------------
 * Device descriptor
 * ---------------------------------------------------------------------- */

static const tusb_desc_device_t s_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0210,            /* USB 2.1 — required for BOS */
    /* Multi-class device with IAD — required for vendor + CDC combo on
     * Windows. macOS / Linux don't care about IAD as much but it
     * doesn't hurt them. */
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = DAP_USB_VID,
    .idProduct          = DAP_USB_PID,
    .bcdDevice          = DAP_USB_BCD_DEVICE,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *) &s_device_desc;
}

/* ----------------------------------------------------------------------
 * Configuration descriptor — 1 vendor interface with 2 bulk EPs
 * ---------------------------------------------------------------------- */

#define EDEV_STR_IDX_INTERFACE_DAP 4u
#define EDEV_STR_IDX_INTERFACE_CDC 5u

/* Length breakdown:
 *   Configuration desc   9
 *   Interface 0 (DAP)    9 + 7 + 7 = 23
 *   IAD for CDC          8
 *   Interface 1 (CDC ctl) 9 + 5 + 5 + 4 + 5 + 7 = 35
 *   Interface 2 (CDC dat) 9 + 7 + 7 = 23
 *   Total                                          98
 */
#define EDEV_CONFIG_TOTAL_LEN  (9u + 23u + 8u + 35u + 23u)

static const uint8_t s_config_desc[] = {
    /* Configuration descriptor (9 bytes) */
    9,
    TUSB_DESC_CONFIGURATION,
    U16_TO_U8S_LE(EDEV_CONFIG_TOTAL_LEN),
    3,                              /* bNumInterfaces */
    1,                              /* bConfigurationValue */
    0,                              /* iConfiguration */
    TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP,
    100,                            /* bMaxPower (200 mA) */

    /* === Interface 0: DAP (vendor-specific) ============================ */
    9,
    TUSB_DESC_INTERFACE,
    EDEV_DAP_ITF_NUM,               /* bInterfaceNumber */
    0,                              /* bAlternateSetting */
    2,                              /* bNumEndpoints */
    TUSB_CLASS_VENDOR_SPECIFIC,
    0x00,
    0x00,
    EDEV_STR_IDX_INTERFACE_DAP,

    /* DAP OUT EP */
    7, TUSB_DESC_ENDPOINT, EDEV_DAP_EP_OUT, TUSB_XFER_BULK,
    U16_TO_U8S_LE(64), 0,

    /* DAP IN EP */
    7, TUSB_DESC_ENDPOINT, EDEV_DAP_EP_IN, TUSB_XFER_BULK,
    U16_TO_U8S_LE(64), 0,

    /* === IAD: groups interfaces 1+2 as a single CDC function =========== */
    8,
    TUSB_DESC_INTERFACE_ASSOCIATION,
    EDEV_CDC_ITF_NUM,               /* bFirstInterface */
    2,                              /* bInterfaceCount */
    TUSB_CLASS_CDC,                 /* bFunctionClass */
    CDC_COMM_SUBCLASS_ABSTRACT_CONTROL_MODEL,
    CDC_COMM_PROTOCOL_NONE,
    EDEV_STR_IDX_INTERFACE_CDC,

    /* === Interface 1: CDC Communications (Control) ===================== */
    9,
    TUSB_DESC_INTERFACE,
    EDEV_CDC_ITF_NUM,               /* bInterfaceNumber = 1 */
    0,
    1,                              /* bNumEndpoints (notification) */
    TUSB_CLASS_CDC,
    CDC_COMM_SUBCLASS_ABSTRACT_CONTROL_MODEL,
    CDC_COMM_PROTOCOL_NONE,
    EDEV_STR_IDX_INTERFACE_CDC,

    /* CDC Header functional descriptor (5 bytes) */
    5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_HEADER, U16_TO_U8S_LE(0x0120),

    /* CDC Call Management functional descriptor (5 bytes) */
    5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_CALL_MANAGEMENT, 0x00,
    EDEV_CDC_DATA_ITF_NUM,

    /* CDC ACM functional descriptor (4 bytes) */
    4, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_ABSTRACT_CONTROL_MANAGEMENT, 0x02,

    /* CDC Union functional descriptor (5 bytes) */
    5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_UNION,
    EDEV_CDC_ITF_NUM,               /* bControlInterface */
    EDEV_CDC_DATA_ITF_NUM,          /* bSubordinateInterface0 */

    /* CDC notification EP — interrupt, 8 B MPS, every 16 ms */
    7, TUSB_DESC_ENDPOINT, EDEV_CDC_EP_NOTIF, TUSB_XFER_INTERRUPT,
    U16_TO_U8S_LE(8), 16,

    /* === Interface 2: CDC Data ========================================= */
    9,
    TUSB_DESC_INTERFACE,
    EDEV_CDC_DATA_ITF_NUM,
    0,
    2,
    TUSB_CLASS_CDC_DATA,
    0x00,
    0x00,
    0x00,

    /* CDC data OUT EP */
    7, TUSB_DESC_ENDPOINT, EDEV_CDC_EP_OUT, TUSB_XFER_BULK,
    U16_TO_U8S_LE(64), 0,

    /* CDC data IN EP */
    7, TUSB_DESC_ENDPOINT, EDEV_CDC_EP_IN, TUSB_XFER_BULK,
    U16_TO_U8S_LE(64), 0,
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void) index;
    return s_config_desc;
}

/* ----------------------------------------------------------------------
 * String descriptors
 * ---------------------------------------------------------------------- */

static const char *const s_string_table[] = {
    [0] = (const char *) "\x09\x04",          /* LANGID — English (US) — handled specially below */
    [1] = "Edevkit",                          /* iManufacturer */
    [2] = "edev_dapv2 CMSIS-DAP",             /* iProduct       */
    [3] = NULL,                               /* iSerialNumber — runtime */
    [4] = "edev_dapv2 CMSIS-DAP v2 Interface",/* iInterface DAP — must contain "CMSIS-DAP" */
    [5] = "edev_dapv2 Debug Log",             /* iInterface CDC */
};

#define EDEV_STR_TABLE_LEN  (sizeof(s_string_table) / sizeof(s_string_table[0]))

/* Buffer for converting C strings to UTF-16LE descriptors on demand. */
static uint16_t s_str_buf[32];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void) langid;

    if (index >= EDEV_STR_TABLE_LEN) return NULL;

    /* Index 0 — language ID descriptor. */
    if (index == 0) {
        s_str_buf[0] = (TUSB_DESC_STRING << 8) | 4;
        s_str_buf[1] = 0x0409;        /* English (US) */
        return s_str_buf;
    }

    /* Index 3 — serial number, sourced at runtime from chip_id. */
    const char *src;
    if (index == 3) {
        src = chip_id_string();
    } else {
        src = s_string_table[index];
    }
    if (src == NULL) return NULL;

    size_t chrlen = strlen(src);
    if (chrlen > (sizeof(s_str_buf) / sizeof(s_str_buf[0])) - 1) {
        chrlen = (sizeof(s_str_buf) / sizeof(s_str_buf[0])) - 1;
    }
    for (size_t i = 0; i < chrlen; ++i) {
        s_str_buf[i + 1] = (uint16_t) src[i];
    }
    s_str_buf[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2u * chrlen + 2u));
    return s_str_buf;
}

/* ----------------------------------------------------------------------
 * BOS chain + MS OS 2.0 descriptor set
 * ---------------------------------------------------------------------- */

#define MS_OS_20_VENDOR_REQ_CODE   0x01u   /* see usb_dap_class.c */

#define MS_OS_20_DESC_LEN                                                     \
    ( /* Set header             */ 10u                                        \
    + /* Compatible ID feature  */ 20u                                        \
    + /* Registry property      */ 132u )

const uint16_t edev_ms_os_20_desc_len = MS_OS_20_DESC_LEN;

#define BOS_TOTAL_LEN  (5u + 28u)

static const uint8_t s_bos_desc[] = {
    /* BOS header (5 bytes) */
    5,                              /* bLength */
    TUSB_DESC_BOS,                  /* bDescriptorType = 0x0F */
    U16_TO_U8S_LE(BOS_TOTAL_LEN),   /* wTotalLength */
    1,                              /* bNumDeviceCaps */

    /* Platform Device Capability — MS OS 2.0 (28 bytes) */
    28,                             /* bLength */
    TUSB_DESC_DEVICE_CAPABILITY,    /* bDescriptorType = 0x10 */
    DEVICE_CAPABILITY_PLATFORM,     /* bDevCapabilityType = 0x05 */
    0x00,                           /* bReserved */

    /* PlatformCapabilityUUID — MS OS 2.0 GUID:
     *   D8DD60DF-4589-4CC7-9CD2-659D9E648A9F
     * Encoded little-endian per RFC 4122. */
    0xDF, 0x60, 0xDD, 0xD8,
    0x89, 0x45,
    0xC7, 0x4C,
    0x9C, 0xD2,
    0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F,

    /* dwWindowsVersion — minimum Windows 8.1 (NTDDI_WINBLUE = 0x06030000) */
    U32_TO_U8S_LE(0x06030000),

    /* wMSOSDescriptorSetTotalLength */
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN),

    MS_OS_20_VENDOR_REQ_CODE,       /* bMS_VendorCode */
    0,                              /* bAltEnumCode */
};

const uint8_t *tud_descriptor_bos_cb(void)
{
    return s_bos_desc;
}

/* MS OS 2.0 descriptor set itself — returned via a vendor request to
 * the device on bRequest=MS_OS_20_VENDOR_REQ_CODE, wIndex=7.
 *
 * Layout (178 bytes):
 *   Set header (10) + Compatible ID (20) + Registry property (132)
 *
 * Skipped: Configuration Subset Header and Function Subset Header.
 * The Compatible ID + property apply to the whole device — Windows
 * still binds WinUSB to interface 0 because the device only has one
 * interface.
 */

/* DeviceInterfaceGUIDs — same value DAPLink uses, so host tools that
 * cache by GUID stay happy. */
#define DEV_IFACE_GUID  u"{CDB3B5AD-293B-4663-AA36-1AAE46463776}\0"
#define DEV_IFACE_GUID_BYTES (sizeof(DEV_IFACE_GUID))   /* includes trailing NUL pair */

#define PROPERTY_NAME   u"DeviceInterfaceGUIDs\0"
#define PROPERTY_NAME_BYTES (sizeof(PROPERTY_NAME))

const uint8_t edev_ms_os_20_desc[MS_OS_20_DESC_LEN] = {
    /* MS OS 2.0 set header (10 bytes) */
    U16_TO_U8S_LE(10),                          /* wLength */
    U16_TO_U8S_LE(0x0000),                      /* wDescriptorType = SET_HEADER */
    U32_TO_U8S_LE(0x06030000),                  /* dwWindowsVersion = 8.1 */
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN),           /* wTotalLength */

    /* Compatible ID feature descriptor (20 bytes) */
    U16_TO_U8S_LE(20),                          /* wLength */
    U16_TO_U8S_LE(0x0003),                      /* wDescriptorType = COMPATIBLE_ID */
    'W', 'I', 'N', 'U', 'S', 'B', 0, 0,         /* CompatibleID[8] */
    0, 0, 0, 0, 0, 0, 0, 0,                     /* SubCompatibleID[8] */

    /* Registry property descriptor (132 bytes total) */
    U16_TO_U8S_LE(132),                         /* wLength */
    U16_TO_U8S_LE(0x0004),                      /* wDescriptorType = REG_PROPERTY */
    U16_TO_U8S_LE(0x0007),                      /* wPropertyDataType = REG_MULTI_SZ */
    U16_TO_U8S_LE(42),                          /* wPropertyNameLength = "DeviceInterfaceGUIDs\0" */
    'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0,
    'I', 0, 'n', 0, 't', 0, 'e', 0, 'r', 0, 'f', 0,
    'a', 0, 'c', 0, 'e', 0, 'G', 0, 'U', 0, 'I', 0,
    'D', 0, 's', 0, 0, 0,
    U16_TO_U8S_LE(80),                          /* wPropertyDataLength = GUID + NUL + extra NUL */
    '{', 0, 'C', 0, 'D', 0, 'B', 0, '3', 0, 'B', 0,
    '5', 0, 'A', 0, 'D', 0, '-', 0, '2', 0, '9', 0,
    '3', 0, 'B', 0, '-', 0, '4', 0, '6', 0, '6', 0,
    '3', 0, '-', 0, 'A', 0, 'A', 0, '3', 0, '6', 0,
    '-', 0, '1', 0, 'A', 0, 'A', 0, 'E', 0, '4', 0,
    '6', 0, '4', 0, '6', 0, '3', 0, '7', 0, '7', 0,
    '6', 0, '}', 0, 0, 0, 0, 0,
};
