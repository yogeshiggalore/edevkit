/*
 * dap_config.h — CMSIS-DAP v2 command IDs, capability bits, and tunables.
 *
 * Values are byte-level constants from the CMSIS-DAP v2 spec (ARM
 * doc). Where a value is implementation-defined (e.g. capability bits,
 * packet size/count), the choice is justified in a comment.
 */

#ifndef EDEV_DAPV2_DAP_CONFIG_H
#define EDEV_DAPV2_DAP_CONFIG_H

#include <stdint.h>

#include "usb_descriptors.h"   /* for EDEV_DAP_PACKET_SIZE / _COUNT */

/* ----- Command IDs --------------------------------------------------- */

#define DAP_CMD_INFO                0x00u
#define DAP_CMD_HOST_STATUS         0x01u
#define DAP_CMD_CONNECT             0x02u
#define DAP_CMD_DISCONNECT          0x03u
#define DAP_CMD_TRANSFER_CONFIGURE  0x04u
#define DAP_CMD_TRANSFER            0x05u
#define DAP_CMD_TRANSFER_BLOCK      0x06u
#define DAP_CMD_TRANSFER_ABORT      0x07u
#define DAP_CMD_WRITE_ABORT         0x08u
#define DAP_CMD_DELAY               0x09u
#define DAP_CMD_RESET_TARGET        0x0Au

#define DAP_CMD_SWJ_PINS            0x10u
#define DAP_CMD_SWJ_CLOCK           0x11u
#define DAP_CMD_SWJ_SEQUENCE        0x12u

#define DAP_CMD_SWD_CONFIGURE       0x13u
#define DAP_CMD_SWD_SEQUENCE        0x1Du

#define DAP_CMD_JTAG_SEQUENCE       0x14u
#define DAP_CMD_JTAG_CONFIGURE      0x15u
#define DAP_CMD_JTAG_IDCODE         0x16u

#define DAP_CMD_SWO_TRANSPORT       0x17u
#define DAP_CMD_SWO_MODE            0x18u
#define DAP_CMD_SWO_BAUDRATE        0x19u
#define DAP_CMD_SWO_CONTROL         0x1Au
#define DAP_CMD_SWO_STATUS          0x1Bu
#define DAP_CMD_SWO_DATA            0x1Cu
#define DAP_CMD_SWO_EXTENDED_STATUS 0x1Eu

#define DAP_CMD_QUEUE_COMMANDS      0x7Eu  /* rewritten to 0x7F in usb_dap_class.c */
#define DAP_CMD_EXECUTE_COMMANDS    0x7Fu

/* ----- 0x80..0x9F Vendor (edev_dapv2-specific) -------------------------
 *
 * Mapping intent: 1-to-1 with `nrfjprog` reset/recover surface, so a
 * host CLI like `edevocd nrf-reset` can route directly to a single
 * vendor command without round-tripping through pyocd's full DP/AP
 * state machine. Vendor commands run the canonical SWD sequence
 * on-probe and return a status byte.
 *
 *   0x83 EDEV_NRF_SYS_RESET   ⇔ `nrfjprog --reset`      (AIRCR.SYSRESETREQ)
 *   0x84 EDEV_NRF_DEBUG_RESET ⇔ `nrfjprog --debugreset` (CTRL-AP RESET)     — future
 *   0x85 EDEV_NRF_RECOVER     ⇔ `nrfjprog --recover`    (CTRL-AP ERASEALL) — future
 */
#define DAP_CMD_EDEV_NRF_SYS_RESET    0x83u
#define DAP_CMD_EDEV_MEM_READ         0x86u   /* AHB-AP block read  — [addr:u32, count:u8] → [status, count, data×count] */
#define DAP_CMD_EDEV_MEM_WRITE        0x87u   /* AHB-AP block write — [addr:u32, count:u8, data×count] → [status, count] */
#define DAP_CMD_EDEV_AP_READ          0x88u   /* Raw AP reg read    — [apsel:u8, apreg:u8] → [status, value:u32] */
#define DAP_CMD_EDEV_AP_WRITE         0x89u   /* Raw AP reg write   — [apsel:u8, apreg:u8, value:u32] → [status] */
#define DAP_CMD_EDEV_CORTEX_M_DUMP    0x8Au   /* All-in-one Cortex-M info dump in a single firmware call.
                                             * Request:  [do_reset_halt:u8, ap_count:u8 (max 8)]
                                             * Response: [status, dpidr:u32, ahb_ap_sel:u8,
                                             *            for each AP slot: [st:u8, idr:u32, base:u32],
                                             *            scb_status:u8, scb_words:u8, scb:u32×64] */

/* ----- DAP_Info (0x00) subcommands ---------------------------------- */

#define DAP_INFO_VENDOR_NAME        0x01u
#define DAP_INFO_PRODUCT_NAME       0x02u
#define DAP_INFO_SERIAL_NUMBER      0x03u
#define DAP_INFO_PROTOCOL_VERSION   0x04u
#define DAP_INFO_TARGET_VENDOR      0x05u
#define DAP_INFO_TARGET_NAME        0x06u
#define DAP_INFO_TARGET_BOARD_VENDOR 0x07u
#define DAP_INFO_TARGET_BOARD_NAME  0x08u
#define DAP_INFO_PRODUCT_FW_VERSION 0x09u
#define DAP_INFO_CAPABILITIES       0xF0u
#define DAP_INFO_TD_TIMER_FREQ      0xF1u
#define DAP_INFO_UART_RX_BUF_SIZE   0xFBu
#define DAP_INFO_UART_TX_BUF_SIZE   0xFCu
#define DAP_INFO_SWO_BUF_SIZE       0xFDu
#define DAP_INFO_PACKET_COUNT       0xFEu
#define DAP_INFO_PACKET_SIZE        0xFFu

/* ----- DAP_Info(0xF0) capability bits --------------------------------
 *
 * Only advertise what the firmware actually delivers. OpenOCD's
 * `tpiu config` (cmsis_dap.c:1490–1555) gates SWO bring-up on bits 2/3,
 * then immediately queries DAP_Info 0xFD (SWO_Buf_Size). If we set bit 2
 * but return buf_size=0 (as we do until M7), OpenOCD enters a half-state:
 * it issues SWO_TRANSPORT/MODE/BAUDRATE/CONTROL(START) and then polls
 * DAP_SWO_Data forever, producing an empty trace pane and a "why doesn't
 * SWO work?" bug report. Capability bits 2 and 6 will be re-enabled in
 * M7 along with the SWO ring buffer.
 *
 * Bit 0 SWD          — yes
 * Bit 1 JTAG         — yes
 * Bit 2 SWO UART     — no  (M7 — re-enable with the ring buffer)
 * Bit 3 SWO Manch.   — no
 * Bit 4 Atomic Cmds  — yes (DAP_ExecuteCommands)
 * Bit 5 TD Timer     — no
 * Bit 6 SWO Streamed — no  (M7 — re-enable with the ring buffer)
 * Bit 7 UART Comm.   — no  (no CDC in v0.1)
 */
#define EDEV_DAP_CAPABILITIES_BYTE0   ( (1u << 0) /* SWD              */ \
                                    | (1u << 1) /* JTAG             */ \
                                    | (1u << 4) /* Atomic commands  */ )

/* ----- Standard transfer-response ACK encodings ----------------------
 *
 * CMSIS-DAP v2 Transfer Response byte layout:
 *   bits 0..2 = wire ACK   — MUST be one of {OK, WAIT, FAULT, NO_ACK}
 *   bit  3    = ProtocolError (parity / framing) — OR-combined with ACK
 *   bit  4    = ValueMismatch (match-read failed)
 *
 * pyocd/probe-rs validate the wire-ACK field against {1,2,4,7}. Setting
 * bit 3 alone (= 0x08) leaves bits 0:2 = 0, which they reject as
 * "Unexpected ACK '0'". OpenOCD is lenient and only checks for OK,
 * which is why edev_dapv2 v0.1 looked fine against OpenOCD.
 */

#define DAP_TRANSFER_OK     0x01u
#define DAP_TRANSFER_WAIT   0x02u
#define DAP_TRANSFER_FAULT  0x04u
#define DAP_TRANSFER_NO_ACK 0x07u   /* wire all-ones — target didn't drive */
#define DAP_TRANSFER_ERROR  0x08u   /* protocol-error flag — OR with a valid ACK */
#define DAP_TRANSFER_MISMATCH 0x10u

/* ----- Default protocol version / port states ----------------------- */

#define EDEV_DAP_PORT_DISABLED   0
#define EDEV_DAP_PORT_SWD        1
#define EDEV_DAP_PORT_JTAG       2

#ifndef EDEV_DAPV2_DAP_FW_VER
#define EDEV_DAPV2_DAP_FW_VER    "2.2.0"
#endif

#endif /* EDEV_DAPV2_DAP_CONFIG_H */
