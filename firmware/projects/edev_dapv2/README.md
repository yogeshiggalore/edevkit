# edev_dapv2

A from-scratch CMSIS-DAP v2 debug-probe firmware for RP2350. Substrate is
**pico-sdk + TinyUSB**; everything above that layer — descriptors, MS OS
2.0 BOS chain, vendor-class driver, DAP dispatcher, every command handler,
SWD/JTAG/SWO PIO programs, ring buffers — is in this tree.

Target hosts:
[probe-rs](https://probe.rs/),
[pyocd](https://pyocd.io/),
[OpenOCD](https://openocd.org/).
Target board for v0.1: a stock Raspberry Pi Pico 2 or a Seeed XIAO RP2350,
3.3 V single-domain.

For why each design choice was made and what the source tree looks like,
see [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

For step-by-step bring-up on a fresh board, see
[`docs/BRINGUP.md`](docs/BRINGUP.md) once it exists.

## Quick start

```sh
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPICO_BOARD=pico2 ..
make -j
# hold BOOTSEL on the Pico 2, plug USB, drag-drop edev_dapv2.uf2 onto the
# RP2350 BOOTSEL mass-storage volume

pyocd list           # should see "edev_dapv2 CMSIS-DAP" at 2e8a:000c
probe-rs list        # ditto
```

The `pico-sdk/` directory is a symlink to `../dap_v2/pico-sdk/` to avoid
duplicating the 382 MB SDK checkout. If you build standalone, set
`PICO_SDK_PATH` in the environment or pass `-DPICO_SDK_PATH=…` to cmake.

## Status

| Milestone | State |
| --- | --- |
| M1 — boot, clocks, LED blink | in progress |
| M2 — USB enumerates as CMSIS-DAP v2 | pending |
| M3 — `pyocd list` / `probe-rs list` see the probe | pending |
| M4 — DAP_Info round-trips | pending |
| M5 — SWD reads IDCODE of a real Cortex-M | pending |
| M6 — full flash via probe-rs on a real target | pending |
| M7 — JTAG TAP scan | pending |
| M8 — SWO trace bytes streamed | pending |

## Relation to `dap_v2/`

This is a **fresh start**. The sibling `dap_v2/` directory (pico-sdk +
TinyUSB-based CMSIS-DAP probe, v0.3.3 at the time of this writing) is left
untouched as a known-working baseline to diff against. `edev_dapv2` was
created to answer the question *"if we wrote a CMSIS-DAP v2 probe with
modern eyes, no SDK lock-in, and only the parts the protocol actually
needs, what would it look like?"*

## License

TBD. Code originating in this tree is © Edevkit. The single piece of
imported third-party material is the upstream RP2350 QSPI 2nd-stage
bootloader (`boot/boot2_*.S`), which carries its own BSD-3-Clause notice
from the Raspberry Pi pico-sdk.
