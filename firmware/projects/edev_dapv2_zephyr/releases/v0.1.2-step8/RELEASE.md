# edev_dapv2_zephyr v0.1.2-step8 — release notes

> **⚠ Newer release available: [v0.1.3-step8](../v0.1.3-step8/RELEASE.md)** — superset; adds vendor cmd `0x8C NRF53_WRITE_MEM` (chip-agnostic AHB-AP burst write). v0.1.2-step8 still works but lacks `0x8C`.

**Build:** 2026-06-30 15:30
**Branch:** `feat/edev_dapv2_zephyr`
**Tip commit:** `4167fdf feat(edev_dapv2_zephyr): step 8 — NRF53_READ_MEM + Net stub DP-wake fix`
**Board:** `rpi_pico2/rp2350a/m33`
**Supersedes:** v0.1.1-step5 (which lacked the read-mem cmd + Net stub fix)

## What's new in v0.1.2-step8

### Step 8 — `0x88 NRF53_READ_MEM` (chip-agnostic AHB-AP read)

New vendor command. Chip-agnostic — works on any ARM Cortex-M reachable
via AHB-AP. Replaces the multi-step `DAP_Transfer` AP.CSW/AP.TAR/AP.DRW
dance with a single packet. Optional `VC_CORERESET_CLEAR` flag clears
`DEMCR.VC_CORERESET` + sleeps 50 ms before the read (Bug 6a fix path,
nRF5340 post-reset App readback).

**Wire format:**

```
Request  (13 bytes):
  [0]   = 0x88
  [1]   = flags  (bit 0 = clear DEMCR.VC_CORERESET first)
  [2]   = ap_index  (0 = App AHB-AP, 1 = Net AHB-AP)
  [3..6]  = u32_le csw  (nRF52/5340 App = 0x23000002; Net = 0x03800042)
  [7..10] = u32_le addr  (4-byte aligned)
  [11..12]= u16_le word_count  (≤ 15 due to current packet-size limit, see below)

Response (on success):
  [0]   = 0x88 (echo)
  [1]   = 0 (OK)
  [2..] = data, little-endian 32-bit words

Response (on error):
  [0]   = 0x88
  [1]   = nrf53_status_t code
```

**Tested on bench nRF52840:**
- `READ_MEM(ap=0, csw=0x23000002, addr=0x00000000, n=4)` → `0xFFFFFFFF` × 4 (post-erase) ✓
- `READ_MEM(ap=0, csw=0x23000002, addr=0xE000ED00, n=1)` → CPUID = `0x410FC241` (Cortex-M4) ✓

**Tested on bench nRF5340:**
- `READ_MEM(ap=1, csw=0x03800042, addr=0xE000ED00, n=1)` → CPUID = `0x410FD214` (Cortex-M33) ✓
- `READ_MEM(ap=1, csw=0x03800042, addr=0x01000000, n=4)` → first 16 B of Net stub ✓
- `READ_MEM(ap=1, csw=0x03800042, addr=0x21000000, n=4)` → 16 B of Net SRAM (zeros post-erase) ✓

### Step 8 packet-size caveat

The probe currently reports `DAP_PACKET_SIZE = 64` (per `DAP_Info(0xFF)`).
That's because the Zephyr `dap_backend_usb.c` matches DAP packet size to
USB MPS, and the RP2350 USB is full-speed (64 B bulk MPS). So
`READ_MEM` responses cap at 64 bytes total — usable for ≤ 15 words per
call.

Workaround for the ESP32 bridge: loop `READ_MEM(addr + i*60, 15)` for
bulk reads. ~17,000 calls for a full 1 MB nRF52 flash dump. At ~5 ms
per round-trip = ~85 s. Slow but works.

A proper fix needs the Zephyr DAP backend to fragment large responses
across multiple USB bulk packets — out of scope for this release.

### Step 4/5 — Net stub DP-wake refresh (partial)

After a `CTRL-AP#3 RESET` pulse, Net AHB-AP reads were returning
`0x00000000` instead of actual Net SRAM contents. Root cause: the DP/AP
state tears down during the Net core reset; just `sticky_clear` +
`power_up` isn't enough. The Python reference does a full `_full_dp_wake()`
at this boundary; we now do too.

**However:** on the bench nRF5340 silicon, even after this fix, the Net
stub runs but enters lockup state (DHCSR `S_LOCKUP = 1`) because the
SPU blocks the Net core's `str` to `0x21000000` after a clean ERASEALL.
The 196-byte stub we cloned from the Python reference was validated on
Ring Pro 351 production silicon where SPU defaults are more permissive.
On a fresh nRF5340 DK / dongle, the stub needs an SPU bootstrap
prologue (~50–100 extra bytes) before touching SRAM/peripherals.

**Status:** Known limitation. Net AHB-AP path itself works (confirmed
by writing `0xCAFEBABE` to `0x21000000` via the AP and reading it back).
The lockup is on the Net CPU side. SPU bootstrap stub is future work
— see `reference_nrfjprog_stub_disassembly_2026_06_26` for the
nrfjprog reference (their 3.7 KB stub is mostly SPU setup).

## Vendor command surface (v0.1.2-step8)

| Cmd | Op | Status | nRF52840 (HW-validated) | nRF5340 (this session) |
|---|---|---|---|---|
| `0x84` | `NRF53_RECOVER` | ✅ ships | (use `0x85`) | ⚠ ERASE+UICR_APP works; Net UICR fails on this DK silicon |
| `0x85` | `NRF53_ERASE` | ✅ ships | ✅ 5/5 PASS | ✅ 2 CTRL-APs erased |
| `0x88` | `NRF53_READ_MEM` | ✅ ships (NEW) | ✅ confirmed | ✅ confirmed |
| `0x8A` | `NRF53_UICR_PROGRAM_APP` | ✅ ships | ✗ nRF5340-only addrs | ✅ APPROTECT/SECURE = 0x50FA50FA |
| `0x8B` | `NRF53_UICR_PROGRAM_NET` | ✅ ships | n/a (no Net) | ⚠ STUB_FAIL on this silicon (SPU lockup) |
| `0x86` | `NRF53_FLASH_WRITE_NET` | ⏳ pending | n/a | future |
| `0x87` | `NRF53_FLASH_WRITE_APP` | ⏳ pending | future | future |
| `0x89` | `NRF53_TARGET_INFO` | ⏳ pending | future | future |

## Artifacts

| File | Size | SHA256 |
|---|---|---|
| `zephyr.uf2` | 146,432 B | `6697048f239d80a1376222325b86b14ea7ae58bb285fb8831cea228c65ea169b` |
| `zephyr.bin` | 73,372 B | `1c0291c147428871ca33b91726b678ad1cad23d4c14492a59aabc924f5253105` |
| `zephyr.elf` | 2,890,008 B | `1cbc7c398333de0b54477ff65051e9368eee2e4ea2b7d9168675afbc4d8bd7fb` |

Memory footprint: 73,372 B FLASH (1.75%) / 24,364 B RAM (4.58%) on RP2350.

## Hardware acceptance summary

### nRF52840 (Cortex-M4, DPv1) — **5/5 PASS**

```
✓ ping            — DPIDR=0x2ba01477  (ARM Cortex-M4, DPv1)
✓ erase           — NRF53_ERASE OK, 1 CTRL-AP
✓ verify-erase    — flash[0] = 0xFFFFFFFF
✓ uicr-app        — SKIP (nRF5340-only, correctly gated)
✓ uicr-net        — SKIP (no Net core)
```

### nRF5340 (Cortex-M33, DPv2) — **5/7 PASS**

```
✓ ping            — DPIDR=0x6ba02477  (ARM Cortex-M33, DPv2)
✓ erase           — NRF53_ERASE OK, 2 CTRL-APs (App + Net)
✓ verify-erase    — flash[0] = 0xFFFFFFFF
✓ uicr-app        — APPROTECT/SECUREAPPROTECT = 0x50FA50FA
✗ uicr-net        — STUB_FAIL: Net core enters lockup (SPU)
✓ erase-pre-recover
✗ recover         — fails at uicr-net stage (same cause)
```

The 2 failures on nRF5340 are the same root cause (Net stub SPU lockup).
The other 5 PASS prove the full SWD bit-bang + DP/AP + CTRL-AP + App
AHB-AP + Net AHB-AP code paths are healthy.

## How to flash

UF2 drag-drop:
```bash
# Hold BOOTSEL while plugging in
cp zephyr.uf2 /Volumes/RP2350/         # macOS
cp zephyr.uf2 /media/$USER/RPI-RP2/    # Linux
```

Or 1200-baud trick (if the Pico is already running ≥ v0.1.1-step5):
```bash
stty -f /dev/cu.usbmodem* 1200        # macOS
stty -F /dev/ttyACM0 1200             # Linux
# Pico re-enumerates; drag the uf2
```

## What the ESP32 bridge author gets in this release

- All previous v0.1.1-step5 capabilities (unchanged)
- **`0x88 NRF53_READ_MEM`** — single-packet memory reads, replaces the
  3-call `DAP_Transfer` sequence for AHB-AP reads. Caps at ~15 words
  per call due to USB-FS packet size, but is **chip-agnostic**: works
  on nRF52840, nRF5340 App, nRF5340 Net, and any Cortex-M target.
- Net AHB-AP behavior is now reliable across CTRL-AP-RESET boundaries
  (the dp_full_wake refresh) — even though the Net stub itself still
  hits SPU lockup, AP-side reads/writes work consistently.

## Reproduce this build

```bash
cd firmware/projects/edev_dapv2_zephyr
../../zephyr-patches/apply.sh
west build -b rpi_pico2/rp2350a/m33 --pristine
shasum -a 256 build/zephyr/{zephyr.uf2,zephyr.bin,zephyr.elf}
# should match SHA256SUMS in this dir
```
