/*
 * jtag.h — JTAG bit-bang shim.
 *
 * v0.1 implementation uses direct SIO GPIO toggling rather than a
 * dedicated PIO program. The reasoning: JTAG needs 3 outputs
 * (TCK/TMS/TDI) and 1 input (TDO), which doesn't fit probe.pio's
 * 2-consecutive-pins model. A dedicated JTAG PIO program is ~20
 * instructions of careful timing; SIO bit-bang gives ~1–2 MHz on
 * RP2350 which covers TAP scan, IDCODE read, and ROM-table walks —
 * the dominant JTAG use cases for a modern debug probe.
 *
 * Pin ownership: while in JTAG mode, this module owns TCK + TMS + TDI
 * + TDO. On DAP_Disconnect or DAP_Connect(SWD), jtag_detach() hands
 * SWCLK/SWDIO back to PIO ownership; the next probe_*() call will
 * re-attach via ensure_pio_pin_ownership().
 */

#ifndef EDEV_DAPV2_JTAG_H
#define EDEV_DAPV2_JTAG_H

#include <stdbool.h>
#include <stdint.h>

void jtag_init  (void);    /* one-time setup (called from probe_init) */
void jtag_attach(void);    /* called on DAP_Connect(JTAG) */
void jtag_detach(void);    /* called on Disconnect or Connect(SWD) */

/* Set TCK frequency. The bit-bang loop derives its half-period from
 * this; very low values become a busy-wait, very high values cap at
 * whatever GPIO toggling can do (~2 MHz on RP2350 with -O2). */
void jtag_set_tck_freq_khz(uint32_t freq_khz);

/* Shift `bit_count` (1..64) bits through TDI with TMS held at the
 * specified level. If `capture` is true, the returned uint64_t holds
 * the TDO sample sequence (LSB-first). If false, return value is
 * undefined.
 *
 * Caller passes TDI bits LSB-first in `tdi`. */
uint64_t jtag_shift(uint8_t bit_count, uint64_t tdi, bool tms_high, bool capture);

#endif /* EDEV_DAPV2_JTAG_H */
