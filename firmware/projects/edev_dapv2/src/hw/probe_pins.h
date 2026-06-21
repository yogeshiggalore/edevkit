/*
 * probe_pins.h — pin assignments for the probe bit-banger.
 *
 * Generic Raspberry Pi Pico 2 layout — see docs/ARCHITECTURE.md for the
 * full table. When porting to the edevkit1 PCB (Vtgt sense, level
 * shifter, eFuse), override these values via -D on the compiler line.
 */

#ifndef EDEV_DAPV2_PROBE_PINS_H
#define EDEV_DAPV2_PROBE_PINS_H

#ifndef EDEV_PROBE_PIN_SWCLK
#define EDEV_PROBE_PIN_SWCLK     2u
#endif

#ifndef EDEV_PROBE_PIN_SWDIO
#define EDEV_PROBE_PIN_SWDIO     3u   /* must be SWCLK + 1 — PIO does
                                      consecutive-pindirs init */
#endif

#ifndef EDEV_PROBE_PIN_TDI
#define EDEV_PROBE_PIN_TDI       4u
#endif

#ifndef EDEV_PROBE_PIN_TDO
#define EDEV_PROBE_PIN_TDO       5u
#endif

#ifndef EDEV_PROBE_PIN_RESET
#define EDEV_PROBE_PIN_RESET     6u
#endif

#ifndef EDEV_PROBE_PIN_SWO
#define EDEV_PROBE_PIN_SWO       8u
#endif

#ifndef EDEV_PROBE_SM
#define EDEV_PROBE_SM            0u
#endif

#define EDEV_PROBE_DEFAULT_SWCLK_KHZ   1000u   /* 1 MHz at probe boot — hosts
                                                will set their own via
                                                DAP_SWJ_Clock */

#endif /* EDEV_DAPV2_PROBE_PINS_H */
