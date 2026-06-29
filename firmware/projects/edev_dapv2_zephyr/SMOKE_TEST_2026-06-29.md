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

## Hypotheses, ranked

1. **Sticky error in target DP from prior session.** Standard recovery: DPIDR FAULT → host reads CTRL/STAT (to learn which sticky bit is set) → host writes DP.ABORT (with sticky-clear bits). probe-rs's CTRL/STAT read fails (ack=0) so it never gets to the ABORT write, and the bus stays stuck. Reset target + re-run = should clear. Try this first next session.
2. **SWDIO turnaround timing slightly off** for *back-to-back* transactions. The first transaction works (FAULT is a valid response). The second one returns ack=0 — line is being held low somewhere. The PIO program holds SWDIO=1 between transactions (via the `set pins, 1` park) but releases on entering `read_bits`. If the target is still in FAULT-recovery (driving 33 dummy bits) we'd be reading those bits, not the new ACK. → try forcing `data_phase=1` to always consume the dummy data.
3. **Specific to Zephyr cmsis_dap.c command ordering.** Zephyr might emit DAP commands in a slightly different sequence than pico-sdk's DAP dispatcher, exposing a wire-state assumption that pico-sdk's dispatcher kept hidden.

Most likely: 2. Pico-sdk's dap_swd.c happened to handle this case because the failing op never made it to `probe_swd_xact` twice in a row before the FAULT was cleared upstream.

## Next session — debugging steps

1. **Power-cycle the nRF52840 target**, then run `probe-rs info` — does the first DPIDR succeed when the DP has no sticky bits? (Tests hypothesis 1.)
2. If still failing: add CDC log of every `api_output_sequence` call (count, first bits) to confirm probe-rs is actually sending the SWD line reset before DPIDR. (Tests whether line reset is being executed.)
3. Force `d->data_phase = 1` unconditionally in `api_configure` and rebuild. If the wire works, the bug is the FAULT data-phase asymmetry handling. (Tests hypothesis 2.)
4. Side-by-side: capture USB packets from `probe-rs info` against pico-sdk firmware vs Zephyr — diff the DAP command sequences. (Tests hypothesis 3.)

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
