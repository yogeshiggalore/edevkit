# Smoke test 2026-06-29 — partial pass

First-power-on of the Zephyr port on a Pico 2 W (same board the pico-sdk firmware ran on), wired to an nRF52840 target on GPIO2 (SWCLK) + GPIO3 (SWDIO) + GND.

## What works ✅

| Check | Result |
|---|---|
| Build | clean (FLASH 65.6 KB, RAM 23 KB, UF2 132 KB) |
| BOOTSEL flash via UF2 drag-drop | ok |
| USB enumeration | ok |
| `probe-rs list` lists the probe | **`edev_dapv2 (Zephyr) -- 2e8a:000c-2:FA6418C89B6A7B39 (CMSIS-DAP)`** |
| Product / manufacturer / serial strings | "edev_dapv2 (Zephyr)" / "Edevkit" / hex chip-id from hwinfo |
| bcdDevice ≥ 0x0220 gate | passed (probe-rs recognises us as CMSIS-DAP) |
| MS OS 2.0 BOS chain | linked (standard CMSIS-DAP v2 GUID); no host-side check done |
| CDC ACM console (`/dev/cu.usbmodem*`) | banner + boot logs come through |
| `DAP_Connect(SWD)` | ok — handler runs, "port swd" + "Debugger connected" in CDC log |
| PIO clock setup | ok — `sys=150 MHz, swd=1 MHz, div=25.000` |
| SWD bit-bang at wire level | running — PIO clocks, target responds |

## What fails 🟡

| Symptom | Decoded |
|---|---|
| `probe-rs info --chip nRF52840_xxAA` returns "Target device did not respond to request" | DPIDR transactions return ACK=FAULT, follow-up CTRL/STAT returns ACK=0 |

CDC log during the failure:

```
xfer req=0x02 hdr=0xa5 ack=0x4   ← DPIDR read → FAULT
xfer req=0x06 hdr=0x8d ack=0x0   ← CTRL/STAT read → line silent
xfer req=0x02 hdr=0xa5 ack=0x4   ← retry → FAULT again
xfer req=0x06 hdr=0x8d ack=0x0   ← retry CTRL/STAT → still silent
```

The PIO driver receives a real ACK value of 0x4 (FAULT in wire-order bits 0,0,1) on the first transaction, then nothing on the next. The wire IS clocking — we know because we decode a 3-bit pattern correctly the first time.

Pico-sdk firmware on the **same hardware** reads the nRF52840 cleanly (`probe-rs info` returns the full ROM walk including the Nordic ETM). So the wiring + target are fine.

## A/B test result (added 2026-06-29 after first round)

**Definitive**: the bug is in the Zephyr port, not in stickiness recovery. Sequence:

1. Reverted Pico 2 W to Friday's pico-sdk firmware (`build/edev_dapv2_v0_1.uf2`, 2026-06-26)
2. Ran `probe-rs info` against the same nRF52840 on the same wires → full ROM walk including Cortex-M4 ETM, Nordic VLSI ASA designer, byte-identical to original baseline. **Target DP is clean and pico-sdk reads it fine.**
3. Immediately re-flashed the diagnostic Zephyr build (no power-cycle of the target between firmwares — DP state preserved as left by pico-sdk's successful disconnect)
4. Ran `probe-rs info` again → **identical failure mode**: first DPIDR returns ACK=4 (FAULT), follow-up CTRL/STAT returns ACK=0.

So a known-clean DP still triggers FAULT under our Zephyr firmware. Hypothesis #1 (stale sticky) is **ruled out**. The bug is in our PIO driver or in Zephyr's `cmsis_dap.c` command-emission pattern.

## Hypotheses, ranked (revised)

1. **Line reset isn't actually clocking 51+ HIGH bits.** If our `api_output_sequence` for SWJ_Sequence doesn't send all 51 bits HIGH (e.g., bit ordering bug, opcode-vs-data race in PIO autopull, swd_write_n call shape), the target's DP state machine never resets and DPIDR returns FAULT. **First thing to verify next session.** Add LOG to `api_output_sequence` to dump count + first word.
2. **PIO is being torn down + re-set-up between Connect and the first transaction**, losing carefully prepared state. We see two `sm_configure` log lines back-to-back at Connect time. Each calls `pio_sm_clear_fifos` and `pio_sm_init`. The second one (from `swdp_set_clock`) might be re-running while PIO is mid-line-reset.
3. **Inter-opcode gap on the SWD line is too long.** Between `swd_write_n` for the header and `swd_read_n` for ACK, there's a ~0.8 us gap (5 PIO cycles for `pull block` → `out pc` → `out x` → `set pindirs`). At 1 MHz SWD, that's almost a whole SWD cycle of SWCLK held low while neither host nor target drives SWDIO. Some targets may interpret the gap as a phase-state reset.
4. **Force `data_phase=1` always** — one-line test in `api_configure` to set `d->data_phase = 1;` unconditionally. Worth trying as a 5-second experiment.

## Next session — debug plan (revised after the A/B confirms the bug is ours)

1. Add `LOG_INF` in `api_output_sequence` dumping `{count, data[0]}` — confirms whether probe-rs actually issues the 51-bit line-reset and what the first byte looks like. The fastest diagnostic.
2. Delete or skip one of the two back-to-back `sm_configure` calls at Connect time — `api_set_clock` should only update `d->clock_hz` and defer reconfigure to next port_on. Removes a known source of PIO state churn before any wire activity.
3. One-line test: set `d->data_phase = 1` unconditionally in `api_configure`. Reflash, re-run. If it works, bug is FAULT data-phase handling.
4. If all the above fail: USB-capture both pico-sdk and Zephyr against the same target and diff the DAP command sequences byte-by-byte.

## Repro

```bash
cd firmware/projects/edev_dapv2_zephyr
# build artefact already at build/zephyr/zephyr.uf2 from this session's commit
# Pico 2 W needs physical BOOTSEL hold + replug to enter mass-storage mode —
# 1200-baud CDC trick is not ported to Zephyr yet.
cp build/zephyr/zephyr.uf2 /Volumes/RP2350/
sleep 5
probe-rs list                                                 # should list "edev_dapv2 (Zephyr)"
timeout 10 cat /dev/cu.usbmodem* > /tmp/zephyr_log.txt &
sleep 1
probe-rs info --chip nRF52840_xxAA --probe 2e8a:000c
```

Diagnostic build (with `LOG_INF` in `sm_configure` + `api_transfer`) is committed as the M5 work-in-progress state — strip the LOG_INFs before merging.
