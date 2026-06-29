# edev_dapv2 — Zephyr port

CMSIS-DAP v2 debug probe firmware on **Zephyr 4.4.1**, targeting Raspberry Pi Pico 2 (RP2350A, Cortex-M33).

This is a deliberate port of [`firmware/projects/edev_dapv2/`](../edev_dapv2/) (pico-sdk, TinyUSB) to Zephyr. The motivation is preparing the codebase to host a Zephyr Bluetooth stack for the BLE-companion roadmap (design doc §18) — `BTstack` on pico-sdk has license issues that block long-term use.

## Why not the upstream Zephyr DAP sample?

Zephyr ships a CMSIS-DAP v2 implementation under [`subsys/dap/`](../../../../zephyrproject/zephyr/subsys/dap/) with a sample at [`samples/subsys/dap/`](../../../../zephyrproject/zephyr/samples/subsys/dap/). It uses [`drivers/dp/swdp_bitbang.c`](../../../../zephyrproject/zephyr/drivers/dp/swdp_bitbang.c) — a GPIO bit-banger.

The pico-sdk implementation we already shipped uses a **PIO-driven** SWD bit-banger (`src/pio/probe_swd.pio`) that achieves 25 MHz SWCLK and reads a full 1 MB nRF52840 in 16 s — byte-identical to a J-Link. The upstream Zephyr bit-banger cannot match this. So the port architecture is:

- Use Zephyr's `subsys/dap/cmsis_dap.c` for the command dispatcher.
- Implement a **new SWDP driver** (`swdp_pio_rpi_pico`) that satisfies the `swdp_api` interface (`include/zephyr/drivers/swdp.h`) using our PIO program underneath.
- Customise USB descriptors for our VID/PID/MS OS 2.0 BOS so probe-rs/pyocd/probe-rs-tools see the same probe identity as before.

## Milestones

| # | Goal | State |
|---|------|-------|
| **M1** | Board boots; USB CDC ACM console works; LED heartbeat at 1 Hz | ✅ builds, untested on HW |
| M2 | DAP subsystem enabled with upstream `swdp_bitbang` placeholder; probe-rs lists the device | pending |
| M3 | `swdp_pio_rpi_pico` driver shipped; SWD bit-bang via PIO | pending |
| M4 | Diff vs pico-sdk dispatcher — patch in posted-AP-read pipeline + SWCLK keep-alive | pending |
| M5 | Validate end-to-end against locked nRF5340 (Ring Pro) | pending |

## Build

```bash
# from this directory
export ZEPHYR_BASE=/Users/yogesh/projects/temp/zephyrproject/zephyr
export ZEPHYR_SDK_INSTALL_DIR=$HOME/zephyr-sdk-1.0.1
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr

west build -b rpi_pico2/rp2350a/m33 --pristine
```

Output:
- `build/zephyr/zephyr.uf2` — drag-drop into BOOTSEL mass-storage to flash.
- `build/zephyr/zephyr.elf` — for `probe-rs run` / debugger attach.

## Flash

```bash
# Hold BOOTSEL on the Pico 2, plug USB, then:
cp build/zephyr/zephyr.uf2 /Volumes/RP2350/
# board reboots into Zephyr automatically
```
