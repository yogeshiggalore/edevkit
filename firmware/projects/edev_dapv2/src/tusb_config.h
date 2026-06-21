/*
 * tusb_config.h — TinyUSB compile-time configuration for edev_dapv2.
 *
 * The CMSIS-DAP v2 interface uses a CUSTOM class driver (see
 * usb_dap_class.c), NOT TinyUSB's stock vendor class — the stock class
 * has internal RX/TX FIFOs that hide USB packet boundaries, and a
 * request/response protocol like CMSIS-DAP needs "one OUT URB = one DAP
 * packet" to hold cleanly. So CFG_TUD_VENDOR stays 0 here; the custom
 * driver is wired in via usbd_app_driver_get_cb() from usb_dap_class.c.
 */

#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined (pico-sdk normally provides this)
#endif

#define CFG_TUSB_RHPORT0_MODE       OPT_MODE_DEVICE

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS                 OPT_OS_PICO
#endif

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN          __attribute__((aligned(4)))
#endif

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE      64
#endif

/* v0.1: only the DAP interface, via custom class driver. No CDC, no MSC,
 * no HID, no stock vendor (we provide our own). */
#define CFG_TUD_HID                 0
#define CFG_TUD_CDC                 0
#define CFG_TUD_MSC                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              0

#ifdef __cplusplus
}
#endif

#endif /* TUSB_CONFIG_H_ */
