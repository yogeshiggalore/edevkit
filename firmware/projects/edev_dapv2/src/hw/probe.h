/*
 * probe.h — bit-bang shim sitting on top of the PIO program.
 *
 * The C-side surface presented to the DAP command handlers is:
 *
 *   probe_init()          — bring up the PIO program + GPIOs.
 *   probe_deinit()        — release them.
 *
 *   probe_set_swclk_freq_khz(f)
 *                         — tune the PIO clock divider so the wire
 *                           clock matches the host's DAP_SWJ_Clock.
 *
 *   probe_write_bits(n, data)
 *                         — drive SWDIO out, shift n bits LSB-first,
 *                           toggling SWCLK. 1 ≤ n ≤ 256.
 *
 *   probe_read_bits(n)    — tristate SWDIO, sample n bits LSB-first
 *                           into a uint32_t (right-aligned). n ≤ 32.
 *
 *   probe_hiz_clocks(n)   — clock n cycles with SWDIO tristated.
 *                           Used for SWD turnaround.
 *
 *   probe_drive_swclk(v)  — direct GPIO drive of SWCLK (used by
 *                           DAP_SWJ_Pins). Disables the PIO state
 *                           machine first.
 *
 *   probe_drive_swdio(v)
 *   probe_drive_nreset(v) — likewise.
 *
 *   probe_read_swclk/swdio/nreset
 *                         — readback values for DAP_SWJ_Pins.
 *
 * All bit transfers are LSB-first because that's what SWD natively
 * is and what the PIO program is configured for (shift-right).
 */

#ifndef EDEV_DAPV2_PROBE_H
#define EDEV_DAPV2_PROBE_H

#include <stdbool.h>
#include <stdint.h>

void     probe_init (void);
void     probe_deinit(void);

void     probe_set_swclk_freq_khz(uint32_t freq_khz);

void     probe_write_bits (uint32_t bit_count, uint32_t data);
uint32_t probe_read_bits  (uint32_t bit_count);
void     probe_hiz_clocks (uint32_t bit_count);

/* Direct-GPIO bit-bang of SWJ_Sequence, bypassing PIO. Matches CMSIS_5
 * reference SWJ_Sequence exactly: byte 0 first, LSB-first per byte,
 * SWDIO set BEFORE SWCLK rising edge so target samples a stable bit.
 *
 * Used by DAP_SWJ_Sequence (the protocol-switch / line-reset path)
 * because the PIO write loop leaves SWCLK held HIGH between consecutive
 * USB packets — fine for one big sequence (e.g. openocd's combined
 * pattern) but causes pyocd / probe-rs to miss the wake-up when they
 * split the activation across 4–5 small packets.
 *
 * After this returns, SWCLK is LOW and SWDIO is OUTPUT at the last
 * driven value. The next DAP_Transfer / DAP_SWD_Sequence will re-engage
 * PIO via ensure_pio_pin_ownership(). */
void     probe_swj_bitbang(uint32_t bit_count, const uint8_t *data);

/* DAP_SWJ_Pins helpers — these take the PIO state machine offline so
 * the host can drive lines directly. Call probe_init() again after
 * you're done to re-attach PIO. In practice the host calls
 * DAP_SWJ_Sequence shortly after DAP_SWJ_Pins, which goes through
 * probe_write_bits and implicitly re-asserts PIO ownership. */
void     probe_drive_swclk (bool high);
void     probe_drive_swdio (bool high);
void     probe_drive_nreset(bool high);

bool     probe_read_swclk (void);
bool     probe_read_swdio (void);
bool     probe_read_nreset(void);

/* Continuous SWCLK keep-alive — runs a second PIO SM that toggles SWCLK
 * at a low frequency (~few kHz) whenever the probe is idle between
 * real SWD transactions. Mimics J-Link's "continuous SWCLK" feature.
 *
 * Helps with low-power MCUs (nRF5340 especially) whose DP clock-gates
 * itself when SWCLK is held quiet for too long — the next real
 * transaction then FAULTs while DP spins back up. Continuous SWCLK
 * prevents the DP from gating in the first place.
 *
 * Enabled by default. Disable via DAP_VENDOR 0x8B (EDEV_KEEPALIVE_CTL)
 * or this function (host-firmware test harnesses).
 *
 * Both this SM and the probe-program SM share the SWCLK pin via
 * explicit handoff in probe_write_bits / probe_read_bits / etc — they
 * are NEVER enabled simultaneously. */
void     probe_keepalive_set_enabled(bool enable);
bool     probe_keepalive_is_enabled (void);

#endif /* EDEV_DAPV2_PROBE_H */
