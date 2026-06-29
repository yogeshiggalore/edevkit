# Vendored Zephyr patches for edev_dapv2_zephyr

These patches add bits that upstream Zephyr is missing (or that we haven't
upstreamed yet) but that the edev_dapv2 Zephyr port depends on. Run
`./apply.sh` from this directory against a Zephyr checkout to apply them.

## What's in here

| File | What it does | Upstream status |
|---|---|---|
| `0001-swdp_ll_pin-add-RP2040-RP2350-elif.patch` | Adds an `#elif` for `CONFIG_SOC_SERIES_RP2040`/`RP2350` in `drivers/dp/swdp_ll_pin.h` so RP2350 pulls in `swdp_ll_pin_rpi_pico.h` instead of falling through to the slow-path stubs. | Not submitted yet |
| `0002-swdp_ll_pin_rpi_pico-fast-SIO-bit-bang.patch` | New file `drivers/dp/swdp_ll_pin_rpi_pico.h` — direct SIO-register pin ops for RP2040/RP2350. `FAST_BITBANG_HW_SUPPORT=1`. | Not submitted yet |

## Why the upstream slow path is broken on RP2350

`drivers/dp/swdp_bitbang.c` selects between two pin-op paths via the
`FAST_BITBANG_HW_SUPPORT` macro that comes from `swdp_ll_pin.h`. Upstream
only defines fast pin ops for nRF52/nRF53 and STM32; every other SoC
falls through to a set of empty stubs with `FAST_BITBANG_HW_SUPPORT=0`.
With that flag off, `sw_transfer` uses Zephyr's `gpio_pin_set_dt` /
`gpio_pin_configure_dt` APIs — tens to hundreds of cycles per pin
operation versus the single-MMIO SIO store the RP2350 actually needs.

In live testing against nRF5340 the slow path empirically blew past
probe-rs's per-DAP-command USB response budget. Patch 0002 closes the
gap: each pin op becomes one store to the RP2350's SIO peripheral
(`sio_hw->gpio_set` etc., from the pico-sdk header that Zephyr already
vendors via `hal_rpi_pico`).

## How to apply

```bash
cd edevkit/firmware/zephyr-patches
./apply.sh                              # uses $ZEPHYR_BASE
# or:
./apply.sh /path/to/zephyrproject/zephyr
```

The script:
- Verifies it's pointed at a Zephyr tree (looks for `Kconfig.zephyr` at root)
- Detects whether each patch is already applied — if so, skips it
  silently so re-runs are safe
- Applies any not-yet-applied patches via `git apply` (works in a clean
  Zephyr git checkout) or `patch -p1` as a fallback

If you have local changes in `drivers/dp/` that conflict, the script
bails before touching anything. Resolve manually with `git apply --3way`
or rebase your changes onto a clean Zephyr first.

## Removing the patches

Once these land in upstream Zephyr (or whenever you rebase to a release
that includes the equivalent), delete this whole directory and remove
the `apply.sh` call from `west_build.sh` / CI. The project depends on
the changes being in the build tree but doesn't reference these files
themselves.
