/*
 * usb_descriptors.h — VID/PID/EP/string constants shared between the
 * descriptor blob and the class driver.
 *
 * The values that have a "must be X because Y" constraint live here so
 * there's one source of truth. Anything that's purely internal stays
 * static in usb_descriptors.c.
 */

#ifndef EDEV_DAPV2_USB_DESCRIPTORS_H
#define EDEV_DAPV2_USB_DESCRIPTORS_H

#include <stdint.h>

/* ----- Identity ------------------------------------------------------ */

/* Raspberry Pi Trading "Debug Probe" identity. Every CMSIS-DAP host
 * whitelists this pair; piggy-backing means zero host-side config. */
#define EDEV_USB_VID              0x2E8Au
#define EDEV_USB_PID              0x000Cu

/* bcdDevice ≥ 0x0220 clears probe-rs 0.31+'s "DAP firmware version
 * gate" — see project memory `reference_probe_rs_bcd_device_gate`. The
 * value is interpreted as the host's idea of our DAP version. */
#define EDEV_USB_BCD_DEVICE       0x0220u

#define EDEV_USB_MANUFACTURER     "Edevkit"
#define EDEV_USB_PRODUCT          "edev_dapv2 CMSIS-DAP"
/* iInterface MUST contain the literal "CMSIS-DAP" — host-side
 * auto-detection (pyocd, openocd, probe-rs) scans for that substring as
 * a fallback when VID/PID isn't on a whitelist. */
#define EDEV_USB_DAP_INTERFACE    "edev_dapv2 CMSIS-DAP v2 Interface"

/* ----- Endpoints ----------------------------------------------------- */

#define EDEV_DAP_OUT_EP_ADDR      0x01u   /* host → device, DAP commands */
#define EDEV_DAP_IN_EP_ADDR       0x81u   /* device → host, DAP responses */
#define EDEV_SWO_IN_EP_ADDR       0x82u   /* device → host, SWO stream */

#define EDEV_DAP_EP_MAX_PACKET    64u     /* USB FS bulk wire-level max */
#define EDEV_SWO_EP_MAX_PACKET    64u

/* ----- DAP packet logical sizing -------------------------------------
 *
 * The wire-level USB FS bulk maximum is 64 bytes per packet (set by
 * EDEV_DAP_EP_MAX_PACKET above), but a logical DAP packet can be larger
 * — the host spans it across multiple bulk transfers. Advertising 512
 * via DAP_Info(0xFF) unlocks bulk pipelining on every host we tested.
 *
 * EDEV_DAP_PACKET_COUNT is how many in-flight packets the host may
 * pipeline at us; we keep one ring slot per pending packet, so picking
 * a power of two avoids modulo arithmetic in the ring index.
 */
#define EDEV_DAP_PACKET_SIZE      512u
#define EDEV_DAP_PACKET_COUNT     4u

/* ----- TinyUSB callback prototypes ----------------------------------- */

/* Provided by usb_descriptors.c, invoked by TinyUSB. */
const uint8_t  *tud_descriptor_device_cb       (void);
const uint8_t  *tud_descriptor_configuration_cb(uint8_t index);
const uint16_t *tud_descriptor_string_cb       (uint8_t index, uint16_t langid);
const uint8_t  *tud_descriptor_bos_cb          (void);

#endif /* EDEV_DAPV2_USB_DESCRIPTORS_H */
