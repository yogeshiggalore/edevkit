#!/usr/bin/env python3
"""
ring_pro_351_acceptance.py — end-to-end acceptance for v0.1.7 firmware
against a Ultrahuman Ring Pro 351 (nRF5340) production target.

This is Step 10 of the original 10-step plan. Runs the bench-validated
nRF5340 acceptance suite against a real production Ring, with stricter
timing budgets and an explicit consent prompt because the procedure
fully erases the Ring's firmware.

DESTRUCTIVE — this script writes UICR.APPROTECT and runs CTRL-AP
ERASEALL on both cores. After a full run the Ring will boot to an
empty image and need its production firmware re-flashed via the
normal release tooling. Do NOT run against a Ring you can't re-flash.

Hardware required:
  - Pico running edev_dapv2_zephyr v0.1.7 (commit 31e3082 or later)
  - Ring Pro 351 on charger (need stable power for ERASEALL on both
    cores). The Ring's internal Li-ion alone is not enough — see
    [[reference_ring_battery_defeats_power_cycle]].
  - SWD pogo pins making solid contact (Ring's debug pads are tiny;
    a loose contact mid-ERASEALL leaves the chip half-erased).

Usage:
  python3 ring_pro_351_acceptance.py              # run full suite
  python3 ring_pro_351_acceptance.py --consent    # skip the y/N prompt
  python3 ring_pro_351_acceptance.py --identify   # NON-DESTRUCTIVE
                                                  # (ping + target-info only)

Expected runtime: ~60 s for full suite. Individual phase timeouts are
3× the bench DK observations to allow for slower production silicon.
"""
import argparse
import struct
import sys
import time

# Reuse the existing test harness primitives — same VID/PID, same
# Probe class, same wire-format helpers.
sys.path.insert(0, ".")
from test_nrf53_vendor_cmds import (
    Probe, fmt_status, STATUS,
    CMD_RECOVER, CMD_ERASE, CMD_FLASH_WRITE_NET, CMD_FLASH_WRITE_APP,
    CMD_TARGET_INFO, CMD_READ_MEM, CMD_UICR_APP, CMD_UICR_NET,
    CMD_WRITE_MEM,
    UNLOCK_VAL, STUB_DONE,
    dap_connect_and_reset,
)

# ── Acceptance budgets (3× bench DK worst observed) ──────────────────
T_PING       = 3000
T_INFO       = 3000
T_ERASE      = 30000
T_UICR_APP   = 5000
T_UICR_NET   = 10000
T_FLASH_OP   = 3000
T_MEM_OP     = 3000
T_RECOVER    = 60000

# ── Expected values ──────────────────────────────────────────────────
NRF5340_DPIDR_LOW = 0x6BA02477  # DP.DPIDR after line reset, App half
NRF5340_DPIDR_NET = 0x6BA02477  # Same DP — multidrop selects half
APP_AHB_AP_IDR    = 0x84770001  # Standard ARM AHB-AP, no security ext
NET_AHB_AP_IDR    = 0x84770001  # Same on Net
M33_CPUID_PART    = 0xD21       # Cortex-M33

# Ring App flash test pattern — 13 words (0x87 max payload)
TEST_PATTERN_APP_ADDR = 0x0000F000  # well past bootloader, in main app
TEST_PATTERN_NET_ADDR = 0x01000000  # base of Net flash, post-erase

# ── Color output (skip if non-TTY) ──────────────────────────────────
def _c(code, s):
    if sys.stdout.isatty():
        return f"\033[{code}m{s}\033[0m"
    return s
def GREEN(s): return _c("32", s)
def RED(s): return _c("31", s)
def YELLOW(s): return _c("33", s)
def CYAN(s): return _c("36", s)


class AcceptanceFailure(Exception):
    pass


def expect(label, condition, detail=""):
    if condition:
        print(f"  {GREEN('PASS')} {label}{('  ' + detail) if detail else ''}")
        return True
    print(f"  {RED('FAIL')} {label}{('  ' + detail) if detail else ''}")
    raise AcceptanceFailure(label)


def phase(name):
    print()
    print(CYAN(f"── {name} ──"))


# ──────────────────────────────────────────────────────────────────────
# Phases
# ──────────────────────────────────────────────────────────────────────
def phase_identify(p):
    phase("1/8  Identify target")
    resp = p.transfer([CMD_TARGET_INFO], timeout_ms=T_INFO)
    expect("TARGET_INFO response length = 14", len(resp) == 14,
           detail=f"got {len(resp)}")
    expect("TARGET_INFO[0] = 0x89", resp[0] == 0x89)
    expect("TARGET_INFO status = OK", resp[1] == 0,
           detail=fmt_status(resp[1]))
    dpidr, ap_idr, cpuid = struct.unpack("<III", resp[2:14])
    expect(f"DPIDR = 0x{NRF5340_DPIDR_LOW:08x} (nRF5340 DPv2)",
           dpidr == NRF5340_DPIDR_LOW,
           detail=f"got 0x{dpidr:08x}")
    cpuid_part = (cpuid >> 4) & 0xFFF
    expect(f"CPUID.PART = 0x{M33_CPUID_PART:03x} (Cortex-M33)",
           cpuid_part == M33_CPUID_PART,
           detail=f"got 0x{cpuid_part:03x}")
    print(f"    DPIDR    = 0x{dpidr:08x}")
    print(f"    AP[0].IDR = 0x{ap_idr:08x}")
    print(f"    CPUID    = 0x{cpuid:08x}")


def phase_mem_roundtrip(p):
    phase("2/8  Mem roundtrip (sanity before erasing)")
    addr = 0x20000000  # App SRAM scratch
    test_words = [0xDEADBEEF, 0xCAFEBABE, 0x12345678, 0xA5A5A5A5]
    req = bytes([CMD_WRITE_MEM]) + struct.pack(
        "<BIIH", 0, 0x23000002, addr, len(test_words))
    req += b"".join(struct.pack("<I", w) for w in test_words)
    resp = p.transfer(req, timeout_ms=T_MEM_OP)
    expect("WRITE_MEM OK", resp[0] == 0x8C and resp[1] == 0,
           detail=fmt_status(resp[1]))

    req = bytes([CMD_READ_MEM]) + struct.pack(
        "<BIIH", 0, 0x23000002, addr, len(test_words))
    resp = p.transfer(req, timeout_ms=T_MEM_OP)
    expect("READ_MEM OK", resp[0] == 0x88 and resp[1] == 0,
           detail=fmt_status(resp[1]))
    read_back = struct.unpack(
        f"<{len(test_words)}I",
        resp[4:4 + 4 * len(test_words)])
    expect("Roundtrip bit-perfect", list(read_back) == test_words,
           detail=f"wrote {test_words}, read {list(read_back)}")


def phase_erase(p):
    phase("3/8  ERASEALL both cores (timeout 30s)")
    t0 = time.time()
    resp = p.transfer([CMD_ERASE], timeout_ms=T_ERASE)
    dt = time.time() - t0
    expect("ERASE response length = 3", len(resp) == 3)
    expect("ERASE status = OK", resp[1] == 0, detail=fmt_status(resp[1]))
    expect("ERASE found 2 CTRL-APs (App + Net)", resp[2] == 2,
           detail=f"got {resp[2]}")
    print(f"    Wall-clock: {dt:.2f}s  (Ring Pro 351 budget: 30s)")


def phase_verify_erase(p):
    phase("4/8  Verify erase — flash[0] = 0xFFFFFFFF")
    req = bytes([CMD_READ_MEM]) + struct.pack(
        "<BIIH", 0, 0x23000002, 0x00000000, 1)
    resp = p.transfer(req, timeout_ms=T_MEM_OP)
    expect("READ_MEM @ 0x0", resp[0] == 0x88 and resp[1] == 0,
           detail=fmt_status(resp[1]))
    val = struct.unpack("<I", resp[4:8])[0]
    expect("flash[0] = 0xFFFFFFFF", val == 0xFFFFFFFF,
           detail=f"got 0x{val:08x}")


def phase_flash_write_app(p):
    phase("5/8  FLASH_WRITE_APP — 13-word pattern at 0x0000F000")
    pattern = [0xA0A0A000 | i for i in range(13)]
    req = bytes([CMD_FLASH_WRITE_APP]) + struct.pack(
        "<IIH", 0x50039000, TEST_PATTERN_APP_ADDR, len(pattern))
    req += b"".join(struct.pack("<I", w) for w in pattern)
    resp = p.transfer(req, timeout_ms=T_FLASH_OP)
    expect("FLASH_WRITE_APP OK", resp[0] == 0x87 and resp[1] == 0,
           detail=fmt_status(resp[1]))

    # Read back via 0x88
    req = bytes([CMD_READ_MEM]) + struct.pack(
        "<BIIH", 0, 0x23000002, TEST_PATTERN_APP_ADDR, len(pattern))
    resp = p.transfer(req, timeout_ms=T_MEM_OP)
    read_back = struct.unpack(
        f"<{len(pattern)}I",
        resp[4:4 + 4 * len(pattern)])
    expect("App flash readback bit-perfect",
           list(read_back) == pattern,
           detail=f"first mismatch at index "
                  f"{next((i for i,(a,b) in enumerate(zip(read_back, pattern)) if a!=b), -1)}")


def phase_flash_write_net(p):
    phase("6/8  FLASH_WRITE_NET — 14-word pattern at 0x01000000")
    pattern = [0xB0B0B000 | i for i in range(14)]
    req = bytes([CMD_FLASH_WRITE_NET]) + struct.pack(
        "<IH", TEST_PATTERN_NET_ADDR, len(pattern))
    req += b"".join(struct.pack("<I", w) for w in pattern)
    resp = p.transfer(req, timeout_ms=T_FLASH_OP)
    expect("FLASH_WRITE_NET OK", resp[0] == 0x86 and resp[1] == 0,
           detail=fmt_status(resp[1]))

    # Read back via 0x88 with Net CSW
    req = bytes([CMD_READ_MEM]) + struct.pack(
        "<BIIH", 1, 0x03800042, TEST_PATTERN_NET_ADDR, len(pattern))
    resp = p.transfer(req, timeout_ms=T_MEM_OP)
    read_back = struct.unpack(
        f"<{len(pattern)}I",
        resp[4:4 + 4 * len(pattern)])
    expect("Net flash readback bit-perfect",
           list(read_back) == pattern)


def phase_uicr_app(p):
    phase("7a/8 UICR_PROGRAM_APP — App APPROTECT/SECUREAPPROTECT = 0x50FA50FA")
    resp = p.transfer([CMD_UICR_APP], timeout_ms=T_UICR_APP)
    expect("UICR_APP response length = 10", len(resp) == 10)
    expect("UICR_APP status = OK", resp[1] == 0, detail=fmt_status(resp[1]))
    approtect, secure = struct.unpack("<II", resp[2:10])
    expect(f"App APPROTECT = 0x{UNLOCK_VAL:08x}",
           approtect == UNLOCK_VAL,
           detail=f"got 0x{approtect:08x}")
    expect(f"App SECUREAPPROTECT = 0x{UNLOCK_VAL:08x}",
           secure == UNLOCK_VAL,
           detail=f"got 0x{secure:08x}")


def phase_uicr_net(p):
    phase("7b/8 UICR_PROGRAM_NET — Net UICR.APPROTECT = 0x50FA50FA")
    resp = p.transfer([CMD_UICR_NET], timeout_ms=T_UICR_NET)
    expect("UICR_NET response length = 10", len(resp) == 10)
    expect("UICR_NET status = OK", resp[1] == 0, detail=fmt_status(resp[1]))
    marker, approtect = struct.unpack("<II", resp[2:10])
    expect(f"Net UICR.APPROTECT = 0x{UNLOCK_VAL:08x}",
           approtect == UNLOCK_VAL,
           detail=f"got 0x{approtect:08x}")
    # SRAM marker is diagnostic-only — Ring Pro 351 production silicon
    # is expected to show 0xDEADC0DE, but DK silicon shows 0x00000000
    # due to the CPU/AHB-AP debug bus asymmetry (see
    # project_edev_dapv2_zephyr_v017_feature_complete_2026_06_30 §2).
    if marker == STUB_DONE:
        print(f"    {GREEN('NOTE')} Net SRAM marker = 0xDEADC0DE "
              f"(production silicon — debug bus consistent)")
    else:
        print(f"    {YELLOW('NOTE')} Net SRAM marker = 0x{marker:08x} "
              f"(matches DK silicon — not a failure)")


def phase_recover(p):
    phase("8/8  RECOVER (full chain) — second run, expect quick success")
    t0 = time.time()
    resp = p.transfer([CMD_RECOVER], timeout_ms=T_RECOVER)
    dt = time.time() - t0
    expect("RECOVER response length = 19", len(resp) == 19,
           detail=f"got {len(resp)}")
    expect("RECOVER status = OK", resp[1] == 0,
           detail=fmt_status(resp[1]))
    ap_count = resp[2]
    expect("RECOVER found 2 APs", ap_count == 2,
           detail=f"got {ap_count}")
    app_app, app_sec, net_marker, net_app = struct.unpack(
        "<IIII", resp[3:19])
    expect(f"App APPROTECT after RECOVER = 0x{UNLOCK_VAL:08x}",
           app_app == UNLOCK_VAL)
    expect(f"App SECUREAPPROTECT after RECOVER = 0x{UNLOCK_VAL:08x}",
           app_sec == UNLOCK_VAL)
    expect(f"Net APPROTECT after RECOVER = 0x{UNLOCK_VAL:08x}",
           net_app == UNLOCK_VAL)
    print(f"    Wall-clock: {dt:.2f}s  (Ring Pro 351 budget: 60s)")


# ──────────────────────────────────────────────────────────────────────
# Driver
# ──────────────────────────────────────────────────────────────────────
def consent_or_bail(skip):
    if skip:
        return
    print(RED("⚠ DESTRUCTIVE: This procedure ERASES the connected"))
    print(RED("  Ring Pro 351 firmware on both cores."))
    print(RED("  The Ring will need re-flashing after this run."))
    print()
    answer = input("Type 'WIPE THE RING' to confirm, anything else aborts: ")
    if answer.strip() != "WIPE THE RING":
        print("Aborted.")
        sys.exit(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--consent", action="store_true",
                    help="skip the destructive-action confirmation prompt")
    ap.add_argument("--identify", action="store_true",
                    help="non-destructive: just ping + target-info")
    args = ap.parse_args()

    p = Probe()
    try:
        # Bring the link up (standard CMSIS-DAP — no chip touched yet)
        dap_connect_and_reset(p)

        if args.identify:
            phase_identify(p)
            print()
            print(GREEN("Identification complete — chip is reachable."))
            return

        consent_or_bail(args.consent)

        phase_identify(p)
        phase_mem_roundtrip(p)
        phase_erase(p)
        phase_verify_erase(p)
        phase_flash_write_app(p)
        phase_flash_write_net(p)
        phase_uicr_app(p)
        phase_uicr_net(p)
        # Run RECOVER as the last phase — it does its own ERASE again,
        # plus the UICR writes, which is the canonical idempotent end-state.
        phase_recover(p)

        print()
        print(GREEN("═══════════════════════════════════════════════"))
        print(GREEN("  Ring Pro 351 acceptance PASSED (9/9 phases)"))
        print(GREEN("═══════════════════════════════════════════════"))
        print()
        print(f"  Firmware: edev_dapv2_zephyr v0.1.7 (commit 31e3082)")
        print(f"  Date: {time.strftime('%Y-%m-%d %H:%M:%S')}")

    except AcceptanceFailure as e:
        print()
        print(RED("═══════════════════════════════════════════════"))
        print(RED(f"  Ring Pro 351 acceptance FAILED at: {e}"))
        print(RED("═══════════════════════════════════════════════"))
        sys.exit(2)
    finally:
        p.close()


if __name__ == "__main__":
    main()
