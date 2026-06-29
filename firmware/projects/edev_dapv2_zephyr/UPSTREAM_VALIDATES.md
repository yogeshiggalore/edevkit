# Critical narrowing 2026-06-29 (round 3)

User asked "is cmsis dap working in zephyr?" — prompted a check that should
have happened earlier. Findings:

## What the Zephyr DAP stack actually is

- `menuconfig DAP` in `subsys/dap/Kconfig` is marked `[EXPERIMENTAL]` and
  `select EXPERIMENTAL`.
- `samples/subsys/dap/sample.yaml` lists `build_only: true` — Zephyr CI
  compiles it, never runs it on hardware.
- Supported sample boards: `nrf52840dk/nrf52840`, `frdm_k64f`,
  `nucleo_c071rb`, `rpi_debug_probe` (RP2040). **RP2350 is not in the list.**
- Stack added in October 2019; most recent meaningful change April 2023.
- README mentions pyOCD working against nRF52840 — so someone got it
  working at least once.

## A/B test executed on our hardware

Built the upstream sample as-is for `rpi_pico2/rp2350a/m33` with an added
board overlay using `zephyr,swdp-gpio` on GP2 (SWCLK) / GP3 (SWDIO).
Flashed via the 1200-baud BOOTSEL trick.

Result: **probe-rs reads the nRF52840 successfully** — full ROM walk,
Cortex-M4 ETM, Nordic VLSI ASA designer, byte-identical to the pico-sdk
firmware's output.

## What this means

| Layer | Status on RP2350 |
|---|---|
| Zephyr `subsys/dap/cmsis_dap.c` (dispatcher) | ✅ works |
| Zephyr `drivers/dp/swdp_bitbang.c` (GPIO bit-banger) | ✅ works |
| Our `drivers/dp/swdp_pio_rpi_pico.c` (PIO bit-banger) | ❌ FAULT |

The bug is isolated. The framework and the GPIO-bitbang reference are
both healthy on RP2350. Our PIO driver is the only broken piece.

## Practical implications

1. **A shippable Zephyr port exists today** — swap `compatible` to
   `zephyr,swdp-gpio` in the overlay and the M5 hardware smoke test
   passes. Slower SWD speed (a few MHz vs our 25 MHz target) but
   functionally complete: identify, flash, OTA, BLE-ready.
2. **The PIO performance win is the only remaining gap.** Worth fixing
   for the 16-second 1 MB nRF52840 read use case, but not on the
   critical path for the BLE roadmap.

## Debug avenues for the PIO driver (next session)

Now that we know the framework is solid, the focus narrows sharply:

1. Diff our `sm_configure` against pico-sdk's `probe_pio_configure`
   line-by-line. Particularly the `sm_config_set_*_pins` calls and the
   `pio_sm_init` invocation.
2. Capture SWCLK + SWDIO on a logic analyser during a Zephyr-PIO attempt
   vs the upstream GPIO bit-bang attempt — diff the actual waveforms.
3. Bypass `pio_rpi_pico_allocate_sm` and use raw `pio_claim_unused_sm` —
   in case Zephyr's wrapper sets state pico-sdk doesn't.
4. The opening sequence of the PIO program after `pio_sm_init`: does
   `pull block` block immediately, or does it execute a phantom
   instruction first? `pio_sm_exec(pio_encode_jmp(initial_pc))` is what
   the SDK does, but in Zephyr that may interact with prior state.
