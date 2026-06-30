# edev_dapv2_zephyr v0.1.4-step8 — release notes

**Build:** 2026-06-30 16:30
**Branch:** `feat/edev_dapv2_zephyr`
**Board:** `rpi_pico2/rp2350a/m33`
**Supersedes:** v0.1.3-step8

## What's new — Net RECOVER works on nRF5340 DK silicon ✅

Previous releases (≤ v0.1.3-step8) showed `NRF53_UICR_PROGRAM_NET` and
`NRF53_RECOVER` failing with `STUB_FAIL` on bench nRF5340 DK hardware
because the Net core entered lockup (`DHCSR.S_LOCKUP=1`) before
completing the stub. v0.1.4-step8 fixes this with three changes:

### Fix 1: lower Net stub MSP from `0x21010000` → `0x21001000`

Root cause (debugged 2026-06-30 EOD against bench nRF5340 DK):
On this silicon, Net SRAM `0x2100FFFC` (top of nominal 64 KB) reads
FAULT from Net AHB-AP — only RAMBLOCK[0] (lower 4 KB) is powered by
default after CTRL-AP RESET. The stub's initial MSP of `0x21010000`
placed the stack in unpowered RAMBLOCK[7]; the very first
exception-entry PUSH (from any minor fault) faulted on unpowered
RAM → double fault → lockup. Lowering MSP to `0x21001000` keeps the
stack in powered RAMBLOCK[0]. Stub size 224 B (was 196).

### Fix 2: VMC bootstrap prologue (defense in depth)

Added 8 × `str r1, [r0, #n*0x10]` instructions at the head of
`reset_handler` that write `0xFFFFFFFF` to all 8 Net VMC RAMBLOCK
POWERSET registers (`0x41081604`..`0x41081674`). This explicitly
powers on all SRAM blocks from Net CPU. Even on silicon where MSP=
`0x21010000` would have worked, this prologue makes the stub
portable across nRF5340 variants. Same pattern the App stub uses
(per `reference_nrfjprog_stub_disassembly_2026_06_26`).

### Fix 3: UICR.APPROTECT is the source of truth, not the SRAM marker

Empirical finding: on bench nRF5340 DK silicon, Net CPU's stores to
SRAM `0x21000000` (where the stub writes its progress markers) are
NOT visible to the host via Net AHB-AP. There's a debug-bus vs CPU-
bus asymmetry — the AHB-AP sees one view, the CPU sees another,
and writes from the CPU don't propagate to the AHB-AP-visible view.
**However:** stores to peripheral registers (NVMC, UICR) DO take
effect on the actual chip. UICR.APPROTECT readback after the stub
runs IS `0x50FA50FA`, even though the SRAM marker reads
`0x00000000`.

`nrf53_uicr_program_net()` now polls UICR.APPROTECT instead of the
SRAM marker, declaring success when APPROTECT reads `0x50FA50FA`.
The SRAM marker is still read for diagnostic-payload reporting but
is informational only. On Ring Pro 351 silicon (where the AHB-AP/
CPU views are unified) both markers and UICR work; on DK silicon
only UICR is observable. Either way the chip is correctly programmed.

## Vendor command surface (v0.1.4-step8) — unchanged from v0.1.3

| Cmd | Op | nRF52840 | nRF5340 (bench validated) |
|---|---|---|---|
| `0x84` | `NRF53_RECOVER` | (use `0x85`) | ✅ **end-to-end pass** |
| `0x85` | `NRF53_ERASE` | ✅ | ✅ |
| `0x88` | `NRF53_READ_MEM` | ✅ | ✅ |
| `0x8A` | `NRF53_UICR_PROGRAM_APP` | (nRF5340-only) | ✅ |
| `0x8B` | `NRF53_UICR_PROGRAM_NET` | (no Net core) | ✅ **now passing** |
| `0x8C` | `NRF53_WRITE_MEM` | ✅ | ✅ |
| `0x86` | `NRF53_FLASH_WRITE_NET` | n/a | ⏳ pending |
| `0x87` | `NRF53_FLASH_WRITE_APP` | ⏳ pending | ⏳ pending |
| `0x89` | `NRF53_TARGET_INFO` | ⏳ pending | ⏳ pending |

## Hardware acceptance — **8/8 PASS on bench nRF5340 DK** 🎉

```
✓ ping             — DPIDR=0x6ba02477 (Cortex-M33, DPv2)
✓ erase            — 2 CTRL-APs (App + Net)
✓ verify-erase     — flash[0] = 0xFFFFFFFF
✓ mem-roundtrip    — WRITE_MEM + READ_MEM bit-perfect
✓ uicr-app         — App APPROTECT/SECUREAPPROTECT = 0x50FA50FA
✓ uicr-net         — Net UICR.APPROTECT = 0x50FA50FA (NEW PASS)
✓ erase-pre-recover
✓ recover          — FULL UNLOCK end-to-end (NEW PASS)
```

Test response from `0x84 NRF53_RECOVER`:
```
84 00 02  fa50fa50  fa50fa50  00000000  fa50fa50
^  ^  ^   ^         ^         ^         ^
|  |  |   |         |         |         └─ Net APPROTECT (success)
|  |  |   |         |         └─────────── Net SRAM marker (informational; 0 on DK)
|  |  |   |         └───────────────────── App SECUREAPPROTECT
|  |  |   └─────────────────────────────── App APPROTECT
|  |  └─────────────────────────────────── ap_count (App + Net)
|  └────────────────────────────────────── status = OK
└───────────────────────────────────────── echo of 0x84
```

## Artifacts

| File | Size | SHA256 |
|---|---|---|
| `zephyr.uf2` | 147,456 B | `1fa8226f6a9091ab6a87027224a565fc1e5fed1092ce8b7d53e88b309d2cd1a9` |
| `zephyr.bin` | 73,884 B | `0652b5f1b754213d6fb8b3d9aa3fcb46589de66eb6abfc21ac9ae904d9005d19` |
| `zephyr.elf` | 2,893,840 B | `d42e5a3d70fb3d99ced4365c2dfdcad3babff10b93d540fe001fff832d565c4f` |

## Tip commits this release

```
(next commit will be tagged v0.1.4-step8)
54f36c6  v0.1.3-step8 — NRF53_WRITE_MEM (0x8C)
dcd3d32  v0.1.2-step8 — NRF53_READ_MEM (0x88)
```

## Reproduce

```bash
cd firmware/projects/edev_dapv2_zephyr
../../zephyr-patches/apply.sh
west build -b rpi_pico2/rp2350a/m33 --pristine
shasum -a 256 build/zephyr/{zephyr.uf2,zephyr.bin,zephyr.elf}
```
