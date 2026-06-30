/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * nrf53_priv.h — module-internal header shared across the nrf53_*.c files.
 * Not exported outside the module.
 */
#ifndef EDEV_NRF53_PRIV_H_
#define EDEV_NRF53_PRIV_H_

#include "nrf53.h"

#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the bound SWDP device, or NULL if nrf53_bind_swdp() hasn't run. */
const struct device *nrf53__swdp(void);

/* Single-shot SWD transfer with WAIT-ACK retry + ACK→status mapping.
 *   req: pre-built SWDP request bits (APnDP | RnW | A2 | A3)
 *   data: in for writes, out for reads (LSB-first 32 bits)
 *   returns: NRF53_OK / WAIT / FAULT / NO_ACK / PROTO / NO_DEV
 */
nrf53_status_t nrf53__transfer(uint8_t req, uint32_t *data);

#ifdef __cplusplus
}
#endif

#endif /* EDEV_NRF53_PRIV_H_ */
