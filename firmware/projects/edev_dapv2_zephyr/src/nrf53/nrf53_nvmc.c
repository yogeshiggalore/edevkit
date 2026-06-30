/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * nrf53_nvmc.c — Nordic NVMC sequencing helpers (READY-polling +
 * CONFIG-write). Same shape works for both the App and Net cores;
 * callers pass the right NVMC base addresses.
 *
 * Algorithm reference: docs/NRF5340_ALGORITHMS.md §4 (App UICR) +
 * §5b (Net flash write).
 */

#include "nrf53.h"
#include "nrf53_priv.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(nrf53_nvmc, CONFIG_EDEV_NRF53_OPS_LOG_LEVEL);

/* Inter-poll cadence for NVMC.READY. The Python reference uses 0.5 ms,
 * which on the Pico's USB-FS bulk link maps to ~50 µs of probe-side
 * work + a sub-millisecond k_sleep. */
#define NVMC_POLL_INTERVAL_US   500U

nrf53_status_t nrf53_nvmc_wait_ready(uint8_t ap_index, uint32_t csw,
				     uint32_t nvmc_ready_addr,
				     uint32_t timeout_ms)
{
	const int64_t deadline = k_uptime_get() + (int64_t)timeout_ms;
	uint32_t v = 0;
	do {
		nrf53_status_t st = nrf53_mem_read(ap_index, csw, nvmc_ready_addr, &v);
		if (st != NRF53_OK) {
			LOG_WRN("NVMC.READY read @ 0x%08x failed: %s",
				nvmc_ready_addr, nrf53_status_str(st));
			return st;
		}
		if (v & 0x01U) {
			return NRF53_OK;
		}
		k_usleep(NVMC_POLL_INTERVAL_US);
	} while (k_uptime_get() < deadline);

	LOG_ERR("NVMC.READY timeout @ 0x%08x (last=0x%08x)",
		nvmc_ready_addr, v);
	return NRF53_TIMEOUT;
}

nrf53_status_t nrf53_nvmc_set_config(uint8_t ap_index, uint32_t csw,
				     uint32_t nvmc_ready_addr,
				     uint32_t nvmc_config_addr,
				     uint32_t mode,
				     uint32_t timeout_ms)
{
	nrf53_status_t st = nrf53_nvmc_wait_ready(ap_index, csw,
						  nvmc_ready_addr, timeout_ms);
	if (st != NRF53_OK) {
		return st;
	}

	st = nrf53_mem_write(ap_index, csw, nvmc_config_addr, mode);
	if (st != NRF53_OK) {
		LOG_ERR("NVMC.CONFIG write (mode=%u) failed: %s",
			(unsigned int)mode, nrf53_status_str(st));
		return st;
	}

	return nrf53_nvmc_wait_ready(ap_index, csw, nvmc_ready_addr, timeout_ms);
}
