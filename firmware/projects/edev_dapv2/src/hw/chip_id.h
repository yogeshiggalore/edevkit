/*
 * chip_id.h — RP2350 per-unit serial number derived from the 64-bit
 * unique board ID exposed by pico-sdk's pico_unique_id library.
 *
 * The string is formatted as "E464-XXXX-XXXX-XXXX" (17 chars + NUL), with
 * "E464" being the edev_dapv2 prefix (E=Edevkit-family, 464 = decimal
 * tag for "dapv2"). The four hex groups are the high-to-low half-words
 * of the chip ID, uppercase. This ends up as the iSerialNumber USB
 * string descriptor.
 */

#ifndef EDEV_DAPV2_CHIP_ID_H
#define EDEV_DAPV2_CHIP_ID_H

#include <stddef.h>

/* "E464" + 3 × "-XXXX" + "\0" = 4 + 15 + 1 = 20 */
#define EDEV_DAPV2_SERIAL_LEN 20

/* Initialise the static serial buffer from the bootrom chip ID. Safe to
 * call multiple times — only reads the chip ID the first time. */
void chip_id_init(void);

/* Returns a pointer to a NUL-terminated ASCII serial string. The buffer
 * lives in BSS and is valid for the lifetime of the firmware. */
const char *chip_id_serial(void);

#endif /* EDEV_DAPV2_CHIP_ID_H */
