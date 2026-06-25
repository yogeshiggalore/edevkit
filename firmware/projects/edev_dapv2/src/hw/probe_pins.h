/*
 * probe_pins.h — RP2350 GPIO assignments for edev_dapv2 v0.1.
 *
 * Single voltage domain (3.3 V). No level shifters. See README §Pinout
 * and the Cortex Debug 10-pin connector mapping for wiring details.
 */

#ifndef EDEV_DAPV2_HW_PROBE_PINS_H
#define EDEV_DAPV2_HW_PROBE_PINS_H

#define PROBE_PIN_SWCLK   2u   /* GP2 — SWCLK / TCK, push-pull out      */
#define PROBE_PIN_SWDIO   3u   /* GP3 — SWDIO / TMS, bidirectional      */
#define PROBE_PIN_TDI     4u   /* GP4 — TDI (JTAG only), out            */
#define PROBE_PIN_TDO     5u   /* GP5 — TDO (JTAG) / SWO (SWD), in      */
#define PROBE_PIN_NRESET  6u   /* GP6 — nRESET, open-drain (drive 0)    */
#define PROBE_PIN_NTRST   7u   /* GP7 — nTRST (optional JTAG TAP reset) */

#define PROBE_PIN_VTGT_ADC      26u  /* GP26 / ADC0 — Vtgt sense (opt.) */
#define PROBE_PIN_LED_EXTERNAL  22u  /* GP22 — external LED, optional   */

#endif /* EDEV_DAPV2_HW_PROBE_PINS_H */
