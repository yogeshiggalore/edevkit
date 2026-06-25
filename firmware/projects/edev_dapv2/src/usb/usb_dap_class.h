/*
 * usb_dap_class.h — custom TinyUSB class driver for the DAP interface.
 */

#ifndef EDEV_DAPV2_USB_DAP_CLASS_H
#define EDEV_DAPV2_USB_DAP_CLASS_H

void edev_dap_class_init(void);

/* Pump the DAP request/response rings. Call once per main-loop tick. */
void edev_dap_class_task(void);

#endif /* EDEV_DAPV2_USB_DAP_CLASS_H */
