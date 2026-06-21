/*
 * usb_descriptors.c — every USB descriptor edev_dapv2 emits.
 *
 * Layout for v0.1:
 *   - one configuration
 *   - one vendor-class interface (interface 0)
 *   - three bulk endpoints: DAP OUT, DAP IN, SWO IN
 *   - BOS descriptor pointing at a Microsoft OS 2.0 descriptor set so
 *     Windows binds WinUSB to the interface without a .inf install
 *
 * Constants live in usb_descriptors.h.
 */

#include <string.h>

#include "tusb.h"
#include "device/usbd_pvt.h"

#include "usb_descriptors.h"
#include "hw/chip_id.h"

/* ---------------------------------------------------------------------
 * Device descriptor
 * ------------------------------------------------------------------ */

/* bcdUSB = 0x0210 (USB 2.1) is mandatory if we want to ship a BOS
 * descriptor. Most older firmware uses 0x0200; we have to bump to 2.1
 * for the WinUSB MS-OS-2.0 chain to be queried at all. */
static const tusb_desc_device_t s_desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0210,
    .bDeviceClass       = 0x00,                 /* declared per interface */
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = EDEV_USB_VID,
    .idProduct          = EDEV_USB_PID,
    .bcdDevice          = EDEV_USB_BCD_DEVICE,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&s_desc_device;
}

/* ---------------------------------------------------------------------
 * Configuration + interface + endpoint descriptors
 * ------------------------------------------------------------------ */

enum {
    ITF_NUM_DAP = 0,
    ITF_NUM_TOTAL
};

/* String descriptor indices — keep in sync with s_string_desc_arr. */
enum {
    STRID_LANGID        = 0,
    STRID_MANUFACTURER  = 1,
    STRID_PRODUCT       = 2,
    STRID_SERIAL        = 3,
    STRID_DAP_INTERFACE = 4
};

#define LEN_CFG_HDR     9u
#define LEN_ITF         9u
#define LEN_EP_BULK     7u
#define LEN_DAP_BLOCK   (LEN_ITF + 3u * LEN_EP_BULK)
#define CONFIG_TOTAL    (LEN_CFG_HDR + LEN_DAP_BLOCK)

static const uint8_t s_desc_configuration[] = {
    /* Configuration header */
    TUD_CONFIG_DESCRIPTOR(
        /* config number  */ 1,
        /* itf count      */ ITF_NUM_TOTAL,
        /* string idx     */ 0,
        /* total length   */ CONFIG_TOTAL,
        /* attribute      */ 0x80,        /* bus-powered, no remote wakeup */
        /* max power (mA) */ 250          /* expressed in 2 mA units → 500 mA */
    ),

    /* ----- Interface 0: vendor-class CMSIS-DAP -----
     *
     * bInterfaceClass = 0xFF (Vendor), no sub-class/protocol. Three bulk
     * endpoints in declaration order: DAP OUT, DAP IN, SWO IN. The class
     * driver in usb_dap_class.c walks them in that order, so do not
     * rearrange without updating edev_dap_open(). */
    LEN_ITF, TUSB_DESC_INTERFACE,
    ITF_NUM_DAP,                /* bInterfaceNumber */
    0x00,                       /* bAlternateSetting */
    0x03,                       /* bNumEndpoints */
    TUSB_CLASS_VENDOR_SPECIFIC, /* bInterfaceClass = 0xFF */
    0x00,                       /* bInterfaceSubClass */
    0x00,                       /* bInterfaceProtocol */
    STRID_DAP_INTERFACE,        /* iInterface */

    /* DAP Bulk OUT  (host → device) */
    LEN_EP_BULK, TUSB_DESC_ENDPOINT, EDEV_DAP_OUT_EP_ADDR, TUSB_XFER_BULK,
    U16_TO_U8S_LE(EDEV_DAP_EP_MAX_PACKET), 0,

    /* DAP Bulk IN   (device → host) */
    LEN_EP_BULK, TUSB_DESC_ENDPOINT, EDEV_DAP_IN_EP_ADDR,  TUSB_XFER_BULK,
    U16_TO_U8S_LE(EDEV_DAP_EP_MAX_PACKET), 0,

    /* SWO Bulk IN   (device → host, streaming trace) */
    LEN_EP_BULK, TUSB_DESC_ENDPOINT, EDEV_SWO_IN_EP_ADDR,  TUSB_XFER_BULK,
    U16_TO_U8S_LE(EDEV_SWO_EP_MAX_PACKET), 0,
};

TU_VERIFY_STATIC(sizeof(s_desc_configuration) == CONFIG_TOTAL,
                 "Configuration descriptor length must match CONFIG_TOTAL");

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return s_desc_configuration;
}

/* ---------------------------------------------------------------------
 * String descriptors
 * ------------------------------------------------------------------ */

/* index 0 is the LangID array. For English (US) the two-byte LangID is
 * 0x0409 (little-endian → {0x09, 0x04}). The rest are UTF-8 source
 * strings; we widen to UTF-16 LE on the fly in the callback. */
static const char *s_string_desc_arr[] = {
    [STRID_LANGID]        = "\x09\x04",
    [STRID_MANUFACTURER]  = EDEV_USB_MANUFACTURER,
    [STRID_PRODUCT]       = EDEV_USB_PRODUCT,
    [STRID_SERIAL]        = NULL,   /* filled at runtime from chip_id */
    [STRID_DAP_INTERFACE] = EDEV_USB_DAP_INTERFACE,
};

static uint16_t s_desc_str_buf[32];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;

    if (index >= (sizeof(s_string_desc_arr) / sizeof(s_string_desc_arr[0]))) {
        return NULL;
    }

    uint8_t chr_count;

    if (index == STRID_LANGID) {
        /* The LangID array is raw bytes, not a Unicode string. Copy it
         * verbatim into the descriptor body. */
        memcpy(&s_desc_str_buf[1], s_string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        const char *str = s_string_desc_arr[index];
        if (str == NULL && index == STRID_SERIAL) {
            str = chip_id_serial();
        }
        if (str == NULL) {
            return NULL;
        }
        size_t len = strlen(str);
        if (len > 31u) {
            len = 31u;
        }
        chr_count = (uint8_t)len;
        for (uint8_t i = 0; i < chr_count; i++) {
            s_desc_str_buf[1 + i] = (uint16_t)str[i];
        }
    }

    /* Header: [type=STRING][total length in bytes] — total length
     * includes the 2-byte header itself plus 2 bytes per UTF-16 char. */
    s_desc_str_buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2u * chr_count + 2u));
    return s_desc_str_buf;
}

/* ---------------------------------------------------------------------
 * BOS descriptor + Microsoft OS 2.0 descriptor set
 *
 * The BOS chain points at a Microsoft OS 2.0 descriptor set; Windows
 * 8.1+ fetches it via a vendor-specific control transfer (handled by
 * tud_vendor_control_xfer_cb below) and uses it to bind the WinUSB
 * driver to interface 0 without a .inf install.
 *
 * Reference: "Microsoft OS 2.0 Descriptors Specification" (Microsoft,
 * 2014). The descriptor sets are byte-oriented blobs with strict
 * lengths — we encode them as static byte arrays and assert lengths.
 * ------------------------------------------------------------------ */

#define EDEV_MS_OS_20_VENDOR_CODE  0x01u   /* arbitrary; matches BOS descriptor */
#define EDEV_MS_OS_20_DESC_LEN     0xB2u

#define EDEV_BOS_TOTAL_LEN  (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

static const uint8_t s_desc_bos[] = {
    TUD_BOS_DESCRIPTOR(EDEV_BOS_TOTAL_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(EDEV_MS_OS_20_DESC_LEN, EDEV_MS_OS_20_VENDOR_CODE),
};

const uint8_t *tud_descriptor_bos_cb(void)
{
    return s_desc_bos;
}

/* The MS OS 2.0 descriptor set itself:
 *   - Set Header                            (10 bytes)
 *   - Configuration Subset Header           ( 8 bytes)
 *   - Function Subset Header (itf 0)        ( 8 bytes)
 *   - Compatible ID Feature (WINUSB)        (20 bytes)
 *   - Registry Property: DeviceInterfaceGUIDs (132 bytes)
 *
 * Total = 178 bytes = EDEV_MS_OS_20_DESC_LEN.
 *
 * The GUID below is the same one yapicoprobe uses for its CMSIS-DAP
 * interface. Windows uses it as the device-interface class key; the
 * actual value is arbitrary as long as it's stable per-product-line.
 */
static const uint8_t s_desc_ms_os_20[] = {
    /* --- Set header --- */
    U16_TO_U8S_LE(0x000A),
    U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(0x06030000),                  /* Windows 8.1 */
    U16_TO_U8S_LE(EDEV_MS_OS_20_DESC_LEN),

    /* --- Configuration subset header --- */
    U16_TO_U8S_LE(0x0008),
    U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),
    0,                                          /* config index = 0 (first) */
    0,                                          /* reserved */
    U16_TO_U8S_LE(EDEV_MS_OS_20_DESC_LEN - 0x0A),

    /* --- Function subset header --- */
    U16_TO_U8S_LE(0x0008),
    U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION),
    ITF_NUM_DAP,
    0,
    U16_TO_U8S_LE(EDEV_MS_OS_20_DESC_LEN - 0x0A - 0x08),

    /* --- Compatible ID feature: binds WinUSB driver --- */
    U16_TO_U8S_LE(0x0014),
    U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
    'W', 'I', 'N', 'U', 'S', 'B',
    0, 0,                                       /* CompatibleID padding */
    0, 0, 0, 0, 0, 0, 0, 0,                     /* SubCompatibleID = none */

    /* --- Registry Property: DeviceInterfaceGUIDs (REG_MULTI_SZ) --- */
    U16_TO_U8S_LE(EDEV_MS_OS_20_DESC_LEN - 0x0A - 0x08 - 0x08 - 0x14),
    U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007),                      /* REG_MULTI_SZ */
    U16_TO_U8S_LE(0x002A),                      /* name length, UTF-16, with NUL */
    'D',0, 'e',0, 'v',0, 'i',0, 'c',0, 'e',0, 'I',0, 'n',0, 't',0, 'e',0,
    'r',0, 'f',0, 'a',0, 'c',0, 'e',0, 'G',0, 'U',0, 'I',0, 'D',0, 's',0,
    0, 0,                                       /* NUL terminator */
    U16_TO_U8S_LE(0x0050),                      /* property length, UTF-16, with NULs */
    '{',0, 'C',0, 'D',0, 'B',0, '3',0, 'B',0, '5',0, 'A',0, 'D',0, '-',0,
    '2',0, '9',0, '3',0, 'B',0, '-',0, '4',0, '6',0, '6',0, '3',0, '-',0,
    'A',0, 'A',0, '3',0, '6',0, '-',0, '1',0, 'A',0, 'A',0, 'E',0, '4',0,
    '6',0, '4',0, '6',0, '3',0, '7',0, '7',0, '6',0, '}',0,
    0, 0, 0, 0,                                 /* MULTI_SZ double-NUL */
};

TU_VERIFY_STATIC(sizeof(s_desc_ms_os_20) == EDEV_MS_OS_20_DESC_LEN,
                 "MS OS 2.0 descriptor length mismatch");

/* Vendor control-transfer handler. Windows fetches the MS OS 2.0 set
 * via a vendor request with bRequest = EDEV_MS_OS_20_VENDOR_CODE and
 * wIndex = 7 (MS_OS_20_DESCRIPTOR_INDEX). */
bool tud_vendor_control_xfer_cb(uint8_t                       rhport,
                                uint8_t                       stage,
                                const tusb_control_request_t *req)
{
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }

    if (req->bmRequestType_bit.type    == TUSB_REQ_TYPE_VENDOR &&
        req->bRequest                  == EDEV_MS_OS_20_VENDOR_CODE &&
        req->wIndex                    == 7u) {
        /* wLength may be smaller than the full set on the first probe
         * (Windows asks for 10 bytes of the header first, then comes
         * back for the full length). tud_control_xfer honours wLength. */
        uint16_t total_len;
        memcpy(&total_len, s_desc_ms_os_20 + 8, sizeof(total_len));
        return tud_control_xfer(rhport, req, (void *)s_desc_ms_os_20, total_len);
    }
    return false;
}
