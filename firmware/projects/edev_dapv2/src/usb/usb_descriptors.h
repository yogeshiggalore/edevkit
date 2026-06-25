/*
 * usb_descriptors.h — accessors for the static descriptor blobs.
 */

#ifndef EDEV_DAPV2_USB_DESCRIPTORS_H
#define EDEV_DAPV2_USB_DESCRIPTORS_H

#include <stdint.h>

/* Endpoint numbers (in/out direction encoded in the high bit). */
#define EDEV_DAP_EP_OUT          0x01u    /* host → probe: DAP requests   */
#define EDEV_DAP_EP_IN           0x81u    /* probe → host: DAP responses  */

#define EDEV_CDC_EP_NOTIF        0x82u    /* CDC interrupt notification    */
#define EDEV_CDC_EP_OUT          0x03u    /* host → probe: CDC bulk        */
#define EDEV_CDC_EP_IN           0x83u    /* probe → host: CDC bulk        */

/* Interface numbers. Order MATTERS — TinyUSB matches drivers by class
 * descriptor in this order. */
#define EDEV_DAP_ITF_NUM         0u
#define EDEV_CDC_ITF_NUM         1u       /* CDC Control                   */
#define EDEV_CDC_DATA_ITF_NUM    2u       /* CDC Data                      */

/* Total length of the MS OS 2.0 descriptor set the BOS chain references. */
extern const uint16_t edev_ms_os_20_desc_len;

#endif /* EDEV_DAPV2_USB_DESCRIPTORS_H */
