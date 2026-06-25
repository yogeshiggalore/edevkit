/*
 * log.c — debug log over USB CDC.
 *
 * Implementation: small SPSC ring buffer in RAM. Producer side (the
 * DAP handlers, SWD bit-bang, etc.) appends bytes. Consumer side
 * (log_task, called from the main loop) drains to TinyUSB's CDC TX
 * FIFO. Drops bytes if the ring overflows (host slow or no host).
 */

#include "util/log.h"

#include "tusb.h"
#include "pico/bootrom.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Standard pico-sdk trick: when host sets the CDC baud rate to 1200,
 * reboot into BOOTSEL mode. Lets `picotool reboot -fu` work via the
 * 1200-baud touch from picotool. Hook the TinyUSB line-coding cb. */
void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const* p_line_coding)
{
    (void) itf;
    if (p_line_coding->bit_rate == 1200) {
        reset_usb_boot(0, 0);
    }
}

#define LOG_RING_LEN  4096u             /* must be power of two */
#define LOG_RING_MASK (LOG_RING_LEN - 1u)

static char     s_ring[LOG_RING_LEN];
static volatile uint32_t s_head;        /* producer */
static volatile uint32_t s_tail;        /* consumer */

void log_init(void)
{
    s_head = 0;
    s_tail = 0;
}

void log_putc(char c)
{
    uint32_t next = (s_head + 1u) & LOG_RING_MASK;
    if (next == s_tail) {
        /* Drop oldest — slide tail. */
        s_tail = (s_tail + 1u) & LOG_RING_MASK;
    }
    s_ring[s_head] = c;
    s_head = next;
}

void log_puts(const char *s)
{
    while (*s) log_putc(*s++);
}

void log_printf(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((unsigned) n > sizeof(buf) - 1) n = sizeof(buf) - 1;
    for (int i = 0; i < n; ++i) log_putc(buf[i]);
}

void log_hex(const uint8_t *data, uint16_t len)
{
    static const char hex[] = "0123456789abcdef";
    for (uint16_t i = 0; i < len; ++i) {
        log_putc(hex[(data[i] >> 4) & 0xFu]);
        log_putc(hex[data[i] & 0xFu]);
    }
}

void log_task(void)
{
    /* Drain unconditionally. If no host has DTR'd, tud_cdc_write
     * returns 0; we leave bytes in the ring. macOS `cat` doesn't set
     * DTR, so tud_cdc_connected() can't be the gate. */
    while (s_tail != s_head) {
        uint32_t avail = tud_cdc_write_available();
        if (avail == 0) break;
        uint32_t to_send = avail;
        /* Slice up to ring end */
        uint32_t until_end = LOG_RING_LEN - s_tail;
        if (to_send > until_end) to_send = until_end;
        /* And up to head */
        uint32_t to_head = (s_head >= s_tail)
                         ? (s_head - s_tail)
                         : (LOG_RING_LEN - s_tail);
        if (to_send > to_head) to_send = to_head;
        if (to_send == 0) break;
        uint32_t wrote = tud_cdc_write(&s_ring[s_tail], to_send);
        s_tail = (s_tail + wrote) & LOG_RING_MASK;
        if (wrote < to_send) break;
    }
    tud_cdc_write_flush();
}
