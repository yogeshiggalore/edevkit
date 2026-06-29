/*
 * chip_id.c — RP2350 unique ID → "EDV-XXXX-XXXX-XXXX-XXXX" string.
 */

#include "hw/chip_id.h"

#include "pico/unique_id.h"

#include <stdio.h>

static char s_chip_id[CHIP_ID_STR_LEN];

void chip_id_init(void)
{
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);

    /* RP2350 returns 8 bytes; format as 4 groups of 4 hex chars. */
    snprintf(s_chip_id, sizeof(s_chip_id),
             "EDV-%02X%02X-%02X%02X-%02X%02X-%02X%02X",
             id.id[0], id.id[1], id.id[2], id.id[3],
             id.id[4], id.id[5], id.id[6], id.id[7]);
}

const char *chip_id_string(void)
{
    return s_chip_id;
}
