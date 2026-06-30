/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * nrf53_stubs.c — embedded Net-core stub blob + the loader that
 * uploads it to Net flash, triggers Net CPU reset, and verifies the
 * on-target completion marker.
 *
 * Blob is the assembled output of src/nrf53/nrf53_stubs/net_disable_approtect.s.
 * If you edit the .s, re-run:
 *   arm-zephyr-eabi-as -mcpu=cortex-m33 -mthumb net_disable_approtect.s -o stub.o
 *   arm-zephyr-eabi-objcopy -O binary stub.o stub.bin
 *   xxd -i stub.bin   # paste the resulting byte array below
 *
 * Algorithm reference: docs/NRF5340_ALGORITHMS.md §4 Stages 4–8 + §7.
 */

#include "nrf53.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(nrf53_stubs, CONFIG_EDEV_NRF53_OPS_LOG_LEVEL);

/* ------------------------------------------------------------------ */
/* Net stub binary blob — 196 B from net_disable_approtect.s          */
/* ------------------------------------------------------------------ */
const uint8_t nrf53_net_stub[] = {
	/* Vector table: 16 × 4 B = 64 B (offset 0x00..0x3F) */
	0x00, 0x00, 0x01, 0x21,  /* 0x00: MSP = 0x21010000 */
	0x59, 0x00, 0x00, 0x00,  /* 0x04: Reset @ 0x58|1 (Thumb) */
	0x41, 0x00, 0x00, 0x00,  /* 0x08: NMI → fault_handler */
	0x41, 0x00, 0x00, 0x00,  /* 0x0C: HardFault */
	0x41, 0x00, 0x00, 0x00,  /* 0x10: MemManage */
	0x41, 0x00, 0x00, 0x00,  /* 0x14: BusFault */
	0x41, 0x00, 0x00, 0x00,  /* 0x18: UsageFault */
	0x41, 0x00, 0x00, 0x00,  /* 0x1C: SecureFault */
	0x00, 0x00, 0x00, 0x00,  /* 0x20: reserved */
	0x00, 0x00, 0x00, 0x00,  /* 0x24: reserved */
	0x00, 0x00, 0x00, 0x00,  /* 0x28: reserved */
	0x41, 0x00, 0x00, 0x00,  /* 0x2C: SVCall */
	0x41, 0x00, 0x00, 0x00,  /* 0x30: DebugMonitor */
	0x00, 0x00, 0x00, 0x00,  /* 0x34: reserved */
	0x41, 0x00, 0x00, 0x00,  /* 0x38: PendSV */
	0x41, 0x00, 0x00, 0x00,  /* 0x3C: SysTick */

	/* fault_handler @ 0x40 — 24 B (0x40..0x57) */
	0x17, 0x48, 0x18, 0x49, 0x08, 0x60, 0xef, 0xf3,
	0x08, 0x80, 0x82, 0x69, 0x16, 0x49, 0x0a, 0x60,
	0xc2, 0x69, 0x16, 0x49, 0x0a, 0x60, 0xfe, 0xe7,

	/* reset_handler @ 0x58 — 72 B (0x58..0x9F) */
	0x4f, 0xf0, 0x04, 0x50, 0x4f, 0xf0, 0x11, 0x31,
	0x01, 0x60, 0x13, 0x4c, 0x13, 0x4d, 0x14, 0x4e,
	0x22, 0x68, 0x01, 0x2a, 0xfc, 0xd1, 0x01, 0x22,
	0x2a, 0x60, 0x22, 0x68, 0x01, 0x2a, 0xfc, 0xd1,
	0x4f, 0xf0, 0x04, 0x50, 0x4f, 0xf0, 0x22, 0x31,
	0x01, 0x60, 0x0e, 0x48, 0x06, 0x60, 0x22, 0x68,
	0x01, 0x2a, 0xfc, 0xd1, 0x00, 0x22, 0x2a, 0x60,
	0x22, 0x68, 0x01, 0x2a, 0xfc, 0xd1, 0x4f, 0xf0,
	0x04, 0x50, 0x09, 0x49, 0x01, 0x60, 0xfe, 0xe7,

	/* Literal pool @ 0xA0 — 36 B (0xA0..0xC3) */
	0xd5, 0x00, 0xdf, 0xba,  /* fault sentinel 0xBADF00D5 */
	0x04, 0x00, 0x00, 0x21,  /* 0x21000004 — fault offset */
	0x08, 0x00, 0x00, 0x21,  /* 0x21000008 */
	0x0c, 0x00, 0x00, 0x21,  /* 0x2100000C */
	0x00, 0x04, 0x08, 0x41,  /* NET_NVMC_READY 0x41080400 */
	0x04, 0x05, 0x08, 0x41,  /* NET_NVMC_CONFIG 0x41080504 */
	0xfa, 0x50, 0xfa, 0x50,  /* UNLOCK_MAGIC 0x50FA50FA */
	0x00, 0x80, 0xff, 0x01,  /* NET_UICR_APPROT 0x01FF8000 */
	0xde, 0xc0, 0xad, 0xde,  /* DONE_MAGIC 0xDEADC0DE */
};
const uint16_t nrf53_net_stub_size = sizeof(nrf53_net_stub);

BUILD_ASSERT(sizeof(nrf53_net_stub) == 196,
	     "Net stub size drift — rebuild from .s if you edited it");

/* ------------------------------------------------------------------ */
/* Loader timing                                                      */
/* ------------------------------------------------------------------ */
/* NVMC.READY timeout per word write. The stub is tiny so 100 ms is
 * plenty even on a slow probe link. */
#define NET_STUB_WORD_TIMEOUT_MS  100U

/* CTRL-AP#3 RESET pulse — same as the Python ref's recover flow. */
#define CTRL_AP3_RESET_ASSERT_MS  50U
#define CTRL_AP3_RESET_RELEASE_MS 200U

/* After release, poll the SRAM marker for up to this long. The stub
 * is microsecond-fast in practice, but the RESET-release path can be
 * lazy on cold Nordic chips. */
#define STUB_MARKER_WAIT_MS       500U
#define STUB_MARKER_POLL_MS       20U

/* ------------------------------------------------------------------ */
nrf53_status_t nrf53_uicr_program_net(uint32_t *out_marker,
				      uint32_t *out_approtect)
{
	const uint32_t net_flash_base = 0x01000000U;
	const uint32_t net_sram_marker = NRF53_NET_SRAM_MARKER;
	const uint32_t net_uicr = NRF53_NET_UICR_APPROTECT;
	nrf53_status_t st;

	/* Bug 6b mitigation — explicit DP cleanup before any Net AHB-AP
	 * work. The Python ref skipped this and Net writes faulted
	 * mid-flow after CTRL-AP ops. */
	(void)nrf53_dp_sticky_clear();
	(void)nrf53_dp_power_up(50);

	/* Stage 4 — Release Net core (App-side write to FORCEOFF=0). */
	st = nrf53_mem_write(NRF53_AP_APP, NRF53_CSW_APP,
			     NRF53_RESET_NETWORK_FORCEOFF, 0U);
	if (st != NRF53_OK) {
		LOG_ERR("Net FORCEOFF release failed: %s", nrf53_status_str(st));
		return st;
	}
	LOG_INF("Net FORCEOFF cleared");

	/* Bug 6b mitigation — sticky clear again after the App-side
	 * cross-bridge write. */
	(void)nrf53_dp_sticky_clear();
	(void)nrf53_dp_power_up(50);

	/* Stage 5 — Write stub into Net flash via Net AHB-AP + Net NVMC. */
	st = nrf53_nvmc_set_config(NRF53_AP_NET, NRF53_CSW_NET,
				   NRF53_NET_NVMC_READY,
				   NRF53_NET_NVMC_CONFIG,
				   NRF53_NVMC_CONFIG_WEN,
				   NET_STUB_WORD_TIMEOUT_MS);
	if (st != NRF53_OK) {
		LOG_ERR("Net NVMC.CONFIG=Wen failed: %s", nrf53_status_str(st));
		return st;
	}

	const size_t word_count = sizeof(nrf53_net_stub) / 4U;
	for (size_t i = 0; i < word_count; i++) {
		uint32_t word;
		/* Bytes are little-endian — assemble word_lo first. */
		const uint8_t *p = &nrf53_net_stub[i * 4U];
		word = (uint32_t)p[0]
		     | ((uint32_t)p[1] << 8)
		     | ((uint32_t)p[2] << 16)
		     | ((uint32_t)p[3] << 24);

		if (word == 0xFFFFFFFFU) {
			continue;   /* matches erased flash; skip to save time */
		}

		uint32_t addr = net_flash_base + (uint32_t)(i * 4U);
		st = nrf53_mem_write(NRF53_AP_NET, NRF53_CSW_NET, addr, word);
		if (st != NRF53_OK) {
			LOG_ERR("Net flash word write @ 0x%08x failed: %s",
				addr, nrf53_status_str(st));
			(void)nrf53_nvmc_set_config(NRF53_AP_NET, NRF53_CSW_NET,
						    NRF53_NET_NVMC_READY,
						    NRF53_NET_NVMC_CONFIG,
						    NRF53_NVMC_CONFIG_REN,
						    NET_STUB_WORD_TIMEOUT_MS);
			return st;
		}

		st = nrf53_nvmc_wait_ready(NRF53_AP_NET, NRF53_CSW_NET,
					   NRF53_NET_NVMC_READY,
					   NET_STUB_WORD_TIMEOUT_MS);
		if (st != NRF53_OK) {
			LOG_ERR("Net NVMC.READY after word @ 0x%08x failed: %s",
				addr, nrf53_status_str(st));
			(void)nrf53_nvmc_set_config(NRF53_AP_NET, NRF53_CSW_NET,
						    NRF53_NET_NVMC_READY,
						    NRF53_NET_NVMC_CONFIG,
						    NRF53_NVMC_CONFIG_REN,
						    NET_STUB_WORD_TIMEOUT_MS);
			return st;
		}
	}

	st = nrf53_nvmc_set_config(NRF53_AP_NET, NRF53_CSW_NET,
				   NRF53_NET_NVMC_READY,
				   NRF53_NET_NVMC_CONFIG,
				   NRF53_NVMC_CONFIG_REN,
				   NET_STUB_WORD_TIMEOUT_MS);
	if (st != NRF53_OK) {
		LOG_ERR("Net NVMC.CONFIG=Ren restore failed: %s", nrf53_status_str(st));
		return st;
	}
	LOG_INF("Net stub written (%u bytes)", (unsigned int)sizeof(nrf53_net_stub));

	/* Stage 6 — Pulse CTRL-AP#3 RESET to launch the Net stub. */
	(void)nrf53_dp_sticky_clear();   /* bug 6b */

	st = nrf53_ap_write(NRF53_CTRL_AP_NET, NRF53_CTRL_AP_RESET, 1U);
	if (st != NRF53_OK) {
		LOG_ERR("CTRL-AP#3 RESET=1 failed: %s", nrf53_status_str(st));
		return st;
	}
	k_msleep(CTRL_AP3_RESET_ASSERT_MS);

	st = nrf53_ap_write(NRF53_CTRL_AP_NET, NRF53_CTRL_AP_RESET, 0U);
	if (st != NRF53_OK) {
		LOG_ERR("CTRL-AP#3 RESET=0 failed: %s", nrf53_status_str(st));
		return st;
	}
	k_msleep(CTRL_AP3_RESET_RELEASE_MS);
	LOG_INF("CTRL-AP#3 reset pulsed — Net stub running");

	/* After CTRL-AP#3 RESET, the DP/AP state may be torn down — the
	 * Net core reset can ripple through the debug arbitration. The
	 * Python reference does a FULL DP wake at this boundary, not
	 * just sticky_clear + power_up. Without this, subsequent Net
	 * AHB-AP reads return 0x00000000 (the bus arbitration is in a
	 * weird state and reads don't actually reach Net SRAM).
	 *
	 * Empirically: nRF5340 + stub-not-running symptom was marker
	 * reading 0x00000000 (not 0xFFFFFFFF erased, not 0xDEADC0DE
	 * done, not 0xBADF00D5 fault) — that's the signature of a Net
	 * AHB-AP read returning literal zeros because the AP path is
	 * disrupted, not because the stub didn't run. Re-issuing
	 * full_wake restores the bus state. */
	(void)nrf53_dp_full_wake(NULL);

	/* Stage 7 — Poll Net SRAM marker for completion. */
	uint32_t marker = 0;
	const int64_t deadline = k_uptime_get() + (int64_t)STUB_MARKER_WAIT_MS;
	while (k_uptime_get() < deadline) {
		(void)nrf53_mem_read(NRF53_AP_NET, NRF53_CSW_NET,
				     net_sram_marker, &marker);
		if (marker == NRF53_STUB_DONE_MAGIC) {
			break;
		}
		if (marker == NRF53_STUB_FAULT_MAGIC) {
			/* The stub's fault handler stamped its sentinel.
			 * Read PC + xPSR for diagnostics. */
			uint32_t pc = 0, xpsr = 0;
			(void)nrf53_mem_read(NRF53_AP_NET, NRF53_CSW_NET,
					     net_sram_marker + 8, &pc);
			(void)nrf53_mem_read(NRF53_AP_NET, NRF53_CSW_NET,
					     net_sram_marker + 12, &xpsr);
			LOG_ERR("Net stub FAULTED — PC=0x%08x xPSR=0x%08x", pc, xpsr);
			if (out_marker) { *out_marker = marker; }
			return NRF53_STUB_FAIL;
		}
		k_msleep(STUB_MARKER_POLL_MS);
	}
	if (out_marker) { *out_marker = marker; }
	if (marker != NRF53_STUB_DONE_MAGIC) {
		LOG_ERR("Net stub timeout — last marker=0x%08x (expected 0x%08x)",
			marker, NRF53_STUB_DONE_MAGIC);
		return NRF53_STUB_FAIL;
	}
	LOG_INF("Net stub completion marker 0xDEADC0DE seen");

	/* Stage 8 — Verify Net UICR.APPROTECT persisted. */
	uint32_t approtect = 0xFFFFFFFFU;
	(void)nrf53_mem_read(NRF53_AP_NET, NRF53_CSW_NET, net_uicr, &approtect);
	if (out_approtect) { *out_approtect = approtect; }
	if (approtect != NRF53_UNLOCK_VAL) {
		LOG_ERR("Net UICR.APPROTECT readback 0x%08x ≠ 0x50FA50FA",
			approtect);
		return NRF53_STUB_FAIL;
	}
	LOG_INF("Net UICR.APPROTECT = 0x50FA50FA confirmed");

	return NRF53_OK;
}
