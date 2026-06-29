/*
 * tusb_config.h — TinyUSB compile-time configuration for edev_dapv2.
 *
 * We use TinyUSB for descriptor handling, EP0 control transfers, and
 * bulk-endpoint plumbing only. The CMSIS-DAP vendor class is served by
 * a custom class driver (src/usb/usb_dap_class.c) registered via
 * `usbd_app_driver_get_cb()` — NOT by TinyUSB's stock CFG_TUD_VENDOR
 * (whose FIFO model hides DAP packet boundaries).
 */

#ifndef EDEV_DAPV2_TUSB_CONFIG_H
#define EDEV_DAPV2_TUSB_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* Common */
#define CFG_TUSB_MCU                 OPT_MCU_RP2040
#define CFG_TUSB_OS                  OPT_OS_PICO
#define CFG_TUSB_DEBUG               0

/* RHPORT 0 in device mode at full-speed. Required by older
 * tusb_init() macros even when CFG_TUD_ENABLED is set. */
#define CFG_TUSB_RHPORT0_MODE        (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

/* USB DMA on RP2350 is happiest at 4-byte alignment. */
#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN           __attribute__ ((aligned(4)))
#endif

/* Device mode only — we're a USB device. */
#define CFG_TUD_ENABLED              1
#define CFG_TUD_MAX_SPEED            OPT_MODE_FULL_SPEED

/* EP0 max packet size. */
#define CFG_TUD_ENDPOINT0_SIZE       64

/* Stock class drivers — CDC is enabled as a debug/log channel so we
 * can see what's happening inside the DAP dispatcher and SWD layer
 * from a host terminal (`screen /dev/tty.usbmodem*`). All other stock
 * classes are off; the DAP interface uses our custom class driver. */
#define CFG_TUD_CDC                  1
#define CFG_TUD_CDC_RX_BUFSIZE       64
#define CFG_TUD_CDC_TX_BUFSIZE       512
#define CFG_TUD_MSC                  0
#define CFG_TUD_HID                  0
#define CFG_TUD_MIDI                 0
#define CFG_TUD_AUDIO                0
#define CFG_TUD_VIDEO                0
#define CFG_TUD_VENDOR               0
#define CFG_TUD_USBTMC               0
#define CFG_TUD_DFU                  0
#define CFG_TUD_DFU_RUNTIME          0
#define CFG_TUD_BTH                  0
#define CFG_TUD_ECM_RNDIS            0
#define CFG_TUD_NCM                  0

#ifdef __cplusplus
}
#endif

#endif /* EDEV_DAPV2_TUSB_CONFIG_H */
