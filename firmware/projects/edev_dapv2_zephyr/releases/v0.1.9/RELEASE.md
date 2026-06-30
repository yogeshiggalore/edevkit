# edev_dapv2_zephyr v0.1.9 — release notes

**Build:** 2026-06-30
**Branch:** `feat/edev_dapv2_zephyr`
**Board:** `rpi_pico2/rp2350a/m33`
**Supersedes:** v0.1.8

> **⚠ Firmware change — re-flash required.**
> The probe binary has changed since v0.1.8 (different SHA256s).
> A v0.1.8 probe MUST be re-flashed to v0.1.9 to get the
> Net flash 0x01000000 boot-region fix.

## What's new — Net flash boot-region auto-erase

Fixes the silicon-specific corruption that caused Ring Pro 351
production firmware images to fail whole-image hash verification
on v0.1.8 and earlier.

### Background

`nrf53_uicr_program_net()` (called by RECOVER) programs the
224-byte UICR-disable stub into Net flash starting at `0x01000000`.
After RECOVER completes, Net flash page 0 contains real (non-erased)
data. Any subsequent `0x86 NRF53_FLASH_WRITE_NET` call to page 0
gets bitwise-AND'd with the existing stub bytes because NVMC can
only program in the 1→0 direction.

Two earlier debugging approaches failed:
1. **Skipping reset between batches** — Net core not the culprit;
   corruption is from the existing flash residue, not active CPU
   activity.
2. **NVMC.ERASEPAGE register at offset 0x508** — non-functional on
   nRF5340 Net NVMC. Writes return immediately without triggering
   any erase (NVMC.READY stays at 1).

### The actual fix — write `0xFFFFFFFF` to trigger page erase

The working method is the nrfx HAL fallback used by probe-rs and
Nordic's official CMSIS Flash Algorithm (`nrf53xx_network.flm`):

```c
NVMC.CONFIG = NVMC_CONFIG_WEN_Een;     // 2 = erase enable
*(volatile uint32_t *)page_addr = 0xFFFFFFFF;
while (!NVMC.READY) {}                  // ~85 ms typical
NVMC.CONFIG = NVMC_CONFIG_WEN_Ren;     // back to read mode
```

The critical detail is the **value `0xFFFFFFFF`** — writing any
other value (e.g. `0`) does NOT trigger erase on this silicon.
This is undocumented in Nordic's public NVMC spec but visible in
the nrfx HAL source (`nrf_nvmc.h` lines ~417–441, `#ifdef
NVMC_ERASEPAGE_ERASEPAGE_Msk` fallback branch).

### Probe firmware change

`nrf53_flash_write_net()` now:
1. Checks if the target page is Net page 0 (`addr & ~0x7FF ==
   0x01000000`) AND a dirty flag is set.
2. If both, halts Net CPU via DHCSR + issues the NVMC page-erase
   sequence above + waits for completion (~85 ms).
3. Clears the dirty flag (subsequent batches into the same page
   skip the erase).
4. Proceeds with normal NVMC.CONFIG=Wen + per-word programming.

The dirty flag is set when `nrf53_uicr_program_net()` programs the
UICR-disable stub, cleared by chip-wide `nrf53_erase_all()`. This
ensures we only pay the ~85 ms erase cost once per session, on the
very first user write to page 0 after RECOVER.

## Hardware acceptance — 11/11 + 2/2 PASS

Regression suite (bench nRF5340 DK, chip ID `FA6418C89BAB3D36`):

```
test_nrf53_vendor_cmds.py          11/11 PASS
```

End-to-end production-firmware acceptance (`_program_and_verify.py`,
both production Ring Pro 351 hex files):

```
uh_ringpro351_v5241951_merged.hex  (718,153 B, 14 segments)
  - all 14 segments verify bit-perfect
  - whole-image SHA-256 = be4da3e6...4654cf MATCH
uh_ringpro351_v5242051_merged.hex  (718,154 B, 14 segments)
  - all 14 segments verify bit-perfect
  - whole-image SHA-256 = 5059c75c...11564f MATCH
```

This is the first probe-firmware release that can byte-for-byte
match J-Link / nrfjprog when flashing the Net core boot region.

## Vendor command surface (v0.1.9) — unchanged from v0.1.8

| Cmd | Op | nRF52840 | nRF5340 |
|---|---|---|---|
| `0x84` | `NRF53_RECOVER` | n/a (use `0x85`) | ✅ HW |
| `0x85` | `NRF53_ERASE` | ✅ HW | ✅ HW |
| `0x86` | `NRF53_FLASH_WRITE_NET` (now with page-0 auto-erase) | n/a | ✅ HW |
| `0x87` | `NRF53_FLASH_WRITE_APP` | algo | ✅ HW |
| `0x88` | `NRF53_READ_MEM` | ✅ HW | ✅ HW |
| `0x89` | `NRF53_TARGET_INFO` | algo | ✅ HW |
| `0x8A` | `NRF53_UICR_PROGRAM_APP` | n/a | ✅ HW |
| `0x8B` | `NRF53_UICR_PROGRAM_NET` | n/a | ✅ HW |
| `0x8C` | `NRF53_WRITE_MEM` | ✅ HW | ✅ HW |

No wire-format changes. The `0x86` command takes the same packet
shape; the new erase logic is internal to the firmware.

## Bridge-side impact

| Before (v0.1.8) | After (v0.1.9) |
|---|---|
| Bridge had to write Net boot region last, OR chunk + pause, OR retry on verify-fail | **No bridge changes needed.** Stream the image in natural address order. |
| First Net segment (page 0) corrupted; verify fails | First Net segment writes ~75 ms slower (one-shot 85 ms erase + write); subsequent calls normal speed; verify passes |

Per-cmd timing budgets (Appendix L.3) — the first `0x86 NRF53_FLASH_WRITE_NET`
to address `0x01000000` after RECOVER now takes up to ~120 ms (was
~30 ms). The recommended 2-second USB timeout in L.3 was already
4× this; no bridge-side timeout adjustment needed.

## What's new in tooling

- `tools/webtest/` — FastAPI + HTML web UI on `http://127.0.0.1:8766/`
  wrapping all vendor commands with a browser interface. Includes
  drag-drop hex flash + verify with live SSE progress. See
  `tools/webtest/README.md`.
- `tools/_program_and_verify.py` — already shipped; now passes 2/2
  on production hex files with this firmware.

## Artifacts

| File | Size | SHA256 |
|---|---|---|
| `zephyr.uf2` | 153,600 B | `49863da4eefe36bce3ae71863e09ba560e4875996d6107b55bbc19bbe9906fd7` |
| `zephyr.bin` | 76,576 B | `365335d6f2996ae4577e6baf8dad365c8c74c5d8b7265226820810578af8a623` |
| `zephyr.elf` | 3,042,228 B | `351fb3e7646c57ce53e13459695a50234d00ec6df16cc34bd40d4e3fe7ebd59f` |

Memory footprint: 76,576 B FLASH (1.83%) / 24,364 B RAM (4.58%).

## Reproduce

```bash
cd firmware/projects/edev_dapv2_zephyr
export ZEPHYR_BASE=/path/to/zephyr
../../zephyr-patches/apply.sh
west build -b rpi_pico2/rp2350a/m33 --pristine
```

zephyr.bin/uf2 SHA256s are deterministic given the same source.
The zephyr.elf SHA may vary slightly due to embedded build
timestamps.

## Pending

```
1-8 ✅ original 10-step plan shipped + bench-validated
9.  ✅ Appendix L (timing budgets); no probe-firmware work needed
10. ⏳ Run ring_pro_351_acceptance.py against real Ring Pro 351 hw
       — runner shipped, blocked on Ring + charging puck on bench;
       the production-hex test in this release proves the firmware
       is ready, but actual Ring silicon validation pending.
```

## Files changed since v0.1.8

```
firmware/projects/edev_dapv2_zephyr/
├── src/nrf53/
│   ├── nrf53.h                  — bool include + 3 new function decls
│   ├── nrf53_flash_write.c      — adds nrf53_net_page_erase() helper
│   │                              + page-0 dirty-flag check at entry
│   │                              + nrf53_net_page0_mark_{dirty,clean,is_dirty}
│   ├── nrf53_stubs.c            — calls mark_dirty() after writing UICR stub
│   └── nrf53_ctrl_ap.c          — calls mark_clean() after ERASEALL
└── tools/webtest/               — new FastAPI web UI (server.py + index.html)

docs/ESP32_BRIDGE.md             — v0.1.8 → v0.1.9 across status / appendices;
                                   K.bonus.1 inverted from "boot region corrupts +
                                   workarounds" to "auto-erased, no workaround"
```

~80 lines of new C across 4 files for the fix itself; ~600 lines
for the new web UI; ~50 lines of doc updates.
