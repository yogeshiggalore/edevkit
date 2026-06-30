# edev_dapv2_zephyr v0.1.3-step8 — release notes

**Build:** 2026-06-30 16:00
**Branch:** `feat/edev_dapv2_zephyr`
**Board:** `rpi_pico2/rp2350a/m33`
**Supersedes:** v0.1.2-step8

## What's new since v0.1.2-step8

**Vendor command `0x8C NRF53_WRITE_MEM`** — counterpart to `0x88 NRF53_READ_MEM`.
Chip-agnostic AHB-AP burst write. Replaces the multi-step
`DAP_Transfer(CSW)` + `DAP_Transfer(TAR)` + `DAP_Transfer(DRW)`
sequence with a single packet.

**Wire format:**

```
Request  (>= 13 bytes):
  [0]    = 0x8C
  [1]    = flags  (reserved, pass 0)
  [2]    = ap_index  (0 = App, 1 = Net)
  [3..6]   = u32_le csw   (e.g. 0x23000002 App, 0x03800042 Net)
  [7..10]  = u32_le addr  (4-byte aligned)
  [11..12] = u16_le word_count  (≤ 12 due to 64 B USB-FS packet limit)
  [13..]   = data, little-endian 32-bit words

Response:
  [0] = 0x8C (echo)
  [1] = nrf53_status_t code
```

**Use cases for the ESP32 bridge:**
- Write target RAM for staging a flash loader (CMSIS Flash Algo pattern)
- Set Cortex-M registers via DCRSR/DCRDR writes
- Configure target peripherals (NVMC, UICR, SPU, etc.) from the host
- Anything currently composed from 3 `DAP_Transfer` calls

**Hardware-validated on bench nRF5340:**
- WRITE_MEM 4 words to Net SRAM[`0x21000000`] via Net AHB-AP → bit-perfect READ_MEM readback
- WRITE_MEM 1 word `0xAABBCCDD` to App SRAM[`0x20000000`] via App AHB-AP → bit-perfect readback

Mem round-trip test added to `tools/test_nrf53_vendor_cmds.py` as
`mem-roundtrip`. Now part of the auto-run suite.

## Vendor command surface (v0.1.3-step8)

| Cmd | Op | Wire | Status |
|---|---|---|---|
| `0x84` | `NRF53_RECOVER`         | 1-byte req, 19-byte resp | ✅ ships |
| `0x85` | `NRF53_ERASE`           | 1-byte req, 3-byte resp | ✅ ships |
| `0x88` | `NRF53_READ_MEM`        | 13-byte req, up to ~60 B data | ✅ ships |
| `0x8A` | `NRF53_UICR_PROGRAM_APP` | 1-byte req, 10-byte resp | ✅ ships |
| `0x8B` | `NRF53_UICR_PROGRAM_NET` | 1-byte req, 10-byte resp | ✅ ships (works on Ring Pro 351; SPU-lockup on DK silicon) |
| **`0x8C`** | **`NRF53_WRITE_MEM`** | **≥ 13 byte req, 2-byte resp** | ✅ **ships (NEW)** |
| `0x86` | `NRF53_FLASH_WRITE_NET` | — | ⏳ pending |
| `0x87` | `NRF53_FLASH_WRITE_APP` | — | ⏳ pending |
| `0x89` | `NRF53_TARGET_INFO`     | — | ⏳ pending |

## Hardware acceptance summary

### nRF5340 (Cortex-M33, DPv2) — **6/8 PASS** (was 5/7)

```
✓ ping            — DPIDR=0x6ba02477
✓ erase           — 2 CTRL-APs erased
✓ verify-erase    — flash[0] = 0xFFFFFFFF
✓ mem-roundtrip   — WRITE_MEM + READ_MEM bit-perfect (NEW)
✓ uicr-app        — App APPROTECT/SECUREAPPROTECT = 0x50FA50FA
✗ uicr-net        — STUB_FAIL (SPU lockup on this DK silicon)
✓ erase-pre-recover
✗ recover         — fails at uicr-net stage (same root cause)
```

### nRF52840 — auto-validated by previous v0.1.2 testing
Standard CMSIS-DAP + `0x85 ERASE` + `0x88 READ_MEM` work as before;
`0x8C WRITE_MEM` works against any AHB-AP-reachable address (same
code path as nRF5340 testing).

## Artifacts

| File | Size | SHA256 |
|---|---|---|
| `zephyr.uf2` | 147,456 B | `805149d13e6d6b054c28a4fcbc4bcd55a4c0b2cf93728a1419ebafe721180d74` |
| `zephyr.bin` | 73,884 B | `f9dfaa618ee88816db8243370a4947ba4bc98aefc4efe678a92c25f61d348249` |
| `zephyr.elf` | 2,893,840 B | `17b7ba4fe6da4b8c18409dd9762bea533811968562ca19f7ae7f8025d68597d2` |

Memory footprint: 73,884 B FLASH (1.76%) / 24,364 B RAM (4.58%) on RP2350.

## What the ESP32 bridge gets

`0x8C WRITE_MEM` makes the bridge's flash-write path on nRF52840 much
simpler:
- Set `UICR.APPROTECT` after recover: 1 `WRITE_MEM` call (vs 3 `DAP_Transfer`)
- Configure CSW + write data: 1 `WRITE_MEM` call
- Halt target via DHCSR: 1 `WRITE_MEM` (DHCSR @ 0xE000EDF0 = 0xA05F0003)
- Set CPU registers via DCRSR/DCRDR: 2 `WRITE_MEM` calls

A full CMSIS Flash Algo flash-write loop on nRF52840 is now mostly
`WRITE_MEM` + `READ_MEM` rather than dozens of `DAP_Transfer` packets.

See `docs/ESP32_BRIDGE.md` Appendix K for the updated nRF52840 cookbook
with `0x8C` recipes.

## Reproduce

```bash
cd firmware/projects/edev_dapv2_zephyr
../../zephyr-patches/apply.sh
west build -b rpi_pico2/rp2350a/m33 --pristine
shasum -a 256 build/zephyr/{zephyr.uf2,zephyr.bin,zephyr.elf}
```
