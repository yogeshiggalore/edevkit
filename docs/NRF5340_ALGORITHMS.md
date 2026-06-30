# nRF5340 recover / erase / write / read — algorithm reference

Register-level pseudocode for the Zephyr probe firmware to implement
nRF5340 recover, erase, flash-write, and direct-memory read **on the
probe itself** (no host-side Python orchestration). Extracted from the
canonical Python reference at `feat/edev_dapv2:firmware/projects/edev_dapv2/tools/webgui/`.

> **Source of truth.** When the C implementation diverges from this doc,
> first verify against `git show feat/edev_dapv2:.../pyocd_diag.py`
> (lines cited below) — that Python is the reference that actually works
> on Ring Pro 351 hardware. This doc captures the algorithm; the Python
> captures the empirical fixes.

Companion docs:
- `docs/ESP32_BRIDGE.md` — how the bridge exposes these to the app
- Memory notes: `project_edev_dapv2_nrf5340_recover_button_2026_06_26`,
  `reference_nrf5340_net_uicr_write_attempts_2026_06_26`,
  `reference_nrfjprog_stub_disassembly_2026_06_26`,
  `reference_nrf5340_erase_recover_write_canonical_2026_06_26`

---

## 1. Register map

### DP (Debug Port — standard ADIv5)

| Offset | Name | Purpose |
|---|---|---|
| `0x00` | ABORT | Clear sticky bits: `STKCMPCLR | STKERRCLR | ORUNERRCLR | WDERRCLR` = `0x0000001E` |
| `0x04` | CTRL/STAT | Power control. Write `0x0000000F` to request `CSYSPWRUPREQ | CDBGPWRUPREQ`; poll for ack. |
| `0x08` | SELECT | AP bank + AP index. `(ap_index << 24) | bank_offset` |
| `0x0C` | RDBUFF | Read buffer (returns data from previous AP read; also flushes posted-read pipeline) |

### AP bank 0 (standard MEM-AP)

| Offset | Name | Purpose |
|---|---|---|
| `0x00` | CSW | Control/Status word — sets HPROT, size, auto-increment |
| `0x04` | TAR | Transfer Address Register |
| `0x0C` | DRW | Data Read/Write |
| `0xFC` | IDR | AP identification (Nordic CTRL-AP IDR = `0x02880000` or `0x12880000`) |

### Nordic CTRL-AP (SoC-specific, AP#2 = App, AP#3 = Net)

| Offset | Name | Purpose |
|---|---|---|
| `0x00` | RESET | Write 1 to assert reset, 0 to release |
| `0x04` | ERASEALL | Write 1 to start mass-erase (flash + UICR for that core) |
| `0x08` | ERASEALLSTATUS | 0 = idle, 1 = busy |
| `0x10` | APPROTECTSTATUS | Read APPROTECT state |

### nRF5340 peripheral addresses

```
App NVMC:
  0x50039400  READY    (bit 0 = 1 when idle)
  0x50039504  CONFIG   (0=Ren, 1=Wen, 2=Een, 4=PEen)

Net NVMC:
  0x41080400  READY
  0x41080504  CONFIG

App UICR:
  0x00FF8000  APPROTECT          (0x50FA50FA = HwDisabled, 0xFFFFFFFF = HwEnabled)
  0x00FF801C  SECUREAPPROTECT    (same encoding)

Net UICR:
  0x01FF8000  APPROTECT          (same encoding)

Cross-bridge:
  0x50005614  RESET.NETWORK.FORCEOFF (App-side; 0 releases Net core from forced-off)

Debug control:
  0xE000EDFC  DEMCR              (bit 0 = VC_CORERESET — halt on reset vector)
  0xE000ED0C  AIRCR              (write 0x05FA0004 = SYSRESETREQ)
```

### CSW values (Nordic-specific, taken from J-Link wire traces)

| AP | Role | CSW | Meaning |
|---|---|---|---|
| 0 | App AHB-AP | `0x23000002` | HPROT=secure/priv, 32-bit, **no auto-increment** |
| 1 | Net AHB-AP | `0x03800042` | HPROT=cache/priv/data, 32-bit, no auto-inc, device-enabled |

> **Why no auto-increment?** TAR auto-increment wraps at 1 KB boundaries on
> some Nordic chips; explicit per-word TAR writes are safer and only ~5%
> slower over USB. The Python ref uses per-word TAR throughout.

### Magic values

```
Stub progress markers (Net stub writes these to SRAM):
  0x11111111   reset_handler entered
  0x22222222   NVMC.CONFIG=Wen acknowledged
  0xDEADC0DE   complete — UICR written, CONFIG=Ren done
  0xBADF00D5   fault occurred (with PC + xPSR in next words)

APPROTECT unlock value:
  0x50FA50FA   "HwDisabled" — written to UICR.APPROTECT/SECUREAPPROTECT

DP.ABORT sticky-clear mask:
  0x0000001E   STKCMPCLR | STKERRCLR | ORUNERRCLR | WDERRCLR

CTRL/STAT power-up request:
  0x0000000F   CSYSPWRUPREQ | CDBGPWRUPREQ (lower 4 bits, request side)
               wait for bits 28 (CDBGPWRUPACK) + 30 (CSYSPWRUPACK) = `0x50000000`

DEMCR halt-at-reset:
  0x00000001   VC_CORERESET (chip halts at reset vector on next CTRL-AP RESET)

Net SRAM marker base:
  0x21000000   first word — stub stage
  0x21000004   0xBADF00D5 if faulted
  0x21000008   faulted PC
  0x2100000C   faulted xPSR

ERASEALL value:
  0x00000001   write to CTRL-AP.ERASEALL to start mass-erase
```

---

## 2. Full DP wake (J-Link `CORESIGHT_Configure` equivalent)

Run this at the start of every multi-step flow and after any CTRL-AP
RESET. Without it, sticky bits and power state accumulate and Net AHB-AP
writes fault mid-flow (bug 6b).

Reference: `pyocd_diag.py::_full_dp_wake()` @ line 373.

```
1. SWD line reset:           swj_sequence(56 clocks, all 1s = 0xFFFFFFFFFFFFFFFF)
2. JTAG→SWD switch:          swj_sequence(16, 0xE79E)
3. SWD dormant-wake alert:   swj_sequence(148, _SWD_ALERT_BYTES)
4. SWD line reset again:     swj_sequence(56, 0xFFFFFFFFFFFFFFFF)
5. dp.connect()              [DPIDR read]
6. DP.ABORT  ← 0x0000001E    [clear all sticky bits]
7. DP.CTRL/STAT ← 0x0000000F [power up debug — CDBGPWRUPREQ | CSYSPWRUPREQ]
   poll until (CTRL/STAT & 0x50000000) == 0x50000000  [CDBGPWRUPACK + CSYSPWRUPACK]
   timeout 100 ms
```

C implementation lives in `nrf53_dp.c::dp_full_wake()`. Every operation
below assumes this has just been called.

---

## 3. ERASE (mass-erase both cores via CTRL-AP)

Reference: `pyocd_diag.py::_erase_ctrl_ap_sync()` @ line 570.

```
ERASE_ALL:
  dp_full_wake()

  # Discover Nordic CTRL-APs (IDR identifies them)
  ctrl_aps = []
  for ap_index in 0..7:
    DP.SELECT  ← (ap_index << 24) | 0xF0       # bank 0xF for IDR
    idr = AP_read(ap_index, 0xFC)
    if idr in {0x02880000, 0x12880000}:        # Nordic CTRL-AP IDR
      ctrl_aps.append(ap_index)
  # Expected on nRF5340: ctrl_aps = [2, 3]  (App ctrl-AP=2, Net ctrl-AP=3)

  for ap in ctrl_aps:
    DP.SELECT       ← (ap << 24) | 0x00        # bank 0 of this CTRL-AP

    # Defensive RESET pulse — clears any stuck state
    AP_write(ap, 0x00, 1);   sleep 20 ms
    AP_write(ap, 0x00, 0);   sleep 20 ms

    AP_write(ap, 0x04, 1)                      # ERASEALL ← 1
    deadline = now + 10 s
    while now < deadline:
      sleep 20 ms
      status = AP_read(ap, 0x08)
      if status == 0: break
    if timeout: record failure, continue to next ap

  # Clean up DP at end (sticky bits may have been set by the CTRL-AP ops)
  dp_sticky_clear()
  dp_power_up()
```

**Bug 6b fix baked in**: the loop body re-selects DP between APs.
`dp_sticky_clear()` and `dp_power_up()` are called at the end so the next
operation starts from clean DP state.

---

## 4. RECOVER (full unlock + UICR programming)

Reference: `pyocd_diag.py::_recover_sync()` @ line 1494.

Eight-stage flow. Run ERASE first if the chip is fully locked; RECOVER
assumes both cores' flash + UICR are already wiped.

```
RECOVER:

  # Stage 1 — Full DP wake
  dp_full_wake()

  # Stage 2 — ERASEALL both cores
  for ap in [2, 3]:                            # App CTRL-AP, Net CTRL-AP
    DP.SELECT ← (ap << 24) | 0x00
    # Defensive RESET pulse
    AP_write(ap, 0x00, 1); sleep 20 ms
    AP_write(ap, 0x00, 0); sleep 20 ms
    # ERASEALL
    AP_write(ap, 0x04, 1)
    poll AP_read(ap, 0x08) == 0, timeout 10 s

  # Stage 3 — Program App UICR.APPROTECT via host-side NVMC writes
  #          (App AHB-AP, CSW=0x23000002)
  dp_sticky_clear()                            # ← FIX 6b: clear sticky before AP switch
  app_wait_ready(0.5 s)                        # NVMC.READY @ 0x50039400 bit 0
  app_nvmc_set_config(1)                       # CONFIG ← Wen
  app_wait_ready(0.5 s)
  app_mem_write(0x00FF8000, 0x50FA50FA)        # UICR.APPROTECT
  app_wait_ready(0.5 s)
  app_mem_write(0x00FF801C, 0x50FA50FA)        # UICR.SECUREAPPROTECT
  app_wait_ready(0.5 s)
  app_nvmc_set_config(0)                       # CONFIG ← Ren
  app_wait_ready(0.5 s)

  # Stage 4 — Release Net core (App-side write)
  app_mem_write(0x50005614, 0)                 # RESET.NETWORK.FORCEOFF ← 0

  # Stage 5 — Write Net stub to Net flash[0x01000000] via Net AHB-AP + Net NVMC
  dp_sticky_clear()                            # ← FIX 6b: clear before Net AHB-AP
  dp_power_up()                                # ← FIX 6b: re-arm DP power
  net_wait_ready(0.5 s)                        # Net NVMC.READY
  net_nvmc_set_config(1)                       # Net NVMC.CONFIG ← Wen
  net_wait_ready(0.5 s)
  for each 32-bit word in NET_STUB:
    if word == 0xFFFFFFFF: continue            # skip erased default
    net_mem_write(dest_addr, word)
    net_wait_ready(0.1 s)
    if timeout: net_nvmc_set_config(0); fail
  net_nvmc_set_config(0)                       # CONFIG ← Ren
  net_wait_ready(0.5 s)

  # Stage 6 — Reset Net core to launch stub
  DP.SELECT ← (3 << 24) | 0x00
  AP_write(3, 0x00, 1); sleep 50 ms            # CTRL-AP#3.RESET ← 1
  AP_write(3, 0x00, 0); sleep 200 ms           # release, give stub time to run

  # Stage 7 — Verify Net stub completion via SRAM marker
  dp_full_wake()                               # ← critical after CTRL-AP RESET
  marker = net_mem_read(0x21000000)
  if marker != 0xDEADC0DE: record failure

  # Stage 8 — Verify Net UICR.APPROTECT persisted
  uicr = net_mem_read(0x01FF8000)
  if uicr != 0x50FA50FA: record failure

  return OK
```

**Bug 6b fix (Net AHB-AP fault mid-flow)**: Stage 5 in the Python today
relies on `_full_dp_wake()` from Stage 1 still being fresh — but after
ERASEALL + App UICR program, sticky bits have accumulated. The C version
explicitly clears them at Stage 3 + Stage 5 boundaries.

---

## 5. WRITE (App + Net flash)

### 5a. App flash

Host-side approach delegates to probe-rs subprocess. For the Zephyr port
this becomes the standard CMSIS Flash Algorithm pattern (RAM-loaded
Thumb loader, called via DAP_Transfer). Same shape as the existing
edevkit flash loader at `loader/nrf_flash_loader.S`.

### 5b. Net flash (direct AP#1 + Net NVMC)

Reference: `pyocd_diag.py::_program_net_flash_sync()` @ line 1113.

```
NET_FLASH_WRITE(segments):
  # Precondition: caller has just done CTRL-AP#3 ERASEALL
  # Net flash is 0xFF, Net AHB-AP#1 unlocked, no reset fired yet.

  dp_full_wake()

  # Defensive CTRL-AP#3 RESET pulse
  DP.SELECT ← (3 << 24) | 0x00
  AP_write(3, 0x00, 1); sleep 20 ms
  AP_write(3, 0x00, 0); sleep 20 ms

  # CTRL-AP#3 ERASEALL again (idempotent; ensures Net AHB-AP unlocked)
  AP_write(3, 0x04, 1)
  poll AP_read(3, 0x08) == 0, timeout 10 s

  # Set up Net AHB-AP#1
  dp_sticky_clear()                            # ← FIX 6b
  DP.SELECT ← (1 << 24) | 0x00
  AP_write(1, 0x00, 0x03800042)                # Net CSW

  # Enable NVMC write mode
  net_wait_ready(2 s)
  net_nvmc_set_config(1)                       # Wen
  net_wait_ready(2 s)

  # Program data (per-word TAR + DRW)
  for each segment (base_addr, buf):
    pad buf to 4-byte boundary with 0xFF
    for addr, word in (base_addr, buf) in 4-byte stride:
      if word == 0xFFFFFFFF: continue          # skip erased default
      net_mem_write(addr, word)
      net_wait_ready(0.1 s)
      if timeout:
        net_nvmc_set_config(0)
        fail "NVMC stuck @ {addr}"

  net_nvmc_set_config(0)                       # Ren

  # Inline verify (NO reset between program and verify — Nordic nan_041/nan_042)
  for each segment (base_addr, buf):
    for addr, expected in (base_addr, buf) in 4-byte stride:
      actual = net_mem_read(addr)
      if actual != expected: fail "verify mismatch @ {addr}"

  # Stage 9: Program Net UICR.APPROTECT
  net_nvmc_set_config(1)
  net_wait_ready(0.5 s)
  net_mem_write(0x01FF8000, 0x50FA50FA)
  net_wait_ready(0.5 s)
  net_nvmc_set_config(0)

  # Verify
  uicr = net_mem_read(0x01FF8000)
  if uicr != 0x50FA50FA: fail

  # Stage 10: Program App UICR.APPROTECT/SECUREAPPROTECT (same DP)
  dp_sticky_clear()                            # ← FIX 6b
  DP.SELECT ← (0 << 24) | 0x00
  AP_write(0, 0x00, 0x23000002)                # App CSW
  app_wait_ready(0.5 s)
  app_nvmc_set_config(1)
  app_wait_ready(0.5 s)
  app_mem_write(0x00FF8000, 0x50FA50FA)
  app_wait_ready(0.5 s)
  app_mem_write(0x00FF801C, 0x50FA50FA)
  app_wait_ready(0.5 s)
  app_nvmc_set_config(0)

  return OK
```

---

## 6. READ (direct AHB-AP memory read)

Reference: `pyocd_diag.py::_read_memory_sync()` @ line 1031.

The key property: bypasses any probe-rs / pyocd auto-unlock vendor
sequence. Works on locked chips and post-erase lockup cores without
destroying flash.

```
READ_MEM(ahb_ap, address, word_count, csw):
  # csw = 0x23000002 for App AHB-AP, 0x03800042 for Net AHB-AP

  # Minimal DP setup — no full_dp_wake here (don't want to reset state)
  dp.connect()        # may fail silently — swallow
  dp_power_up()       # may fail silently — swallow

  output = []
  for i in 0..word_count:
    addr = address + i*4

    DP.SELECT ← (ahb_ap << 24) | 0x00          # bank 0
    AP_write(ahb_ap, 0x00, csw)                # CSW (every word — paranoid)
    AP_write(ahb_ap, 0x04, addr)               # TAR
    AP_read(ahb_ap, 0x0C)                      # post DRW (discard)
    val = DP_read(0x0C)                        # RDBUFF (actual data)
    output.append(val)

  return output
```

**Why TAR per word (no auto-increment)?** Nordic chips wrap TAR at 1 KB
boundaries with auto-inc enabled — easy to hit on a 4 KB read. Per-word
TAR is ~5% slower over USB but always correct.

---

## 7. Net stub (`nrf53_net_disable_approtect.s`, 116 B)

Lives in Net flash at `0x01000000`. Runs on Net CPU after CTRL-AP#3
RESET → boots from flash[0] → executes from the reset vector.

Reference: `nrf53_net_disable_approtect.s`, full ASM in the source repo.

```
Vector table (at 0x01000000):
  [0x00] MSP_init    = 0x21010000          (Net SRAM top)
  [0x04] Reset       = reset_handler|1     (Thumb)
  [0x08..0x3C] all faults → spin loop      (BKPT then b .)

SRAM progress markers (Net stub writes these):
  0x21000000  stage progress
  0x21000004  0xBADF00D5 if a fault fired
  0x21000008  faulted PC
  0x2100000C  faulted xPSR

reset_handler:
  1. write 0x11111111 → 0x21000000          (marker: entered)
  2. poll NVMC.READY @ 0x41080400
  3. NVMC.CONFIG @ 0x41080504 ← 1 (Wen)
  4. poll NVMC.READY
  5. write 0x22222222 → 0x21000000          (marker: Wen acknowledged)
  6. UICR.APPROTECT @ 0x01FF8000 ← 0x50FA50FA
  7. poll NVMC.READY
  8. NVMC.CONFIG @ 0x41080504 ← 0 (Ren)
  9. poll NVMC.READY
 10. write 0xDEADC0DE → 0x21000000          (marker: complete)
 11. spin forever (no BKPT — C_DEBUGEN may not be set yet)

fault_handler:
  1. write 0xBADF00D5 → 0x21000004
  2. write PC → 0x21000008                   (from stack frame)
  3. write xPSR → 0x2100000C
  4. spin forever
```

**Why every fault handler exists**: per
`reference_nrfjprog_stub_disassembly_2026_06_26`, the root cause of the
"persistent lockup PC=0xEFFFFFFE" symptom in earlier iterations was
**missing fault handlers**. Without them, any peripheral access fault
dispatches to a vector slot whose default content (post-erase) is
`0xFFFFFFFE`, causing a double-fault → lockup. The fix is install
spin/BKPT handlers at every vector slot — exactly what nrfjprog's
3748-byte stub does, just compressed into 116 bytes.

---

## 8. The three known bugs — root cause + fix

### Bug 6a — App readback shows `0xFFFFFFFF` post-reset

**Symptom**: After RECOVER → FLASH → CTRL-AP RESET, reading App flash
returns 0xFFs even via direct pyocd. Chip IS correctly programmed (Net
verified byte-for-byte; App UICR shows 0x50FA50FA).

**Root cause (from the algorithm extraction)**: In the current Python flow,
`DEMCR.VC_CORERESET` is set to 1 before the CTRL-AP RESET (line 912) but
**never cleared after** (line 922). The CPU stays halted at the reset
vector; the flash controller hasn't fully initialized for read access yet
when the readback fires.

**Fix in C**: After CTRL-AP RESET and stub completion, before any readback:
```
ap_write(DEMCR @ 0xE000EDFC, 0x00000000)   # VC_CORERESET ← 0
sleep 50 ms                                # flash controller init
# now readback works
```

### Bug 6b — Net AHB-AP fault mid-flow

**Symptom**: Net AHB-AP writes (CSW=0x03800042) work standalone but
FAULT with "SWD/JTAG communication failure (FAULT ACK)" when invoked
inside the multi-step RECOVER or ERASE flow.

**Root cause**: After CTRL-AP operations (ERASEALL, RESET), sticky error
bits accumulate in DP.CTRL/STAT and the DP's power-up request can regress.
The Python `_write_blob_via_nvmc()` does NOT clear them before starting
Net AHB-AP writes (relies on a stale earlier `_full_dp_wake()`).

**Fix in C**: between every CTRL-AP op and any sustained AHB-AP work:
```
dp_sticky_clear()      # DP.ABORT ← 0x0000001E
dp_power_up()          # CTRL/STAT ← 0x0000000F; poll ACK bits
```

Specific insertion points (per algorithm above):
- Stage 3 of RECOVER (before App UICR write)
- Stage 5 of RECOVER (before Net stub install)
- Stage 7 of RECOVER (after CTRL-AP RESET, before SRAM marker read)
- Stage 10 of WRITE (before App UICR after Net done)

### Bug 6c — WAIT-ACK retry exceeds USB transfer timeout

**Symptom**: When the target stalls (especially mid-recovery), the in-
protocol WAIT-ACK retries cumulatively exceed the host's USB transfer
timeout (~1 s for our DAP backend). The host sees `RPC_ERR_DAP` even
though the SWD link is healthy.

**Root causes**:
1. Default DAP transfer timeout is ~100 ms in pyocd; long NVMC polls
   amplify this.
2. No progress responses between long polls — the host gets one final
   "result" packet at the end, nothing in the middle.

**Fix in C** (probe-side):
- No single AP transaction blocks > 100 ms.
- Long polls (`*_wait_ready`) chunk into 5-10 ms slices that yield to
  the USB poll task.
- Long operations emit a progress packet every ~256 words or every 50 ms
  (whichever first) — like the dap_ble_bridge `FLASH_READ_FULL_CHUNK`
  unsolicited notifications (`0x4050`).
- Host-side USB timeout: 5 s for recovery-class commands, 1 s for
  per-transfer ops.

---

## 9. Implementation checklist for the Zephyr port

Suggested file layout under `firmware/projects/edev_dapv2_zephyr/`:

```
src/nrf5340/
  nrf53.h              # public API (recover/erase/write/read)
  nrf53.c              # the four entry points + orchestration
  nrf53_dp.c           # DP layer: full_wake, sticky_clear, power_up
  nrf53_ap.c           # AP layer: read/write with TAR/DRW abstraction
  nrf53_ctrl_ap.c      # Nordic CTRL-AP: ERASEALL, RESET
  nrf53_nvmc.c         # NVMC sequencing (App + Net helpers)
  nrf53_uicr.c         # UICR.APPROTECT/SECUREAPPROTECT programming
  nrf53_stubs.c        # NET_STUB binary blob + verify-by-SRAM-marker
  nrf53_stubs/net_disable_approtect.s   # source for the stub
  nrf53_progress.c     # progress packet emission
  Kconfig              # CONFIG_NRF53_OPS
  CMakeLists.txt
```

Dispatcher integration (CMSIS-DAP vendor commands `0x84..0x8B`):

| DAP vendor cmd | Operation |
|---|---|
| `0x84` | `NRF53_RECOVER` (8-stage flow) |
| `0x85` | `NRF53_ERASE` (CTRL-AP ERASEALL both cores) |
| `0x86` | `NRF53_FLASH_WRITE_NET` (segments arrive as CMSIS-DAP packet payloads) |
| `0x87` | `NRF53_FLASH_WRITE_APP` (uses CMSIS Flash Algo) |
| `0x88` | `NRF53_READ_MEM` (ahb_ap, addr, len, csw) — bypasses host probe-rs |
| `0x89` | `NRF53_TARGET_INFO` (DPIDR, AP_IDR, FICR.PART) |
| `0x8A` | `NRF53_UICR_PROGRAM_APP` (low-level — just App UICR) |
| `0x8B` | `NRF53_UICR_PROGRAM_NET` (low-level — Net via stub) |

The high-level ESP32 RPC layer (`docs/ESP32_BRIDGE.md` §11–12) forwards
to these as opaque vendor packets; the bridge does no orchestration.

### Build-order (each step lands a working firmware)

1. **`nrf53_dp.c` + `nrf53_ap.c`** — DP/AP primitives. Validates against
   `RPC_DPID` / `RPC_ATTACH`.
2. **`nrf53_ctrl_ap.c`** — exposes `NRF53_ERASE` vendor command.
   Validate: erase a locked Ring → confirm UICR reads as 0xFFFFFFFF.
3. **`nrf53_uicr.c` + `nrf53_nvmc.c`** — exposes `NRF53_UICR_PROGRAM_APP`
   (host-side, no stub needed). Validate: post-erase, read UICR.APPROTECT
   = 0x50FA50FA.
4. **`nrf53_stubs.c`** + assembled `.S` → blob. Exposes
   `NRF53_UICR_PROGRAM_NET` (loads + executes stub). Validate: SRAM marker
   = 0xDEADC0DE after CTRL-AP RESET.
5. **`NRF53_RECOVER`** — composes 1–4. End-to-end against Ring Pro 351.
6. **`NRF53_FLASH_WRITE_NET`** — Net flash via direct AP#1 + NVMC.
   Validate: byte-for-byte verify of merged hex.
7. **`NRF53_FLASH_WRITE_APP`** — RAM-loaded loader (CMSIS Flash Algo).
   Validate: byte-for-byte verify with halt-at-reset.
8. **`NRF53_READ_MEM`** — direct AHB-AP read with **VC_CORERESET clear**
   (Fix 6a). Validate: App readback returns real vectors, not 0xFF.
9. **Progress packets** (Fix 6c) — emit `0x4050`-style notifications
   every 256 words during long ops.
10. **End-to-end smoke**: ERASE → FLASH App+Net → READ verify. Compare
    md5 to `uhocd dump-flash` for byte-identical confirmation.

---

## 10. Acceptance criteria (per operation)

| Op | Acceptance |
|---|---|
| RECOVER | Ring Pro 351 (locked, wedged) → unlocked → Net UICR=0x50FA50FA → App UICR=0x50FA50FA. Total time < 5 s. |
| ERASE | Full ERASEALL both cores. Subsequent read of any flash word = 0xFFFFFFFF. Time < 2 s. |
| WRITE (Net) | 175 KB merged Net image → byte-for-byte readback match. Time < 30 s. |
| WRITE (App) | 512 KB merged App image → byte-for-byte readback match (after VC_CORERESET clear). Time < 60 s. |
| READ | Locked nRF5340 → direct AHB-AP read returns DPIDR/AP_IDR even when AHB-AP locked. Halt-at-reset variant returns reset vector contents. |

If any acceptance fails, cross-check against the Python reference: run
`uhocd` (the pyocd fork) against the same Pico over the same USB cable
and compare bit-for-bit.
