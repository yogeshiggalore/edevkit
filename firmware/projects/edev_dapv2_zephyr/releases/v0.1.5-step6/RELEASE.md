# edev_dapv2_zephyr v0.1.5-step6 — release notes

**Build:** 2026-06-30 17:00
**Branch:** `feat/edev_dapv2_zephyr`
**Board:** `rpi_pico2/rp2350a/m33`
**Supersedes:** v0.1.4-step8

## What's new — `NRF53_FLASH_WRITE_NET (0x86)`

Step 6 of the implementation plan ships. Probe-side batched Net flash
programming: write up to 14 contiguous 32-bit words per packet, with
the NVMC sequence (Wen → per-word write+READY → Ren) wrapped inside
the probe.

**Wire format:**

```
Request (>= 7 bytes):
  [0]    = 0x86
  [1..4] = u32_le addr        (4-byte aligned; typically 0x01000000..0x0103FFFF)
  [5..6] = u16_le word_count  (≤ 14 due to 64 B USB-FS packet limit)
  [7..]  = data, little-endian 32-bit words

Response (4 bytes):
  [0]   = 0x86 (echo)
  [1]   = nrf53_status_t
  [2..3] = u16_le words_written  (= word_count on full success)
```

`words_written` reports how many words made it before any failure —
the bridge can resume from that offset on retry.

**Behaviour:**
- AP and CSW are hardcoded: ap=1, csw=0x03800042 (Net AHB-AP). Caller
  doesn't need to track them.
- Words equal to `0xFFFFFFFF` are skipped (NVMC can't reprogram erased
  flash to erased flash anyway, and skipping saves USB time on sparse
  data).
- NVMC.CONFIG = Ren is restored even on mid-batch failure — flash is
  never left write-enabled.
- Caller must ensure target page is already erased (typically via
  prior `0x85 NRF53_ERASE`).

**Hardware-validated** on bench nRF5340 DK:
- Wrote 14-word pattern (`0x11111111` … `0xCAFEBABE`) to Net flash at
  `0x01000800`; readback via `0x88` was bit-perfect.
- Mid-batch `0xFFFFFFFF` skip verified: `[0xA, 0xB, 0xFFFFFFFF, 0xD]`
  → readback `[0xA, 0xB, 0xFFFFFFFF, 0xD]` (the 0xFF word stayed as
  erased flash, the other 3 were programmed).

## Vendor command surface (v0.1.5-step6)

| Cmd | Op | Wire | nRF52840 | nRF5340 |
|---|---|---|---|---|
| `0x84` | RECOVER | 1→19 B | (use `0x85`) | ✅ |
| `0x85` | ERASE | 1→3 B | ✅ | ✅ |
| **`0x86`** | **FLASH_WRITE_NET** | **≥7→4 B** | n/a | ✅ **(NEW)** |
| `0x88` | READ_MEM | 13→≤60 B data | ✅ | ✅ |
| `0x8A` | UICR_PROGRAM_APP | 1→10 B | (5340 addrs) | ✅ |
| `0x8B` | UICR_PROGRAM_NET | 1→10 B | n/a | ✅ |
| `0x8C` | WRITE_MEM | ≥13→2 B | ✅ | ✅ |
| `0x87` | FLASH_WRITE_APP | — | ⏳ pending | ⏳ pending |
| `0x89` | TARGET_INFO | — | ⏳ pending | ⏳ pending |

## Hardware acceptance — **9/9 PASS** on bench nRF5340 DK

```
✓ ping             — DPIDR=0x6ba02477 (Cortex-M33, DPv2)
✓ erase            — 2 CTRL-APs (App + Net)
✓ verify-erase     — flash[0] = 0xFFFFFFFF
✓ mem-roundtrip    — WRITE_MEM + READ_MEM bit-perfect
✓ uicr-app         — App APPROTECT/SECUREAPPROTECT = 0x50FA50FA
✓ uicr-net         — Net UICR.APPROTECT = 0x50FA50FA
✓ flash-write-net  — 14-word pattern → bit-perfect readback (NEW)
✓ erase-pre-recover
✓ recover          — full RECOVER end-to-end
```

## What this gives the ESP32 bridge for nRF5340 flash programming

Before v0.1.5, the bridge had to compose Net flash write from
`0x8C WRITE_MEM` + `0x88 READ_MEM` (NVMC.READY poll) — roughly 3
USB calls per word + 1 Wen/Ren per word. With v0.1.5, one `0x86`
call programs up to 14 words with a single Wen/Ren wrapping.

Estimated speedup for a full 256 KB Net flash write:
- Before: 65,536 words × 3 calls/word ≈ 200,000 USB round-trips
- After:  65,536 / 14 batches × 1 call/batch ≈ 4,700 USB round-trips
- ~40× fewer USB calls; expected ~5–10× wall-clock speedup over USB-FS

## Artifacts

| File | Size | SHA256 |
|---|---|---|
| `zephyr.uf2` | 148,992 B | `ff400462a1938f63714ea890a50920bd0c143deb458aca0540baecb9dad1aedb` |
| `zephyr.bin` | 74,140 B | `411afa99f8b223b789cfdea74c3e9e3e135bb638cd0a4dd34b550f4e44d1be23` |
| `zephyr.elf` | 2,896,560 B | `5e9869bc3580c8045eee4a21c55347af460ad5388aa35e1549173075444ebff9` |

## Tip commits

```
(this commit) — v0.1.5-step6 — NRF53_FLASH_WRITE_NET (0x86)
e39e114        — Net stub works on nRF5340 DK silicon — 8/8 PASS
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
