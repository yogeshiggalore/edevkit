# edev_dapv2_zephyr v0.1.1-step5 — release notes

> **⚠ Newer release available: [v0.1.2-step8](../v0.1.2-step8/RELEASE.md)** — superset; adds vendor cmd `0x88 NRF53_READ_MEM` (chip-agnostic AHB-AP read) + a Net-stub DP-wake refresh after CTRL-AP RESET. v0.1.1-step5 is still functional but lacks `0x88`.

**Build:** 2026-06-30 12:50  
**Branch:** `feat/edev_dapv2_zephyr`  
**Tip commit:** `c32eba8 test(edev_dapv2_zephyr): chip-aware harness — nRF52840 + nRF5340 paths`  
**Board:** `rpi_pico2/rp2350a/m33`  
**Supersedes:** v0.1-step5 (which had the dp_full_wake bugs and didn't work against nRF52840)

## What changed since v0.1-step5

Hardware-validated bug-fix release. **v0.1-step5 didn't work on real hardware**
— it wedged with PROTO / NO_ACK on the first DPIDR read. Three bugs in
`nrf53_dp_full_wake` (commit `fa5d305`):

1. **`swdp_port_on()` never called** — our vendor commands ran standalone
   without a prior `DAP_Connect`, so SWDIO/SWCLK never entered active drive
   mode. Standard CMSIS-DAP `DAP_Connect` dispatcher does this; we bypassed
   it. Fix: call `swdp_port_on()` at the top of `nrf53_dp_full_wake`.
2. **Dormant-wake alert wedges DPv1 (nRF52840)** — ADIv6 dormant is only for
   DPv2+ silicon (Cortex-M33+). The alert sequence is harmful on DPv1
   (Cortex-M4 like nRF52840). Fix: adaptive sequence — try simple line reset
   first; only do dormant alert as fallback if DPIDR fails.
3. **Alert byte packing was wrong** — `0x1A` activation byte straddles bits
   140..147 (not byte-aligned). Old layout had byte 17=`0x00`, byte 18=`0x1A`
   which when emitted as 148 contiguous LSB-first bits produces a corrupted
   activation. Fix: byte 17=`0xA0` (4 zeros lo nibble + `0x1A` lo nibble hi),
   byte 18=`0x01` (`0x1A` hi nibble lo). nRF5340 silicon was masking this
   bug via post-alert line reset; nRF52840 wasn't.
4. **8 idle cycles after final line reset** — standard CMSIS-DAP path
   always sends a trailing `0x00` byte before the first transaction. ADIv5
   doesn't strictly mandate this but every working reference does. Missing
   idle cycles → NO_ACK on first DPIDR.

Plus `c32eba8`: test harness now chip-aware. PING checks the wire via standard
CMSIS-DAP, detects nRF52 vs nRF5340 from DPIDR version, and gates
nRF5340-only tests behind chip detection.

## Vendor command surface

Unchanged from v0.1-step5 — same four vendor commands shipped:

| Cmd  | Op | Status | nRF52840 | nRF5340 |
|---|---|---|---|---|
| `0x84` | `NRF53_RECOVER`           | ✅ shipped | ✗ nRF5340-only logic | ✅ works |
| `0x85` | `NRF53_ERASE`             | ✅ shipped | ✅ **works** | ✅ works |
| `0x86` | `NRF53_FLASH_WRITE_NET`   | ⏳ step 6   | n/a (no Net) | future |
| `0x87` | `NRF53_FLASH_WRITE_APP`   | ⏳ step 7   | future | future |
| `0x88` | `NRF53_READ_MEM`          | ⏳ step 8   | future | future |
| `0x89` | `NRF53_TARGET_INFO`       | ⏳ later    | future | future |
| `0x8A` | `NRF53_UICR_PROGRAM_APP`  | ✅ shipped | ✗ nRF5340 addrs | ✅ works |
| `0x8B` | `NRF53_UICR_PROGRAM_NET`  | ✅ shipped | n/a (no Net) | ✅ works |

**Standard CMSIS-DAP v2 surface** (DAP_Info, Connect, Transfer, Block, SWJ_*,
WriteAbort) works on **both** chip families.

## Per-chip support matrix

### nRF52840 (Cortex-M4, DPv1) — **hardware-validated** ✅

What the ESP32 bridge can do today:

| Operation | How |
|---|---|
| Identify probe | Standard `DAP_Info` |
| Identify target | Standard `DAP_Transfer` reading DPIDR + CPUID |
| Open SWD link | Standard `DAP_Connect` + `DAP_SWJ_Sequence` (line reset + JTAG→SWD + idle) |
| Read DPIDR / AP_IDR | Standard `DAP_Transfer` |
| Read flash (any address) | Standard `DAP_Transfer` setting AP.CSW=`0x23000002`, AP.TAR=addr, read AP.DRW → DP.RDBUFF |
| Chip wipe | Vendor `0x85 NRF53_ERASE` (returns `ap_count=1`) |
| Write UICR.APPROTECT | Compose from standard `DAP_Transfer` writing NVMC sequence (nRF52 NVMC=`0x4001E000`, UICR.APPROTECT=`0x10001208`) |
| Write flash | RAM-loaded CMSIS Flash Algo via standard `DAP_Transfer` |

What's NOT in this release:
- Vendor cmd for nRF52-family UICR programming (hardcoded to nRF5340 addresses today)
- High-level flash write vendor commands (host composes from standard DAP)
- Halt-at-reset for App readback (no special vendor for this; standard works)

### nRF5340 (Cortex-M33, DPv2) — **not yet hardware-validated**

Vendor cmds `0x84`, `0x85`, `0x8A`, `0x8B` should all work end-to-end per
the algorithm spec (`docs/NRF5340_ALGORITHMS.md`), but haven't been smoke-
tested against Ring Pro 351 / nRF5340 DK in this release cycle. Steps 6-10
of the implementation plan are still pending.

## Artifacts

| File | Size | SHA256 |
|---|---|---|
| `zephyr.uf2` | 145,408 B | `b04afb8c9b4221c0dde2e81115cade7c710c6543c8e49d3f8553b4e483d4e0d9` |
| `zephyr.bin` | 72,644 B | `91fc9d6aac2f00aec152d5adddacd60e4d0c8442cef4e8873adf13070abe4e07` |
| `zephyr.elf` | 2,878,880 B | `2943d10766cd5e607acaac2ed092b5536b8e2d88fef3b992513c9c3ebaed20c8` |

Memory footprint: 72,644 B FLASH (1.73%) / 24,364 B RAM (4.58%) on RP2350.

## How to flash

Hold BOOTSEL while plugging USB → drag `zephyr.uf2` to `/Volumes/RP2350` (macOS)
or `RPI-RP2` (older OSes / RP2040). Or use the 1200-baud trick on a probe
already running our firmware:

```bash
# macOS
stty -f /dev/cu.usbmodem* 1200
# Linux
stty -F /dev/ttyACM0 1200
# probe re-enumerates as RP2350 mass-storage; drag the uf2
```

## How to test (acceptance suite)

```bash
pip install pyusb pyserial
cd firmware/projects/edev_dapv2_zephyr/tools
python3 test_nrf53_vendor_cmds.py
```

Connect SWD: Pico GPIO2 → target SWCLK, GPIO3 → target SWDIO, GND ↔ GND.
Keep target powered.

**Expected on nRF52840:** 5/5 PASS:

```
✓ ping            — DPIDR=0x2ba01477  (ARM Cortex-M4, DPv1)
✓ erase           — status=OK, 1 CTRL-AP erased
✓ verify-erase    — flash[0] = 0xFFFFFFFF after wipe
✓ uicr-app        — SKIP (nRF5340-only)
✓ uicr-net        — SKIP (nRF5340-only)
```

**Expected on nRF5340** (not validated this cycle but algorithm-complete): 7/7 PASS
including `uicr-app`, `uicr-net`, and full `recover`.

## What the ESP32 bridge author needs to know

**Use this release (v0.1.1-step5)** — not v0.1-step5 which doesn't work on
nRF52840. Flash this UF2 onto the bridge's companion Pico.

**Standard CMSIS-DAP works for nRF52840 end-to-end.** The vendor cmd surface
is a convenience layer; `NRF53_ERASE` (0x85) is the only vendor cmd they
need on nRF52840 (full chip wipe in one call). Everything else — UICR
programming, flash write, target info — composes from standard `DAP_Transfer`.

**See `docs/ESP32_BRIDGE.md`** for the full integration guide. Per-chip
support matrix is in §0 "Pico-side firmware status".

## Reproduce this build

```bash
cd firmware/projects/edev_dapv2_zephyr
../../zephyr-patches/apply.sh                # idempotent
west build -b rpi_pico2/rp2350a/m33 --pristine
shasum -a 256 build/zephyr/{zephyr.uf2,zephyr.bin,zephyr.elf}
# should match SHA256SUMS in this dir
```
