/*
 * chip_id.h — per-unit serial number derived from RP2350 unique ID.
 *
 * Format: "EDV-XXXX-XXXX-XXXX-XXXX" — 23 chars including NUL.
 * Used by the USB iSerialNumber string descriptor.
 */

#ifndef EDEV_DAPV2_HW_CHIP_ID_H
#define EDEV_DAPV2_HW_CHIP_ID_H

#include <stddef.h>

#define CHIP_ID_STR_LEN 24u   /* "EDV-XXXX-XXXX-XXXX-XXXX" + NUL */

/* Read RP2350 64-bit chip-id via bootrom and format the serial string.
 * Call once at boot before any consumer (USB descriptors, log prefix). */
void chip_id_init(void);

/* Returns pointer to the formatted "EDV-…" string. NUL-terminated. */
const char *chip_id_string(void);

#endif /* EDEV_DAPV2_HW_CHIP_ID_H */
