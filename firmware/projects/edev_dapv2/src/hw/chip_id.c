/*
 * chip_id.c — format the 64-bit RP2350 unique ID into "E464-XXXX-XXXX-XXXX".
 */

#include "chip_id.h"

#include <stdint.h>
#include <stdbool.h>

#include "pico/unique_id.h"

static char s_serial[EDEV_DAPV2_SERIAL_LEN];
static bool s_inited;

static char hex_nibble(uint8_t v)
{
    return v < 10 ? (char)('0' + v) : (char)('A' + (v - 10));
}

void chip_id_init(void)
{
    if (s_inited) {
        return;
    }

    pico_unique_board_id_t bid;
    pico_get_unique_board_id(&bid);

    /* Layout: "E464-AAAA-BBBB-CCCC" where AAAA = bytes [0..1] of the
     * board id (uppercase hex), BBBB = [2..3] then [4..5], CCCC = [6..7].
     * pico_unique_board_id_t holds 8 bytes — typically the QSPI flash
     * unique ID; on RP2350 boards with non-volatile OTP we use that
     * instead, but pico_get_unique_board_id() abstracts that away. */
    char *p = s_serial;
    *p++ = 'E';
    *p++ = '4';
    *p++ = '6';
    *p++ = '4';

    for (uint8_t group = 0; group < 3; group++) {
        *p++ = '-';
        for (uint8_t byte = 0; byte < 2; byte++) {
            uint8_t b = bid.id[group * 2 + byte];
            *p++ = hex_nibble(b >> 4);
            *p++ = hex_nibble(b & 0x0f);
        }
    }
    *p = '\0';

    s_inited = true;
}

const char *chip_id_serial(void)
{
    if (!s_inited) {
        chip_id_init();
    }
    return s_serial;
}
