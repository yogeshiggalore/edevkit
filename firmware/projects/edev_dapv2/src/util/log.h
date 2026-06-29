/*
 * log.h — minimal debug log over USB CDC.
 *
 * Writes to TinyUSB's stock CDC class (CFG_TUD_CDC=1). The host sees a
 * /dev/tty.usbmodem* alongside the CMSIS-DAP interface.
 *
 * Calls are best-effort: if the CDC EP is full or no host has opened
 * the port, bytes are dropped silently. Never blocks.
 */

#ifndef EDEV_DAPV2_UTIL_LOG_H
#define EDEV_DAPV2_UTIL_LOG_H

#include <stdint.h>

void log_init(void);
void log_task(void);                /* drain ring buffer to CDC */

void log_putc(char c);
void log_puts(const char *s);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_hex(const uint8_t *data, uint16_t len);

#endif /* EDEV_DAPV2_UTIL_LOG_H */
