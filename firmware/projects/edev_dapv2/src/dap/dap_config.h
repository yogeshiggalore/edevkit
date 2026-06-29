/*
 * dap_config.h — compile-time CMSIS-DAP v2 capability + size config.
 *
 * Reported via DAP_Info(0xF0/0xFF/0xFE) and constrains all other code.
 * Bumping these is a deliberate revision-bump moment — host tools cache
 * these values.
 */

#ifndef EDEV_DAPV2_DAP_CONFIG_H
#define EDEV_DAPV2_DAP_CONFIG_H

#include <stdint.h>

/* Capability byte 0 — DAP_Info(0xF0) returns this.
 *
 * Bits:
 *   0  SWD
 *   1  JTAG
 *   2  SWO UART
 *   3  SWO Manchester
 *   4  Atomic command (ExecuteCommands 0x7F)
 *   5  Test domain timer
 *   6  SWO streaming
 *   7  UART communication port (CMSIS-DAP v2.1)
 *
 * v0.1 advertises: SWD + JTAG + Atomic = 0x13.
 * Do NOT advertise SWO until it's actually implemented — host tools
 * silently half-state when caps lie (see project memory
 * `uh_dapv2_caps_byte`).
 */
#define DAP_CAP_SWD              (1u << 0)
#define DAP_CAP_JTAG             (1u << 1)
#define DAP_CAP_SWO_UART         (1u << 2)
#define DAP_CAP_SWO_MANCHESTER   (1u << 3)
#define DAP_CAP_ATOMIC           (1u << 4)
#define DAP_CAP_TEST_DOMAIN      (1u << 5)
#define DAP_CAP_SWO_STREAM       (1u << 6)
#define DAP_CAP_UART_PORT        (1u << 7)

#define DAP_CAPABILITIES_BYTE0   (DAP_CAP_SWD | DAP_CAP_JTAG | DAP_CAP_ATOMIC)
#define DAP_CAPABILITIES_BYTE1   0u   /* reserved */

/* Packet config — what the host should assume per DAP packet.
 *
 * Wire-side EP MPS is 64 (USB FS bulk max). A logical DAP packet up to
 * `DAP_PACKET_SIZE` bytes spans multiple bulk transfers; TinyUSB
 * delivers it to us as one xfer_cb completion.
 */
#define DAP_PACKET_SIZE          512u
#define DAP_PACKET_COUNT         4u

/* Firmware version string — DAP_Info(0x04). probe-rs 0.31+ requires
 * "2.2.0" or higher (see project memory `probe_rs_bcd_device_gate`). */
#define DAP_FW_VERSION_STR       "2.2.0"

/* Identification strings — DAP_Info(0x01..0x03). */
#define DAP_VENDOR_NAME_STR      "Edevkit"
#define DAP_PRODUCT_NAME_STR     "edev_dapv2 CMSIS-DAP"
/* iSerialNumber comes from chip_id_string() — set at runtime. */

/* USB IDs. */
#define DAP_USB_VID              0x2E8Au   /* Raspberry Pi Trading      */
#define DAP_USB_PID              0x000Cu   /* CMSIS-DAP — RP Debug Probe */
#define DAP_USB_BCD_DEVICE       0x0220u   /* clears probe-rs gate      */

#endif /* EDEV_DAPV2_DAP_CONFIG_H */
