#!/usr/bin/env python3
"""
_flash_ops_sequence.py — quick driver for the canonical flow on
nRF5340: info → read → erase → recover → write → read-verify.

Reuses test_nrf53_vendor_cmds.py primitives. Not part of the
acceptance suite; this is an interactive driver for ad-hoc
validation on bench.
"""
import struct
import sys
import time

from test_nrf53_vendor_cmds import (
    Probe, fmt_status,
    CMD_RECOVER, CMD_ERASE, CMD_FLASH_WRITE_NET, CMD_FLASH_WRITE_APP,
    CMD_TARGET_INFO, CMD_READ_MEM, CMD_WRITE_MEM,
    UNLOCK_VAL,
    dap_connect_and_reset,
)

GREEN = lambda s: f"\033[32m{s}\033[0m" if sys.stdout.isatty() else s
RED   = lambda s: f"\033[31m{s}\033[0m" if sys.stdout.isatty() else s
CYAN  = lambda s: f"\033[36m{s}\033[0m" if sys.stdout.isatty() else s
YELL  = lambda s: f"\033[33m{s}\033[0m" if sys.stdout.isatty() else s

APP_CSW = 0x23000002
NET_CSW = 0x03800042

def phase(num, total, label):
    print()
    print(CYAN(f"━━ {num}/{total}  {label} ━━"))

def ok(label, detail=""):  print(f"  {GREEN('OK  ')} {label}" + (f"   {detail}" if detail else ""))
def fail(label, detail=""): print(f"  {RED('FAIL')} {label}" + (f"   {detail}" if detail else "")); sys.exit(2)
def info(label):           print(f"  {label}")

def read_mem(p, csw, addr, n_words, timeout_ms=5000):
    ap_index = 0 if csw == APP_CSW else 1
    req = (bytes([CMD_READ_MEM, 0x00, ap_index])
           + struct.pack("<II", csw, addr)
           + struct.pack("<H", n_words))
    resp = p.transfer(req, timeout_ms=timeout_ms)
    if resp[0] != 0x88 or resp[1] != 0:
        fail(f"READ_MEM @ 0x{addr:08x}", fmt_status(resp[1]))
    return struct.unpack(f"<{n_words}I", resp[2:2 + 4*n_words])

def write_mem(p, csw, addr, words, timeout_ms=5000):
    ap_index = 0 if csw == APP_CSW else 1
    req = (bytes([CMD_WRITE_MEM, 0x00, ap_index])
           + struct.pack("<II", csw, addr)
           + struct.pack("<H", len(words))
           + b"".join(struct.pack("<I", w) for w in words))
    resp = p.transfer(req, timeout_ms=timeout_ms)
    if resp[0] != 0x8C or resp[1] != 0:
        fail(f"WRITE_MEM @ 0x{addr:08x}", fmt_status(resp[1]))


def main():
    p = Probe()
    try:
        dap_connect_and_reset(p)

        # ── 1 INFO ─────────────────────────────────────────────────
        phase(1, 5, "FLASH INFO — chip identification (0x89 TARGET_INFO)")
        t0 = time.time()
        resp = p.transfer([CMD_TARGET_INFO], timeout_ms=3000)
        dt = time.time() - t0
        if len(resp) != 14 or resp[0] != 0x89 or resp[1] != 0:
            fail("TARGET_INFO", f"len={len(resp)} status={fmt_status(resp[1])}")
        dpidr, ap_idr, cpuid = struct.unpack("<III", resp[2:14])
        designer = (dpidr >> 1) & 0x7FF
        version  = (dpidr >> 12) & 0xF
        cpuid_part = (cpuid >> 4) & 0xFFF
        info(f"DPIDR        = 0x{dpidr:08x}  (designer=0x{designer:03x}, version=DPv{version})")
        info(f"AHB-AP[0].IDR= 0x{ap_idr:08x}")
        info(f"CPUID        = 0x{cpuid:08x}  (PART=0x{cpuid_part:03x} → Cortex-M{ 'M33' if cpuid_part==0xD21 else 'M?' })")
        if dpidr == 0x6BA02477 and cpuid_part == 0xD21:
            ok("Identified as nRF5340 (DPv2, Cortex-M33)", f"{dt*1000:.0f} ms")
        else:
            fail("Unexpected chip", f"DPIDR=0x{dpidr:08x} CPUID_PART=0x{cpuid_part:03x}")

        # ── 2 READ ─────────────────────────────────────────────────
        phase(2, 5, "FLASH READ — first 8 words of App + Net flash (0x88 READ_MEM)")

        t0 = time.time()
        app_pre = read_mem(p, APP_CSW, 0x00000000, 8)
        dt = time.time() - t0
        info(f"App flash @ 0x00000000:")
        for i, w in enumerate(app_pre):
            info(f"    [{i:02d}] 0x{w:08x}")
        ok("Read 8 words App flash", f"{dt*1000:.0f} ms")

        t0 = time.time()
        net_pre = read_mem(p, NET_CSW, 0x01000000, 8)
        dt = time.time() - t0
        info(f"Net flash @ 0x01000000:")
        for i, w in enumerate(net_pre):
            info(f"    [{i:02d}] 0x{w:08x}")
        ok("Read 8 words Net flash", f"{dt*1000:.0f} ms")

        # ── 3 ERASE ────────────────────────────────────────────────
        phase(3, 5, "FLASH ERASE — CTRL-AP ERASEALL on both cores (0x85 NRF53_ERASE)")
        t0 = time.time()
        resp = p.transfer([CMD_ERASE], timeout_ms=20000)
        dt = time.time() - t0
        if len(resp) != 3 or resp[0] != 0x85 or resp[1] != 0:
            fail("ERASE", fmt_status(resp[1]))
        ap_count = resp[2]
        ok(f"ERASEALL completed", f"{ap_count} CTRL-APs, {dt:.2f}s")

        # Verify both cores wiped
        app_post = read_mem(p, APP_CSW, 0x00000000, 8)
        net_post = read_mem(p, NET_CSW, 0x01000000, 8)
        if all(w == 0xFFFFFFFF for w in app_post) and all(w == 0xFFFFFFFF for w in net_post):
            ok("Post-erase readback = 0xFFFFFFFF on both cores")
        else:
            info(f"App[0..7] = {[f'0x{w:08x}' for w in app_post]}")
            info(f"Net[0..7] = {[f'0x{w:08x}' for w in net_post]}")
            fail("Post-erase readback contains non-FF words")

        # ── 4 RECOVER ──────────────────────────────────────────────
        phase(4, 5, "FLASH RECOVER — full unlock chain (0x84 NRF53_RECOVER)")
        t0 = time.time()
        resp = p.transfer([CMD_RECOVER], timeout_ms=30000)
        dt = time.time() - t0
        if len(resp) != 19 or resp[0] != 0x84 or resp[1] != 0:
            fail("RECOVER", fmt_status(resp[1]))
        ap_count = resp[2]
        app_app, app_sec, net_marker, net_app = struct.unpack("<IIII", resp[3:19])
        info(f"App APPROTECT       = 0x{app_app:08x}  (expect 0x{UNLOCK_VAL:08x})")
        info(f"App SECUREAPPROTECT = 0x{app_sec:08x}  (expect 0x{UNLOCK_VAL:08x})")
        info(f"Net SRAM marker     = 0x{net_marker:08x}  (DK silicon: 0x00000000, prod: 0xDEADC0DE)")
        info(f"Net UICR.APPROTECT  = 0x{net_app:08x}  (expect 0x{UNLOCK_VAL:08x})")
        if app_app == UNLOCK_VAL and app_sec == UNLOCK_VAL and net_app == UNLOCK_VAL:
            ok("RECOVER complete — all UICRs unlocked", f"{ap_count} APs, {dt:.2f}s")
        else:
            fail("UICR readback mismatch")

        # ── 5 WRITE ────────────────────────────────────────────────
        phase(5, 5, "FLASH WRITE — pattern programming on App + Net (0x86 / 0x87)")

        # App: 13-word pattern at 0xF000 (well past bootloader area)
        app_addr = 0x0000F000
        app_pattern = [0xA0A00000 | i for i in range(13)]
        t0 = time.time()
        req = bytes([CMD_FLASH_WRITE_APP]) + struct.pack(
            "<IIH", 0x50039000, app_addr, len(app_pattern))
        req += b"".join(struct.pack("<I", w) for w in app_pattern)
        resp = p.transfer(req, timeout_ms=3000)
        dt = time.time() - t0
        if resp[0] != 0x87 or resp[1] != 0:
            fail("FLASH_WRITE_APP", fmt_status(resp[1]))
        words_written = struct.unpack("<H", resp[2:4])[0]
        ok(f"App write @ 0x{app_addr:08x}", f"{words_written}/13 words, {dt*1000:.0f} ms")

        app_back = read_mem(p, APP_CSW, app_addr, len(app_pattern))
        if list(app_back) == app_pattern:
            ok("App write readback bit-perfect")
        else:
            info(f"Expected: {[f'0x{w:08x}' for w in app_pattern]}")
            info(f"Got:      {[f'0x{w:08x}' for w in app_back]}")
            fail("App write readback mismatch")

        # Net: 14-word pattern at 0x01000800 (well within Net flash, just erased).
        # The very first words (0x01000000+) include reset/vector area —
        # matching the test_nrf53_vendor_cmds.py working test address.
        net_addr = 0x01000800
        net_pattern = [0xB0B00000 | i for i in range(14)]
        t0 = time.time()
        req = bytes([CMD_FLASH_WRITE_NET]) + struct.pack(
            "<IH", net_addr, len(net_pattern))
        req += b"".join(struct.pack("<I", w) for w in net_pattern)
        resp = p.transfer(req, timeout_ms=3000)
        dt = time.time() - t0
        if resp[0] != 0x86 or resp[1] != 0:
            fail("FLASH_WRITE_NET", fmt_status(resp[1]))
        words_written = struct.unpack("<H", resp[2:4])[0]
        ok(f"Net write @ 0x{net_addr:08x}", f"{words_written}/14 words, {dt*1000:.0f} ms")

        net_back = read_mem(p, NET_CSW, net_addr, len(net_pattern))
        if list(net_back) == net_pattern:
            ok("Net write readback bit-perfect")
        else:
            info(f"Expected: {[f'0x{w:08x}' for w in net_pattern]}")
            info(f"Got:      {[f'0x{w:08x}' for w in net_back]}")
            fail("Net write readback mismatch")

        # ── Done ───────────────────────────────────────────────────
        print()
        print(GREEN("═══════════════════════════════════════════════════"))
        print(GREEN("  5/5 phases PASS — info, read, erase, recover, write"))
        print(GREEN("═══════════════════════════════════════════════════"))
        print(f"  Target: nRF5340 DK")
        print(f"  Firmware: edev_dapv2_zephyr v0.1.8 (binary == v0.1.7)")
        print(f"  Date: {time.strftime('%Y-%m-%d %H:%M:%S')}")

    finally:
        p.close()


if __name__ == "__main__":
    main()
