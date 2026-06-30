# edev_dapv2_zephyr v0.1.8 — release notes

> **⚠ Superseded by [v0.1.9](../v0.1.9/RELEASE.md)** — fixes the
> Net flash 0x01000000 boot-region write corruption documented in
> this release's K.bonus.1 note. The bridge author no longer needs
> any workaround for the boot region. **Re-flash v0.1.8 probes to
> v0.1.9 if you intend to write Net flash images that include data
> at 0x01000000+.**

**Build:** 2026-06-30 (docs+tools-only release)
**Branch:** `feat/edev_dapv2_zephyr`
**Board:** `rpi_pico2/rp2350a/m33`
**Supersedes:** v0.1.7
**Firmware binaries:** byte-identical to v0.1.7 (no source changes)

## What's new

This is a **docs + tools** release. The probe firmware itself is
unchanged from v0.1.7 — same `zephyr.bin`, same `zephyr.uf2`, same
SHA256s. **You do not need to re-flash a v0.1.7 probe to get v0.1.8
functionality.** Re-flashing is a no-op.

What changed since v0.1.7:

### 1. ESP32 bridge guide — comprehensive doc sync

`docs/ESP32_BRIDGE.md` now reflects everything the v0.1.7 firmware
ships, with the bench-validated gotchas the bridge author needs.

- **Appendix K.bonus.0 — `0x89 NRF53_TARGET_INFO`** (added in commit
  `79c9bc2`): wire format, family/vendor/core inference recipe,
  when-to-call guidance.
- **Appendix K.4 FICR caveat**: `nRF53 FICR.INFO.PART @ +0x140`
  returns `0xFFFFFFFF` on bench DK silicon. Bridge should use
  `0x89` for family detection and probe FICR at multiple candidate
  offsets via `0x88 READ_MEM`, treating `0xFFFFFFFF` as "subtype
  unknown."
- **Appendix L — Per-cmd timing budgets + bridge progress strategy**
  (commit `065803e`): bench wall-clock measurements per vendor cmd,
  recommended USB bulk-transfer timeouts for the ESP32 side,
  Pattern A (bridge-side timer-driven BLE notifications,
  recommended) vs Pattern B (probe-side intermediate packets, NOT
  recommended). Closes out Step 9 of the original 10-step plan.
- **Appendix M — Ring Pro 351 acceptance procedure** (commit
  `065803e`): full procedure for end-to-end production-silicon
  acceptance against a real Ring. Per-phase failure-mode triage
  table. Pending execution against actual Ring hardware.
- **K.bonus.1 — Net flash @ 0x01000000 gotcha** (commit `c26fbfa`):
  writes to the very first word of Net flash silently succeed at
  NVMC level but don't persist (likely reset-vector / mass-erase-
  mark area at hardware level). Bridge should start Net writes at
  `0x01000800` or later. Validated 2026-06-30 against bench DK.
- **K.10 freshness pass**: removed stale "future cmd" notes for
  `0x87`/`0x88`/`0x8A` (all shipped in v0.1.2–v0.1.7); reduced to
  the genuinely-still-bridge-composed items (multi-batch loops,
  nRF52 UICR programming).

### 2. New bench tools

- `tools/ring_pro_351_acceptance.py` (commit `065803e`) — 9-phase
  runner with 3× bench-DK timing budgets, explicit "WIPE THE RING"
  consent prompt, `--identify` non-destructive mode for SWD/contact
  verification.
- `tools/_flash_ops_sequence.py` (commit `c26fbfa`) — quick
  info→read→erase→recover→write driver for ad-hoc bench validation.
  Reuses `test_nrf53_vendor_cmds.py` primitives. 5/5 PASS on
  bench nRF5340 DK.

### 3. Memory note updates (`/Users/yogesh/.claude/projects/.../memory/`)

- `project_edev_dapv2_zephyr_v017_feature_complete_2026_06_30.md`
  — supersedes the v0.1.3 entry pinned in MEMORY.md; documents the
  full 9-vendor-cmd surface, three critical findings (Net stub MSP
  fix, CPU/AHB-AP debug-bus asymmetry, FICR caveat), commit chain,
  and remaining-work resume points.

## Vendor command surface (v0.1.8) — unchanged from v0.1.7

| Cmd | Op | nRF52840 | nRF5340 |
|---|---|---|---|
| `0x84` | `NRF53_RECOVER` | n/a (use `0x85`) | ✅ HW |
| `0x85` | `NRF53_ERASE` | ✅ HW | ✅ HW |
| `0x86` | `NRF53_FLASH_WRITE_NET` | n/a | ✅ HW |
| `0x87` | `NRF53_FLASH_WRITE_APP` | algo | ✅ HW |
| `0x88` | `NRF53_READ_MEM` | ✅ HW | ✅ HW |
| `0x89` | `NRF53_TARGET_INFO` | algo | ✅ HW |
| `0x8A` | `NRF53_UICR_PROGRAM_APP` | n/a | ✅ HW |
| `0x8B` | `NRF53_UICR_PROGRAM_NET` | n/a | ✅ HW |
| `0x8C` | `NRF53_WRITE_MEM` | ✅ HW | ✅ HW |

## Hardware acceptance — **11/11 PASS** (bench nRF5340 DK, re-validated)

Re-ran `test_nrf53_vendor_cmds.py` and `_flash_ops_sequence.py`
against the connected bench probe (chip ID `FA6418C89BAB3D36`,
running v0.1.7 firmware which is byte-identical to v0.1.8) on
2026-06-30 EOD:

```
test_nrf53_vendor_cmds.py:   11/11 PASS
_flash_ops_sequence.py:       5/5 PASS  (info, read, erase, recover, write)
```

## Artifacts (byte-identical to v0.1.7)

| File | Size | SHA256 |
|---|---|---|
| `zephyr.uf2` | 152,064 B | `f0af00aa57d6f59b290bb58465f19428058b5336cab2aaeff7979442a1e446b7` |
| `zephyr.bin` | 75,932 B | `d86b0753c54ea4884327cd5f557c0f33f3938694a19720333c52f4ab8ee9fec1` |
| `zephyr.elf` | 3,018,472 B | `79cb9575df5cf80fa83c05beba0bb0999c5b77eba9da3d6a0b34643058f1c734` |

Memory footprint: 75,932 B FLASH (1.81%) / 24,364 B RAM (4.58%).

## Pending across the 10-step plan

```
1-8 ✅ ALL SHIPPED + bench-validated (since v0.1.7)
9.  ✅ Doc-only deliverable (Appendix L, this release)
10. ⏳ Run ring_pro_351_acceptance.py against Ring Pro 351
       — runner shipped, blocked on Ring + charging puck on bench
```

## Reproduce

```bash
cd firmware/projects/edev_dapv2_zephyr
export ZEPHYR_BASE=/Users/yogesh/projects/temp/zephyrproject/zephyr
../../zephyr-patches/apply.sh
west build -b rpi_pico2/rp2350a/m33 --pristine
# NOTE: rebuilt SHAs may differ from above due to embedded build
# timestamps in zephyr.elf. zephyr.bin/uf2 are byte-deterministic
# given the same source tree; mismatches indicate either patch
# drift or a different toolchain version.
```

## Commit chain (v0.1.7 → v0.1.8)

```
c26fbfa  docs+tools: Net flash @ 0x01000000 write gotcha + _flash_ops_sequence.py
065803e  feat: close out Steps 9 + 10 — Appendix L (timing) + M (Ring acceptance) + runner
79c9bc2  docs(esp32): v0.1.7 sync — 0x89 TARGET_INFO + FICR caveat
31e3082  feat: v0.1.7 — NRF53_TARGET_INFO (0x89)   ← v0.1.7 tag
```

## Bridge-side action items (informational)

Items the ESP32 bridge author should pick up from the new docs:

1. **Read Appendix L.3** — set per-cmd USB bulk transfer timeouts
   on the ESP32 host stack (30s for `0x84`, 15s for `0x85`, etc.)
2. **Implement Pattern A from L.4** — bridge-side timer-driven
   `TLV_PROGRESS` BLE notifications, 250 ms cadence, capped at 99 %.
3. **Avoid `0x01000000` in Net flash writes** — start at
   `0x01000800` or later per K.bonus.1's new gotcha note.
4. **Use `0x89 TARGET_INFO` on connect** — single packet replaces
   3 separate `DAP_Transfer` round-trips for family identification.
