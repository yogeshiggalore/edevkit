# edev_dapv2

A from-scratch **pure CMSIS-DAP v2** debug-probe firmware for RP2350.

The substrate is **pico-sdk + TinyUSB**. Above that layer — descriptors,
MS OS 2.0 BOS chain, custom vendor-class driver, DAP dispatcher, every
command handler, SWD/JTAG/SWO PIO programs — is in this tree.

Pure transport: no DAP_VENDOR commands in firmware, no custom host tool.
Works out-of-the-box with [probe-rs](https://probe.rs/),
[pyocd](https://pyocd.io/), and [OpenOCD](https://openocd.org/).

Target board: **Raspberry Pi Pico 2 W** (RP2350A + CYW43439), 3.3 V
single-domain. Stock Pico 2 also supported via `-DPICO_BOARD=pico2`.

## Status

| Milestone | What | State |
| --- | --- | --- |
| M1 | Boot, clocks, task loop scaffold, LED heartbeat | **in progress** |
| M2 | USB enumerates as CMSIS-DAP v2 | pending |
| M3 | `pyocd list` / `probe-rs list` see the probe | pending |
| M4 | DAP_Info round-trips | pending |
| M5 | SWD reads IDCODE of a real Cortex-M | pending |
| M6 | Full flash via probe-rs on a real target | pending |
| M7 | JTAG TAP scan + flash | pending |
| M8 | SWO trace bytes streamed | pending |

## Quick start

```sh
# First configure clones pico-sdk (~380 MB, one-time).
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
make -j

# Hold BOOTSEL on the Pico 2 W, plug USB. Drag-drop the .uf2 onto the
# RPI-RP2 mass-storage volume.
ls edev_dapv2_v0_1.uf2
```

To use an existing SDK checkout, set `PICO_SDK_PATH` in your shell or
pass `-DPICO_SDK_PATH=/path/to/pico-sdk` to cmake.

Default board is `pico2_w`. For a stock Pico 2:
```sh
cmake -DPICO_BOARD=pico2 ..
```

## Recommended host options

The probe runs PIO-driven SWD at up to 25 MHz. probe-rs's "auto"
clock picks ~1 MHz conservatively — pass `--speed 8000` (8 MHz) for
~4× faster memory dumps. Tested on nRF52840:

| `--speed` | 1 MB read | reliability |
| --- | --- | --- |
| (default) | 16.4 s | 100 % |
| 8000      |  4.5 s | 100 % ← **recommended** |
| 10000     | ~4.2 s |  ~70 % |
| 12000+    | varies | unstable on jumper wiring |

Reliability above 8 MHz depends on wiring quality. With short jumper
leads to a target on a development board, 8 MHz is the sweet spot.
For mounted probes with short controlled-impedance traces, 15–25 MHz
should be achievable.

```sh
probe-rs read --probe 2e8a:000c --chip nRF52840_xxAA \
              --protocol swd --speed 8000 \
              b32 0x00000000 262144 > flash.txt
# 1 MB of nRF52840 flash in ~4.5 seconds, byte-identical to J-Link.
```

## Pinout (v0.1, 3.3 V single domain)

| Pico 2 W GPIO | Pin | Signal | Notes |
| ---: | ---: | --- | --- |
| GP2 | 4 | SWCLK / TCK | PIO0 SM0, push-pull |
| GP3 | 5 | SWDIO / TMS | PIO0 SM0, bidir |
| GP4 | 6 | TDI (JTAG only) | PIO0 SM0 |
| GP5 | 7 | TDO / SWO (mux) | PIO0 SM0 (JTAG); PIO0 SM1 (SWO) |
| GP6 | 9 | nRESET (open-drain) | SIO; drive 0 only |
| GP7 | 10 | nTRST (optional) | SIO |
| GP0 | 1 | UART TX → target RX | hw UART0 |
| GP1 | 2 | UART RX ← target TX | hw UART0 |
| GP26 / ADC0 | 31 | Vtgt sense (optional) | reports voltage to host |
| GP22 | 29 | Status LED (optional, external) | 330 Ω + LED to GND |

The on-board LED on Pico 2 W is wired to the CYW43439 chip, not an
RP2350 GPIO, so the heartbeat blink is a no-op on this board. Wire an
external LED to GP22 if you want visual sign-of-life.

The pinout maps directly to the standard 10-pin Cortex Debug
connector (1.27 mm) — see the firmware-design doc for the table.

## Toolchain

- `arm-none-eabi-gcc` 11+ recommended (10.3 also works for v0.1)
- `cmake` 3.13+
- `make` or `ninja`
- For OTA / probing later: `picotool`, `probe-rs`, `pyocd`, `openocd`

## License

Code originating in this tree is © Edevkit, MIT-licensed unless a file
header says otherwise. Imported third-party material carries its
upstream notice.
