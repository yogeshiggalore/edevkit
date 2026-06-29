# nRF5340 wedge-state: edev probe firmware gap vs J-Link

## What was tested (2026-06-29)

Goal: same flash read / erase / write round-trip we ran on nRF52840 ([UPSTREAM_VALIDATES.md](UPSTREAM_VALIDATES.md)), but on a connected nRF5340 Ring (App + Net cores). Only 3 wires hooked up: **SWDIO, SWCLK, GND** — no TVCC, no nRESET (Ring product has none exposed).

## What works

| Op | Result |
|---|---|
| **Identify** | ✓ Both cores detected via DP.TARGETID (App 1024 KB @ 0x00000000, Net 256 KB @ 0x01000000) |
| **J-Link@200kHz read/erase** | ✓ Reads `flash[0] = 0xFFFFFFFF` after erase; full erase completes in 0.22 s |
| **Webgui `_full_dp_wake` fix** (commit `f1bebc6`) | ✓ Now correctly falls through to `dp.connect()` when SWJ_Sequence raises; CTRL-AP discovery works; `IndexError: bytearray index out of range` from /api/recover is unblocked |

## What fails (probe firmware gap)

After multiple back-to-back ERASEALL / reset attempts via our edev probe, the chip enters a deep wedge state where:

- AP#0 (App AHB-AP) writes accepted but reads return **WAIT ACK** continuously
- CTRL-AP#2 / CTRL-AP#3 `ERASEALL=1` writes accepted but `ERASEALLSTATUS` never returns to READY (stays at 0x01)
- `APPROTECTSTATUS = 0x50FA50FA` — UICR.APPROTECT is HwDisabled (Friday's recovery still in place), so the chip is *logically* unlocked. The lockup is on the CPU-bus / NVMC side, not APPROTECT.

In this wedge state:
- **J-Link@200kHz still works** (verified: erase + read at 200 kHz returns 0xFFFFFFFF cleanly)
- **edev probe@200kHz returns USB timeout** to probe-rs:
  ```
  Error handling CMSIS-DAP command Transfer.
  Timeout in USB communication.
  ```
  Same symptom on our pico-sdk firmware AND our Zephyr port. The chip's deep WAIT-ACK responses (which J-Link patiently retries) exceed the per-DAP-command USB-response budget our firmware emits.

## Root cause analysis

J-Link's CMSIS-DAP-equivalent firmware does two things our probe doesn't:

1. **SWCLK keep-alive between transactions** — keeps the chip's debug clock domain alive across idle gaps so it doesn't gate down. The pico-sdk firmware *used to* have this (memory: `project_swclk_keepalive_pio_2026_06_19`) but the v0.2 PIO rebuild on 2026-06-25 dropped it. The Zephyr port (GPIO bit-banger) doesn't have it.
2. **WAIT-ACK retry that stays within USB-response budget** — when the chip says WAIT, J-Link retries up to thousands of times AND drives SWCLK between retries to keep the debug domain alive AND still returns the DAP response inside the host's USB timeout. Our probe firmware either gives up too fast or burns through retries without keeping SWCLK clocked.

## Implications for the edev_dapv2 product

For chips that don't enter the deep wedge state (clean chips fresh out of the box, healthy nRF52, etc.) the probe works fine — see the **nRF52840 full round-trip** validated this session.

For chips already wedged from prior debug sessions, our probe genuinely can't recover them without firmware-level CMSIS-DAP improvements:
- Add SWCLK keep-alive (re-port the dropped pico-sdk PIO state machine, or add a software equivalent in Zephyr)
- Re-tune WAIT-ACK retry to balance retry count vs USB-response budget

This is multi-day firmware work, not a quick patch.

## Practical recommendations

| Use case | Tool |
|---|---|
| nRF52840 / nRF52833 / nRF52832 | ✓ edev probe (Zephyr port, validated this session) |
| nRF5340 in clean state | edev probe should work — try first, fall back to J-Link if it gets wedged |
| nRF5340 already wedged from a prior session | **J-Link until we add SWCLK keep-alive + better WAIT-ACK retry to edev firmware** |
| BLE roadmap (Zephyr BT stack on RP2350) | ✓ edev Zephyr port is the foundation — unaffected by this gap |

## Follow-up tickets

1. **(P1) Port SWCLK keep-alive to Zephyr port.** Either via a PIO0 state machine (matching the dropped pico-sdk impl) or via Zephyr GPIO toggling from a high-priority work item.
2. **(P2) Investigate Zephyr CMSIS-DAP WAIT-ACK retry budget.** Confirm what value DAP_TransferConfigure ends up setting on the wire, and whether `wait_retry=65535` from pyocd's max actually translates to retries in our probe firmware.
3. **(P3) Capture J-Link wire trace** (the user has J-Link Pro) at the wedge state and diff against ours to identify any specific SWJ_Sequence pattern J-Link uses that we don't.

## Progress this session (2026-06-29 — task #18/#19/#20/#21)

**#18 audit:** DAP_SwjSequence handler is correct. Real bottleneck on wedged nRF5340 is the WAIT-ACK retry loop emitting thousands of DBG log lines that flood the cdc_acm console + log subsystem — easily exceeds probe-rs's 1-second per-USB-command timeout. Fix: keep DAP_LOG_LEVEL at INF in prj.conf. Additional finding: Zephyr's `drivers/dp/swdp_ll_pin.h` falls through to slow stubs (`FAST_BITBANG_HW_SUPPORT=0`) on RP2350 because no SoC-specific impl existed. Patched upstream with `swdp_ll_pin_rpi_pico.h` (direct SIO MMIO writes, FAST=1) — net improvement for ALL Zephyr-based RP2350 debug probe work, not just edevkit.

**#19 SWCLK keep-alive:** Implemented as a k_timer at 5 kHz generating one isolated SWCLK pulse per fire (in `src/swclk_keepalive.c`). Live test on wedged nRF5340: removes the USB timeout (probe-rs now gets a proper response) BUT each async pulse shifts the chip's SWD frame counter → subsequent DPIDR reads land at the wrong bit offset = "Target did not respond". **An async timer cannot fix this without breaking protocol.** The real fix is **in-protocol idle cycles inserted between WAIT-ACK retries inside Zephyr's `drivers/dp/swdp_bitbang.c::sw_transfer`** — i.e. after the WAIT-FAULT exit path, drive a few SWCLK cycles with SWDIO=0 (legal SWD idle, no protocol disruption) before returning so the next retry sees the chip still clocked. Requires patching upstream Zephyr. Scaffolding for the timer is left in place but the `keepalive_active` atomic stays 0; `edev_swclk_keepalive_resume()` is the hook to flip it on once the in-protocol path is built.

**#20 retry budget:** Confirmed `do_swdp_transfer` honors `retry_count` (defaults to 100, set by probe-rs via DAP_TransferConfigure). At 200 kHz, 100 retries × ~250 µs ≈ 25 ms per Transfer — well inside the USB timeout. Not the bottleneck. No tuning needed.

**#21 TARGETSEL bits:** Audited the DAP_SwjSequence handler + sw_output_sequence — byte-accurate transmission, no truncation. probe-rs's multidrop alert pattern is being sent correctly. Failure is on the chip's response side, not our transmit side.

**Net status:** nRF5340 wedge recovery still requires J-Link. Path to fixing it on edev_dapv2 is: upstream-patch Zephyr's `swdp_bitbang.c` to add in-protocol idle cycles in the WAIT-ACK retry path, then call `edev_swclk_keepalive_resume()` from main. Multi-day work, blocks on upstream review or carrying a local patch.
