# edev_dapv2_zephyr v0.1.6-step7 — release notes

**Build:** 2026-06-30 17:30
**Branch:** `feat/edev_dapv2_zephyr`
**Board:** `rpi_pico2/rp2350a/m33`
**Supersedes:** v0.1.5-step6

## What's new — `NRF53_FLASH_WRITE_APP (0x87)`

Step 7 of the implementation plan ships. App-flash batch programmer
mirroring step 6's `0x86 NRF53_FLASH_WRITE_NET` but for App AHB-AP.
Family-aware via the `nvmc_base` parameter.

**Wire format:**

```
Request (>= 11 bytes):
  [0]    = 0x87
  [1..4] = u32_le nvmc_base   (0x50039000 nRF5340 App, 0x4001E000 nRF52)
  [5..8] = u32_le addr        (App flash address, 4-byte aligned)
  [9..10] = u16_le word_count  (≤ 13 due to 64 B USB-FS packet limit)
  [11..] = data, little-endian 32-bit words

Response (4 bytes):
  [0]   = 0x87 (echo)
  [1]   = nrf53_status_t
  [2..3] = u16_le words_written
```

Hardcoded for App AHB-AP (ap=0, csw=`0x23000002`). Words equal to
`0xFFFFFFFF` are skipped. NVMC.CONFIG is always restored to Ren before
returning.

**Family targets:**
- nRF52840 / 833 / 832 — `nvmc_base = 0x4001E000`, flash at `0x00000000..0x000FFFFF` (1 MB on 52840)
- nRF5340 App — `nvmc_base = 0x50039000`, flash at `0x00000000..0x000FFFFF` (1 MB)

**Hardware-validated on bench nRF5340 DK** (Cortex-M33, DPv2):
13-word pattern → bit-perfect readback. Algorithmically identical for
nRF52840 (same App AHB-AP CSW, same NVMC register layout; only base
differs); nRF52 path will be exercised when the bridge author drives
it through the BLE+ESP32+Pico stack.

## Vendor command surface (v0.1.6-step7) — full set

| Cmd | Op | nRF52840 | nRF5340 |
|---|---|---|---|
| `0x84` | RECOVER (full unlock) | (use `0x85`) | ✅ HW |
| `0x85` | ERASE (CTRL-AP ERASEALL) | ✅ HW | ✅ HW |
| `0x86` | FLASH_WRITE_NET | n/a | ✅ HW |
| **`0x87`** | **FLASH_WRITE_APP** | algo (same path as nRF5340) | ✅ **HW (NEW)** |
| `0x88` | READ_MEM (chip-agnostic) | ✅ HW | ✅ HW |
| `0x8A` | UICR_PROGRAM_APP | n/a (nRF52 UICR differs) | ✅ HW |
| `0x8B` | UICR_PROGRAM_NET | n/a | ✅ HW |
| `0x8C` | WRITE_MEM (chip-agnostic) | ✅ HW | ✅ HW |
| `0x89` | TARGET_INFO | ⏳ pending | ⏳ pending |

`HW` = bench-tested against real silicon. `algo` = code matches the
canonical Python reference for that chip family; runs through the same
code path as the HW-validated case.

## Hardware acceptance — **10/10 PASS** on bench nRF5340 DK

```
✓ ping             — DPIDR=0x6ba02477 (Cortex-M33, DPv2)
✓ erase            — 2 CTRL-APs (App + Net)
✓ verify-erase     — flash[0] = 0xFFFFFFFF
✓ mem-roundtrip    — WRITE_MEM + READ_MEM bit-perfect
✓ flash-write-app  — 13-word pattern → bit-perfect readback (NEW)
✓ uicr-app         — App APPROTECT/SECUREAPPROTECT = 0x50FA50FA
✓ uicr-net         — Net UICR.APPROTECT = 0x50FA50FA
✓ flash-write-net  — 14-word pattern → bit-perfect readback
✓ erase-pre-recover
✓ recover          — full RECOVER end-to-end
```

## Speedup the bridge gets

A complete 1 MB App flash write on nRF5340 (or nRF52840 with the same
NVMC pattern):

| Path | Per-word USB calls | Total round-trips for 1 MB |
|---|---|---|
| Compose from `0x8C` + `0x88` poll | ~3 | ~750,000 |
| Vendor `0x87` (13 words / call) | 1/13 ≈ 0.077 | ~20,000 |

~35× fewer USB calls. With USB-FS at ~1 ms RTT, that's roughly 8–12
minutes saved on a full-image App flash write.

## Status across the 10-step plan

```
1-7 ✅ ALL SHIPPED + bench-validated
  1. DP/AP primitives
  2. CTRL-AP ERASE
  3. App UICR programming (host-side)
  4. Net stub + UICR programming
  5. RECOVER (full unlock composition)
  6. FLASH_WRITE_NET
  7. FLASH_WRITE_APP  ← this release

8. ✅ READ_MEM + WRITE_MEM (chip-agnostic primitives)
9. ⏳ Progress packets (mostly ESP32-side concern; deferrable)
10. ⏳ Ring Pro 351 acceptance (needs Ring hw on bench)
```

The Pico-side firmware now covers all the **substantive** vendor-cmd
work in the original plan. Remaining items (9, 10) are either ESP32-
side (progress notifications over BLE) or Ring-hardware-acceptance.

## Artifacts

| File | Size | SHA256 |
|---|---|---|
| `zephyr.uf2` | 151,040 B | `cc03bb0a728a718100e0241b4aea2e19c2156cd1fdb9ffcf4d2ee1405185a5ed` |
| `zephyr.bin` | 75,068 B | `c16fb44dc0ddb798eeaf918f26d4bf354ab604f088133584aefe393fb83a7a65` |
| `zephyr.elf` | 2,900,328 B | `12ad085ac2784cddcdb556e716dd37776adf6daeb0a9f9c8976acc728a24923c` |

## Tip commits

```
(this commit) — v0.1.6-step7 — NRF53_FLASH_WRITE_APP (0x87)
0e9778e        — v0.1.5-step6 — NRF53_FLASH_WRITE_NET (0x86)
e39e114        — Net stub works on nRF5340 DK silicon
54f36c6        — v0.1.3-step8 — NRF53_WRITE_MEM (0x8C)
dcd3d32        — v0.1.2-step8 — NRF53_READ_MEM (0x88)
```

## Reproduce

```bash
cd firmware/projects/edev_dapv2_zephyr
../../zephyr-patches/apply.sh
west build -b rpi_pico2/rp2350a/m33 --pristine
shasum -a 256 build/zephyr/{zephyr.uf2,zephyr.bin,zephyr.elf}
```
