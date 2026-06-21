# edev_dapv2 releases

Each successful CMake build drops a copy of the firmware here, named
with the version tag baked into `CMakeLists.txt`. This is the
human-friendly location for "what's the current release artifact" —
the canonical build output still lives under `build/`.

## Versioning convention

| Bump kind | What changes |
| --- | --- |
| **patch** (`0.1.0` → `0.1.1`) | bug fix, behaviour identical to host; same UF2 filename `edev_dapv2_v0_1.uf2` overwrites in this directory |
| **minor** (`0.1.x` → `0.2.0`) | new features, possibly new DAP commands or vendor extensions; new file `edev_dapv2_v0_2.uf2` is added |
| **major** (`0.x.y` → `1.0.0`) | hardware-incompatible or first stable release; new file `edev_dapv2_v1_0.uf2` |

The filename carries `vMAJOR_MINOR` only. The patch level is inside
the firmware — query it via `DAP_Info(0x09)` "Product Firmware
Version" or in the web tester's **Probe info** panel.

## Bumping the version

Edit the top of `CMakeLists.txt`:

```cmake
set(EDEV_DAPV2_VERSION_MAJOR 0)
set(EDEV_DAPV2_VERSION_MINOR 1)
set(EDEV_DAPV2_VERSION_PATCH 0)
```

Then `make` — the post-build step copies the new artifact here.

## Changelog

| Version | Date | Notes |
| --- | --- | --- |
| `v0.1.0` | 2026-06-18 | First milestone build. SWD + JTAG (SIO bit-bang) + SWO (PIO UART RX). USB enumeration verified on Pico 2. probe-rs and openocd both initialise the probe and run the full DAP handshake successfully (`DAP_Info`, `DAP_Connect`, `DAP_SWJ_Clock`, `DAP_SWJ_Pins` readback shows correct idle pin states, `DAP_SWJ_Sequence`, `DAP_Transfer` correctly reports SWD protocol error when no target wired). End-to-end IDCODE read with a real Cortex-M target still pending hardware wiring. |
