/*
 * usb_dap_class.h — custom TinyUSB class driver for the CMSIS-DAP v2
 * vendor interface.
 *
 * The class driver claims the three bulk endpoints declared in
 * usb_descriptors.c and pumps DAP packets through a pair of ring
 * buffers (request: USB OUT → main, response: main → USB IN).
 *
 * The main loop calls edev_dap_class_task() each iteration; it drains
 * request slots into the dispatcher and pushes any pending response
 * slots out on EP IN.
 */

#ifndef EDEV_DAPV2_USB_DAP_CLASS_H
#define EDEV_DAPV2_USB_DAP_CLASS_H

#include <stdbool.h>
#include <stdint.h>

/* Drain pending request packets through the dispatcher, kick the IN
 * endpoint if responses are queued. Non-blocking. Call every main loop. */
void edev_dap_class_task(void);

/* SWO data ingress — called by swo.c (later milestone) when a chunk is
 * ready to ship to the host on EP 0x82. Returns true if the transfer
 * was accepted, false if the endpoint is busy or unopened. */
bool edev_dap_class_swo_submit(const uint8_t *buf, uint16_t len);
bool edev_dap_class_swo_is_busy(void);

#endif /* EDEV_DAPV2_USB_DAP_CLASS_H */
