/*
 * dap_internal.h — types and prototypes shared between dap_*.c handlers.
 */

#ifndef EDEV_DAPV2_DAP_INTERNAL_H
#define EDEV_DAPV2_DAP_INTERNAL_H

#include <stdint.h>

/* CMSIS-DAP command IDs (subset we implement; rest fall through to
 * dap_invalid_handler returning DAP_ERROR). */
#define ID_DAP_Info                  0x00u
#define ID_DAP_HostStatus            0x01u
#define ID_DAP_Connect               0x02u
#define ID_DAP_Disconnect            0x03u
#define ID_DAP_TransferConfigure     0x04u
#define ID_DAP_Transfer              0x05u
#define ID_DAP_TransferBlock         0x06u
#define ID_DAP_TransferAbort         0x07u
#define ID_DAP_WriteABORT            0x08u
#define ID_DAP_Delay                 0x09u
#define ID_DAP_ResetTarget           0x0Au
#define ID_DAP_SWJ_Pins              0x10u
#define ID_DAP_SWJ_Clock             0x11u
#define ID_DAP_SWJ_Sequence          0x12u
#define ID_DAP_SWD_Configure         0x13u
#define ID_DAP_SWD_Sequence          0x1Du
#define ID_DAP_JTAG_Sequence         0x14u
#define ID_DAP_JTAG_Configure        0x15u
#define ID_DAP_JTAG_IDCODE           0x16u
#define ID_DAP_SWO_Transport         0x17u
#define ID_DAP_SWO_Mode              0x18u
#define ID_DAP_SWO_Baudrate          0x19u
#define ID_DAP_SWO_Control           0x1Au
#define ID_DAP_SWO_Status            0x1Bu
#define ID_DAP_SWO_Data              0x1Cu
#define ID_DAP_SWO_ExtendedStatus    0x1Eu
#define ID_DAP_QueueCommands         0x7Eu   /* HID only */
#define ID_DAP_ExecuteCommands       0x7Fu

/* Status / response codes. */
#define DAP_OK                       0x00u
#define DAP_ERROR                    0xFFu

/* DAP_Connect modes (byte 1 of request, byte 1 of response). */
#define DAP_PORT_DEFAULT             0u    /* host doesn't care */
#define DAP_PORT_SWD                 1u
#define DAP_PORT_JTAG                2u
#define DAP_PORT_OFF                 0u    /* response when Disconnect */

/* DAP_Transfer response byte layout (resp[2] on the wire):
 *
 *   bits 0..2 = wire ACK — MUST be one of {1, 2, 4, 7}:
 *     1  OK         001
 *     2  WAIT       010
 *     4  FAULT      100
 *     7  NO_ACK     111  (target floating; illegal ACK codes also map here)
 *
 *   bit  3   = ProtocolError flag (parity bad, framing wrong, sticky
 *              error). OR-combine with bits 0:2 — *never* set alone.
 *
 *   bit  4   = ValueMismatch flag (match-read failed). OR-combine.
 *
 * Setting bit 3 alone (0x08) leaves bits 0:2 = 0, which is *not* a
 * valid wire ACK. pyocd masks `ack & 0x07` and rejects anything outside
 * {1,2,4,7}; probe-rs spams "Protocol error". Lesson from previous
 * tree's [[uh-dapv2-ack-framing-fix]]. */
#define DAP_TRANSFER_OK              (1u << 0)   /* 0x01 */
#define DAP_TRANSFER_WAIT            (1u << 1)   /* 0x02 */
#define DAP_TRANSFER_FAULT           (1u << 2)   /* 0x04 */
#define DAP_TRANSFER_NO_ACK          0x07u       /* 111 — flag, no ACK */
#define DAP_TRANSFER_ERROR           (1u << 3)   /* 0x08 — flag, OR-combine */
#define DAP_TRANSFER_MISMATCH        (1u << 4)   /* 0x10 — flag, OR-combine */

/* DAP_Transfer request flags (per-transaction byte). */
#define DAP_TRANSFER_APnDP           (1u << 0)
#define DAP_TRANSFER_RnW             (1u << 1)
#define DAP_TRANSFER_A2              (1u << 2)
#define DAP_TRANSFER_A3              (1u << 3)
#define DAP_TRANSFER_MATCH_VALUE     (1u << 4)
#define DAP_TRANSFER_MATCH_MASK      (1u << 5)
#define DAP_TRANSFER_TIMESTAMP       (1u << 7)

/* Per-handler signatures.
 * Each takes the rest of the request after the command byte (so req
 * starts at byte 1 of the wire packet), and writes the *response
 * payload* after the response command echo. Returns response payload
 * length. */
typedef uint16_t (*dap_handler_fn)(const uint8_t *req, uint16_t req_len,
                                   uint8_t *resp, uint16_t resp_cap);

uint16_t dap_handle_info       (const uint8_t *req, uint16_t req_len, uint8_t *resp, uint16_t resp_cap);
uint16_t dap_handle_host_status(const uint8_t *req, uint16_t req_len, uint8_t *resp, uint16_t resp_cap);
uint16_t dap_handle_connect    (const uint8_t *req, uint16_t req_len, uint8_t *resp, uint16_t resp_cap);
uint16_t dap_handle_disconnect (const uint8_t *req, uint16_t req_len, uint8_t *resp, uint16_t resp_cap);
uint16_t dap_handle_delay      (const uint8_t *req, uint16_t req_len, uint8_t *resp, uint16_t resp_cap);
uint16_t dap_handle_reset_tgt  (const uint8_t *req, uint16_t req_len, uint8_t *resp, uint16_t resp_cap);
uint16_t dap_handle_write_abort(const uint8_t *req, uint16_t req_len, uint8_t *resp, uint16_t resp_cap);

uint16_t dap_handle_swj_pins   (const uint8_t *req, uint16_t req_len, uint8_t *resp, uint16_t resp_cap);
uint16_t dap_handle_swj_clock  (const uint8_t *req, uint16_t req_len, uint8_t *resp, uint16_t resp_cap);
uint16_t dap_handle_swj_seq    (const uint8_t *req, uint16_t req_len, uint8_t *resp, uint16_t resp_cap);

uint16_t dap_handle_swd_config (const uint8_t *req, uint16_t req_len, uint8_t *resp, uint16_t resp_cap);
uint16_t dap_handle_swd_seq    (const uint8_t *req, uint16_t req_len, uint8_t *resp, uint16_t resp_cap);
uint16_t dap_handle_xfer_config(const uint8_t *req, uint16_t req_len, uint8_t *resp, uint16_t resp_cap);
uint16_t dap_handle_transfer   (const uint8_t *req, uint16_t req_len, uint8_t *resp, uint16_t resp_cap);
uint16_t dap_handle_xfer_block (const uint8_t *req, uint16_t req_len, uint8_t *resp, uint16_t resp_cap);

uint16_t dap_handle_jtag_seq   (const uint8_t *req, uint16_t req_len, uint8_t *resp, uint16_t resp_cap);
uint16_t dap_handle_jtag_cfg   (const uint8_t *req, uint16_t req_len, uint8_t *resp, uint16_t resp_cap);
uint16_t dap_handle_jtag_idcode(const uint8_t *req, uint16_t req_len, uint8_t *resp, uint16_t resp_cap);

uint16_t dap_handle_swo_xport  (const uint8_t *req, uint16_t req_len, uint8_t *resp, uint16_t resp_cap);
uint16_t dap_handle_swo_mode   (const uint8_t *req, uint16_t req_len, uint8_t *resp, uint16_t resp_cap);
uint16_t dap_handle_swo_baud   (const uint8_t *req, uint16_t req_len, uint8_t *resp, uint16_t resp_cap);
uint16_t dap_handle_swo_ctrl   (const uint8_t *req, uint16_t req_len, uint8_t *resp, uint16_t resp_cap);
uint16_t dap_handle_swo_status (const uint8_t *req, uint16_t req_len, uint8_t *resp, uint16_t resp_cap);
uint16_t dap_handle_swo_data   (const uint8_t *req, uint16_t req_len, uint8_t *resp, uint16_t resp_cap);
uint16_t dap_handle_swo_estat  (const uint8_t *req, uint16_t req_len, uint8_t *resp, uint16_t resp_cap);

uint16_t dap_handle_execute    (const uint8_t *req, uint16_t req_len, uint8_t *resp, uint16_t resp_cap);

/* Connect-mode state — set by dap_handle_connect, read by SWJ_Pins
 * etc. to know whether SWD or JTAG pin-direction policy applies. */
extern uint8_t dap_connected_port;

#endif /* EDEV_DAPV2_DAP_INTERNAL_H */
