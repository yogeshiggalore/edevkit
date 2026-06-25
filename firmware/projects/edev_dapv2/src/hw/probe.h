/*
 * probe.h — SWD bit-bang and SWJ pin shim.
 *
 * v0.1 uses SIO bit-bang in C (no PIO). Top clock ~1–2 MHz; sufficient
 * for proving correctness end-to-end. PIO upgrade is on the v0.2
 * roadmap.
 */

#ifndef EDEV_DAPV2_HW_PROBE_H
#define EDEV_DAPV2_HW_PROBE_H

#include <stdbool.h>
#include <stdint.h>

void probe_init(void);
void probe_connect_swd(void);
void probe_disconnect(void);
void probe_reset_target(void);

void probe_set_clock(uint32_t hz);
void probe_set_swd_config(uint8_t turnaround, uint8_t data_phase);
void probe_set_xfer_config(uint8_t idle_cycles,
                           uint16_t wait_retry, uint16_t match_retry);

/* SWJ_Pins helpers (CMSIS-DAP bit positions are decoded here). */
void    probe_swj_set_pins(uint8_t pin_select, uint8_t pin_output);
uint8_t probe_swj_get_pins(void);

/* Raw bit sequence — used for line reset, JTAG-to-SWD switch, dormant
 * wakeup. Drives SWCLK + SWDIO; bits sent LSB first per CMSIS-DAP. */
void probe_swj_sequence(uint16_t bit_count, const uint8_t *data);

/* SWD_Sequence — like swj_sequence but can switch SWDIO direction
 * mid-sequence and return bits read back. info: bit_count + dir flag
 * encoded per spec. Returns total bytes written to `out`. */
uint16_t probe_swd_sequence(uint8_t sequence_count,
                            const uint8_t *info_and_data, uint16_t data_len,
                            uint8_t *out, uint16_t out_cap);

/* Core SWD transfer.
 *
 * `request` is the 4-bit "header" packed as: bit0 APnDP, bit1 RnW,
 *           bit2 A2, bit3 A3 (matches DAP_Transfer per-xact byte).
 * For READ:  *data is filled with the 32-bit read value.
 * For WRITE: data_value contains the 32-bit value to write.
 * Returns the 3-bit ACK + parity-bad flag in low bits:
 *   bit 0: OK   bit 1: WAIT   bit 2: FAULT   bit 3: parity / no-ACK
 * (matches CMSIS-DAP DAP_Transfer response ACK encoding.)
 */
uint8_t probe_swd_transfer(uint8_t request, uint32_t data_value,
                           uint32_t *data_out);

/* Convenience: write DP ABORT (req = AP/DP=0, RnW=0, A2=0, A3=0). */
bool probe_swd_write_abort(uint32_t value);

#endif /* EDEV_DAPV2_HW_PROBE_H */
