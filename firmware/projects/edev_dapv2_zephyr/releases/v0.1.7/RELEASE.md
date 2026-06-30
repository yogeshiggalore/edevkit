# edev_dapv2_zephyr v0.1.7 — release notes

> **⚠ Superseded by [v0.1.8](../v0.1.8/RELEASE.md)** — docs + tools
> only; firmware binaries are byte-identical to this release. Adds
> ESP32_BRIDGE.md Appendix L (timing budgets), Appendix M (Ring
> acceptance procedure), the Net-flash @ 0x01000000 gotcha note, and
> bench tools `ring_pro_351_acceptance.py` + `_flash_ops_sequence.py`.
> **You do not need to re-flash a v0.1.7 probe to get v0.1.8.**

**Build:** 2026-06-30 18:00
**Branch:** `feat/edev_dapv2_zephyr`
**Board:** `rpi_pico2/rp2350a/m33`
**Supersedes:** v0.1.6-step7

## What's new — `NRF53_TARGET_INFO (0x89)` — full vendor command suite complete

Adds the last planned vendor command. Returns DPIDR + AP[0].IDR +
CPUID in a single 14-byte response — replaces three separate
`DAP_Transfer` / `READ_MEM` round-trips with one.

**Wire format:**

```
Request:  [0x89]                       (1 byte, no payload)
Response: [0x89, status,
           u32_le dpidr,                — DP.DPIDR
           u32_le ap0_idr,              — AHB-AP[0].IDR
           u32_le cpuid]                — Cortex-M CPUID at 0xE000ED00
          = 14 bytes
```

**What the bridge derives from the three fields:**
- **Family** from `DPIDR.VERSION` (bits 12..15): `1 = nRF52` (DPv1),
  `2 = nRF5340` (DPv2)
- **Vendor** from `DPIDR.DESIGNER` (bits 1..11): `0x23B = ARM`,
  `0x144 = Nordic`
- **ARM core** from `CPUID.PART` (bits 4..15): `0xC24 = Cortex-M4`,
  `0xD21 = Cortex-M33`
- **AHB-AP variant** from `AP_IDR` — tells the bridge whether the
  standard ARM AHB-AP (`0x84770001` on nRF5340, `0x24770011` on
  nRF52840) is present and what revision.

**Not in the response: FICR (PART, VARIANT, DEVICEID).** Bench
finding 2026-06-30 EOD: on nRF5340 DK silicon, FICR.INFO.PART at the
documented offset (`+0x140`) returns `0xFFFFFFFF` — either the chip
is an engineering sample with unprogrammed FICR or the documented
offset is wrong for this variant. The bridge can compose its own
FICR reads via `0x88 NRF53_READ_MEM` at chip-specific addresses
after inferring family from this call.

## Vendor command surface (v0.1.7) — complete

| Cmd | Op | nRF52840 | nRF5340 |
|---|---|---|---|
| `0x84` | `NRF53_RECOVER` (full unlock) | n/a (use `0x85`) | ✅ HW |
| `0x85` | `NRF53_ERASE` (CTRL-AP ERASEALL) | ✅ HW | ✅ HW |
| `0x86` | `NRF53_FLASH_WRITE_NET` | n/a | ✅ HW |
| `0x87` | `NRF53_FLASH_WRITE_APP` | algo | ✅ HW |
| `0x88` | `NRF53_READ_MEM` (chip-agnostic) | ✅ HW | ✅ HW |
| **`0x89`** | **`NRF53_TARGET_INFO`** | algo | ✅ **HW (NEW)** |
| `0x8A` | `NRF53_UICR_PROGRAM_APP` | n/a (nRF52 UICR differs) | ✅ HW |
| `0x8B` | `NRF53_UICR_PROGRAM_NET` (on-target stub) | n/a | ✅ HW |
| `0x8C` | `NRF53_WRITE_MEM` (chip-agnostic) | ✅ HW | ✅ HW |

Every planned vendor command in the original 10-step plan is now
shipped + bench-validated on nRF5340 DK. nRF52840-side validation
shipped earlier in v0.1.1; subsequent point releases retain
nRF52840 compatibility (same code paths for `0x85`, `0x87`, `0x88`,
`0x8C`, `0x89`).

## Hardware acceptance — **11/11 PASS** on bench nRF5340 DK

```
✓ ping             — DPIDR=0x6ba02477 (Cortex-M33, DPv2)
✓ target-info      — DPIDR + AP_IDR + CPUID in one packet (NEW)
✓ erase            — 2 CTRL-APs (App + Net)
✓ verify-erase     — flash[0] = 0xFFFFFFFF
✓ mem-roundtrip    — WRITE_MEM + READ_MEM bit-perfect
✓ flash-write-app  — 13-word pattern → bit-perfect readback
✓ uicr-app         — App APPROTECT/SECUREAPPROTECT = 0x50FA50FA
✓ uicr-net         — Net UICR.APPROTECT = 0x50FA50FA
✓ flash-write-net  — 14-word pattern → bit-perfect readback
✓ erase-pre-recover
✓ recover          — full RECOVER end-to-end (App + Net UICR)
```

## Status of the original 10-step plan

```
1-8 ✅ ALL SHIPPED + bench-validated against nRF5340
  1. DP/AP primitives
  2. CTRL-AP ERASE (0x85)
  3. App UICR programming (0x8A)
  4. Net stub + UICR programming (0x8B)
  5. RECOVER (0x84) — full unlock composition
  6. FLASH_WRITE_NET (0x86)
  7. FLASH_WRITE_APP (0x87)
  8. READ_MEM (0x88) + WRITE_MEM (0x8C)

Plus this release:
  TARGET_INFO (0x89) — one-call ARM identification

9.  ⏳ Progress packets (mostly ESP32-side; not a probe-side concern)
10. ⏳ Ring Pro 351 acceptance (needs Ring hw on bench)
```

The probe firmware is **feature-complete** for everything in the
original Pico-side plan. Remaining items 9 and 10 are either ESP32-
side (BLE progress notifications during long ops) or external
hardware validation against a specific Nordic product.

## Artifacts

| File | Size | SHA256 |
|---|---|---|
| `zephyr.uf2` | 152,064 B | `f0af00aa57d6f59b290bb58465f19428058b5336cab2aaeff7979442a1e446b7` |
| `zephyr.bin` | 75,580 B | `d86b0753c54ea4884327cd5f557c0f33f3938694a19720333c52f4ab8ee9fec1` |
| `zephyr.elf` | 2,901,096 B | `79cb9575df5cf80fa83c05beba0bb0999c5b77eba9da3d6a0b34643058f1c734` |

Memory footprint: 75,580 B FLASH (1.80%) / 24,364 B RAM (4.58%).

## Reproduce

```bash
cd firmware/projects/edev_dapv2_zephyr
../../zephyr-patches/apply.sh
west build -b rpi_pico2/rp2350a/m33 --pristine
shasum -a 256 build/zephyr/{zephyr.uf2,zephyr.bin,zephyr.elf}
```
