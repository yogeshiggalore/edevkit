/*
 * swo.h — SWO trace UART receiver. PIO-based at the line level, ring
 * buffer in SRAM, polled drain from the main loop.
 *
 * Bandwidth: at clk_sys = 150 MHz, PIO clk = 8 × baud, max baud is
 * limited by the PIO divider's fractional precision and the receiver
 * pulse-width tolerance. Practical ceiling ~3 Mbaud, conservative
 * recommendation 1 Mbaud (= ~100 KB/s of trace bytes, which is well
 * within ITM's typical use).
 *
 * v0.1 uses polled drain from the main loop — at every loop pass we
 * pop everything in the PIO RX FIFO into the ring buffer. Main loop
 * iterates fast enough (tens of µs/iter) to keep up. If we ever
 * need ≥ 1 Mbaud sustained we upgrade to DMA from PIO0 → SRAM ring;
 * the API below stays unchanged.
 */

#ifndef EDEV_DAPV2_SWO_H
#define EDEV_DAPV2_SWO_H

#include <stdbool.h>
#include <stdint.h>

void     swo_init(void);

/* Configure the baudrate. Returns the actual achievable baud (which
 * may differ slightly from requested due to PIO divider quantisation).
 * Returns 0 if the request is unachievable (e.g. > 5 Mbaud). */
uint32_t swo_set_baudrate(uint32_t requested_baud);

void     swo_start(void);   /* enable PIO SM + flush ring buffer */
void     swo_stop (void);   /* disable PIO SM                    */

bool     swo_is_active  (void);
bool     swo_has_overrun(void);
uint32_t swo_bytes_available(void);

/* Read up to `max` bytes from the ring into `out`. Returns the
 * actual number of bytes copied. Used by DAP_SWO_Data (polled mode). */
uint32_t swo_read(uint8_t *out, uint32_t max);

/* Per-iteration drain + submit. Called from the main loop:
 *   - pop everything from PIO RX FIFO into the ring buffer
 *   - if the bulk SWO endpoint is idle and the ring has data,
 *     submit a chunk via edev_dap_class_swo_submit(). */
void     swo_task(void);

#endif /* EDEV_DAPV2_SWO_H */
