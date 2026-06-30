#!/usr/bin/env python3
"""
test_nrf53_vendor_cmds.py — hardware acceptance for the edev_dapv2_zephyr
probe firmware. Exercises both standard CMSIS-DAP and the custom
NRF53_* vendor commands (0x84..0x8B) against a real Nordic target,
validates wire-format responses, and reports pass/fail.

Chip-family-aware: ping reads DPIDR + the CTRL-AP count via NRF53_ERASE
to detect nRF52 vs nRF5340, then skips nRF5340-only tests on nRF52.

Requires pyusb + pyserial (`pip install pyusb pyserial`). On macOS may
also need: brew install libusb

Hardware setup:
  - Pico running edev_dapv2_zephyr (≥ commit fa5d305 for nRF52840
    support — earlier builds wedge nRF52840 with malformed dormant
    alert).
  - SWD wires: Pico GPIO2→SWCLK, GPIO3→SWDIO, GND↔GND.
  - Target powered (keep on charger for sealed wearables).

Usage:
  python3 test_nrf53_vendor_cmds.py                # auto: ping + all applicable
  python3 test_nrf53_vendor_cmds.py ping           # just basic link check
  python3 test_nrf53_vendor_cmds.py erase          # NRF53_ERASE
  python3 test_nrf53_vendor_cmds.py uicr-app       # nRF5340-only
  python3 test_nrf53_vendor_cmds.py uicr-net       # nRF5340-only
  python3 test_nrf53_vendor_cmds.py recover        # nRF5340-only

DESTRUCTIVE — most tests wipe the target chip's flash. Don't run
against hardware whose firmware you want to keep.
"""
import argparse
import struct
import sys

import usb.core
import usb.util

VID = 0x2E8A
PID = 0x000C
VENDOR_IFACE_CLASS = 0xFF

# Vendor command bytes
CMD_RECOVER            = 0x84
CMD_ERASE              = 0x85
CMD_FLASH_WRITE_NET    = 0x86
CMD_FLASH_WRITE_APP    = 0x87
CMD_TARGET_INFO        = 0x89
CMD_READ_MEM           = 0x88
CMD_UICR_APP           = 0x8A
CMD_UICR_NET           = 0x8B
CMD_WRITE_MEM          = 0x8C

# nrf53_status_t mirror
STATUS = {
    0: "OK", 1: "WAIT", 2: "FAULT", 3: "NO_ACK", 4: "PROTO",
    5: "TIMEOUT", 6: "ARGS", 7: "NO_DEV", 8: "STUB_FAIL",
}

ID_DAP_INVALID = 0xFF
UNLOCK_VAL = 0x50FA50FA
STUB_DONE = 0xDEADC0DE
STUB_FAULT = 0xBADF00D5


def fmt_status(b):
    return f"{b} ({STATUS.get(b, '?')})"


class Probe:
    def __init__(self):
        dev = usb.core.find(idVendor=VID, idProduct=PID)
        if dev is None:
            raise SystemExit(
                f"No device with VID:PID {VID:04x}:{PID:04x} found.\n"
                f"Plug the Pico into a USB port and retry.")
        try:
            if dev.is_kernel_driver_active(0):
                dev.detach_kernel_driver(0)
        except (NotImplementedError, usb.core.USBError):
            pass

        cfg = dev.get_active_configuration()
        self.itf = None
        for itf in cfg:
            if itf.bInterfaceClass != VENDOR_IFACE_CLASS:
                continue
            eps = list(itf.endpoints())
            bulk = [e for e in eps if usb.util.endpoint_type(
                e.bmAttributes) == usb.util.ENDPOINT_TYPE_BULK]
            if len(bulk) >= 2:
                self.itf = itf
                self.out_ep = next(
                    e for e in bulk
                    if usb.util.endpoint_direction(e.bEndpointAddress)
                    == usb.util.ENDPOINT_OUT)
                self.in_ep = next(
                    e for e in bulk
                    if usb.util.endpoint_direction(e.bEndpointAddress)
                    == usb.util.ENDPOINT_IN)
                break
        if self.itf is None:
            raise SystemExit("No CMSIS-DAP v2 vendor interface found on device.")
        self.dev = dev
        usb.util.claim_interface(dev, self.itf.bInterfaceNumber)

    def close(self):
        try:
            usb.util.release_interface(self.dev, self.itf.bInterfaceNumber)
        except Exception:
            pass

    def transfer(self, cmd_bytes, timeout_ms=15000):
        self.out_ep.write(bytes(cmd_bytes), timeout=timeout_ms)
        resp = self.in_ep.read(512, timeout=timeout_ms)
        return bytes(resp)


# ──────────────────────────────────────────────────────────────────────
# Helpers — drive standard CMSIS-DAP packets so we can read DPIDR + AP
# stuff without relying on our vendor commands. Used by ping + erase-verify.
# ──────────────────────────────────────────────────────────────────────
def dap_connect_and_reset(p, clock_hz=1_000_000):
    """DAP_Connect → SWJ_Clock → line reset + JTAG→SWD + line reset + idle."""
    p.transfer(b'\x02\x01')                                   # DAP_Connect(SWD)
    p.transfer(b'\x11' + struct.pack('<I', clock_hz))         # DAP_SWJ_Clock
    p.transfer(b'\x04\x00' + struct.pack('<HH', 100, 0))      # DAP_TransferConfigure
    # line reset (56) + JTAG-SWD (16) = 72 bits
    p.transfer(b'\x12' + bytes([72]) + b'\xFF'*7 + b'\x9E\xE7')
    # line reset (56) + 8 idle = 64 bits
    p.transfer(b'\x12' + bytes([64]) + b'\xFF'*7 + b'\x00')


def dap_read_dp(p, reg_offset):
    """Read a DP register via DAP_Transfer. Returns (ok, value)."""
    # request byte: APnDP=0 RnW=1 A2A3 from reg_offset bits 2:3
    req = 0x02  # RnW
    if reg_offset & 0x04: req |= 0x04
    if reg_offset & 0x08: req |= 0x08
    r = p.transfer(b'\x05\x00\x01' + bytes([req]))
    # response: [0x05, count, last_ack, data[4]]
    if len(r) >= 7 and r[1] == 1 and r[2] == 1:
        return True, struct.unpack_from('<I', r, 3)[0]
    return False, (r[2] if len(r) > 2 else 0xFF)


def dap_write_dp(p, reg_offset, value):
    req = 0x00  # APnDP=0 RnW=0
    if reg_offset & 0x04: req |= 0x04
    if reg_offset & 0x08: req |= 0x08
    r = p.transfer(b'\x05\x00\x01' + bytes([req]) + struct.pack('<I', value))
    return len(r) >= 3 and r[2] == 1


def dap_read_ap(p, ap_index, reg_offset):
    """Select AP bank, post AP read, flush via DP.RDBUFF."""
    bank = reg_offset & 0xF0
    if not dap_write_dp(p, 0x08, (ap_index << 24) | bank):
        return False, 0
    # APnDP=1 RnW=1
    req = 0x03
    if reg_offset & 0x04: req |= 0x04
    if reg_offset & 0x08: req |= 0x08
    r = p.transfer(b'\x05\x00\x01' + bytes([req]))
    if not (len(r) >= 3 and r[2] == 1):
        return False, (r[2] if len(r) > 2 else 0xFF)
    # discard data, flush via RDBUFF
    return dap_read_dp(p, 0x0C)


def dap_mem_read(p, ap_index, csw, addr):
    """Standard AHB-AP single-word read."""
    if not dap_write_dp(p, 0x08, (ap_index << 24) | 0x00): return False, 0
    # AP.CSW write
    if not (len(p.transfer(b'\x05\x00\x01\x01' + struct.pack('<I', csw))) >= 3): return False, 0
    # AP.TAR write
    if not (len(p.transfer(b'\x05\x00\x01\x05' + struct.pack('<I', addr))) >= 3): return False, 0
    # AP.DRW read (posted)
    r = p.transfer(b'\x05\x00\x01\x0F')
    if not (len(r) >= 3 and r[2] == 1): return False, 0
    return dap_read_dp(p, 0x0C)  # RDBUFF flush


# ──────────────────────────────────────────────────────────────────────
# Test functions
# ──────────────────────────────────────────────────────────────────────
def test_ping(p, ctx):
    """Basic SWD link check via standard CMSIS-DAP. Reads DPIDR + AP_IDR.
    Populates ctx['family'] for downstream chip-aware decisions."""
    print("\n=== PING — basic SWD link via standard CMSIS-DAP ===")
    dap_connect_and_reset(p)
    # ADIv5 §B4.2.2: first transaction after a line reset MUST be DPIDR
    # read. Writing CTRL/STAT first wedges the DP with sticky FAULT.
    ok, dpidr = dap_read_dp(p, 0x00)
    if not ok:
        print(f"FAIL: DPIDR read failed (ack={dpidr})")
        return False
    # Now safe to power up debug + system domains
    dap_write_dp(p, 0x04, 0x50000000)
    designer = (dpidr >> 1) & 0x7FF
    partno = (dpidr >> 20) & 0xFF
    version = (dpidr >> 12) & 0xF
    print(f"  DPIDR    = 0x{dpidr:08x}")
    print(f"    designer = 0x{designer:03x}  (0x23B=ARM, 0x144=Nordic)")
    print(f"    partno   = 0x{partno:02x}")
    print(f"    version  = 0x{version:x}  (1=DPv1, 2=DPv2, 3=DPv3)")
    ok, ap_idr = dap_read_ap(p, 0, 0xFC)
    if ok:
        print(f"  AHB-AP[0].IDR = 0x{ap_idr:08x}")
    # Family heuristic: Cortex-M4 (partno=0xba on DPv1) = nRF52;
    # Cortex-M33 (partno different on DPv2) = nRF5340
    if version == 1:
        ctx['family'] = 'nrf52'
        print("  → detected family: nRF52 family (DPv1)")
    elif version == 2:
        ctx['family'] = 'nrf5340'
        print("  → detected family: nRF5340 family (DPv2)")
    else:
        ctx['family'] = 'unknown'
        print(f"  → unknown family (DP version {version})")
    print("PASS")
    return True


def _validate_echo(resp, expected_cmd):
    if not resp:
        print("FAIL: empty response")
        return False
    if resp[0] == ID_DAP_INVALID:
        print("FAIL: probe returned ID_DAP_INVALID — vendor cmd handler not installed.")
        return False
    if resp[0] != expected_cmd:
        print(f"FAIL: echo 0x{resp[0]:02x} ≠ expected 0x{expected_cmd:02x}")
        return False
    return True


def test_erase(p, ctx):
    print("\n=== NRF53_ERASE (0x85) — CTRL-AP ERASEALL ===")
    resp = p.transfer([CMD_ERASE], timeout_ms=15000)
    print(f"  raw: {resp.hex()}")
    if not _validate_echo(resp, CMD_ERASE):
        return False
    if len(resp) < 3:
        print(f"FAIL: response too short ({len(resp)} bytes)")
        return False
    status, ap_count = resp[1], resp[2]
    print(f"  status={fmt_status(status)}  ap_count={ap_count}")
    if status != 0:
        return False
    expected = 2 if ctx.get('family') == 'nrf5340' else 1
    if ap_count != expected:
        print(f"WARN: ap_count={ap_count} (expected {expected} for {ctx.get('family')})")
    print(f"PASS  ({ap_count} CTRL-AP(s) erased)")
    ctx['erased'] = True
    return True


def test_verify_erase(p, ctx):
    """Read flash[0x00000000] via standard AHB-AP; expect 0xFFFFFFFF."""
    print("\n=== VERIFY_ERASE — read flash[0] via standard CMSIS-DAP ===")
    if not ctx.get('erased'):
        print("SKIP — erase didn't run successfully")
        return True
    # Re-init link (CTRL-AP RESET in erase may have left things in unusual state)
    dap_connect_and_reset(p)
    # DPIDR read MUST be first xact after line reset (ADIv5 §B4.2.2),
    # otherwise DP wedges with sticky FAULT and subsequent reads fail.
    ok, dpidr = dap_read_dp(p, 0x00)
    if not ok:
        print(f"FAIL: post-erase DPIDR read failed (ack={dpidr})")
        return False
    dap_write_dp(p, 0x04, 0x50000000)
    # CSW for App AHB-AP: 0x23000002 for nRF5340, 0x23000012 (auto-inc) or
    # 0x23000002 for nRF52 — same secure+32-bit, no auto-inc works either.
    csw = 0x23000002
    ok, val = dap_mem_read(p, 0, csw, 0x00000000)
    if not ok:
        print(f"FAIL: flash read failed (ack={val})")
        return False
    print(f"  flash[0x00000000] = 0x{val:08x}  (expect 0xFFFFFFFF)")
    if val != 0xFFFFFFFF:
        print(f"FAIL: erase didn't take — flash still has 0x{val:08x}")
        return False
    print("PASS  flash wiped to 0xFFFFFFFF")
    return True


def test_uicr_app(p, ctx):
    print("\n=== NRF53_UICR_PROGRAM_APP (0x8A) ===")
    if ctx.get('family') == 'nrf52':
        print("SKIP — nRF5340-only (uses nRF5340 NVMC + UICR addresses).")
        print("       On nRF52, UICR.APPROTECT is at 0x10001208 (different layout).")
        print("       Future work — see RELEASE.md.")
        return True
    resp = p.transfer([CMD_UICR_APP], timeout_ms=15000)
    print(f"  raw: {resp.hex()}")
    if not _validate_echo(resp, CMD_UICR_APP):
        return False
    if len(resp) < 10:
        print(f"FAIL: response too short")
        return False
    status = resp[1]
    approt, secure = struct.unpack_from("<II", resp, 2)
    print(f"  status={fmt_status(status)}")
    print(f"  APPROTECT       = 0x{approt:08x}  (expect 0x{UNLOCK_VAL:08x})")
    print(f"  SECUREAPPROTECT = 0x{secure:08x}  (expect 0x{UNLOCK_VAL:08x})")
    if status != 0 or approt != UNLOCK_VAL or secure != UNLOCK_VAL:
        return False
    print("PASS  App UICR HwDisabled")
    return True


def test_uicr_net(p, ctx):
    print("\n=== NRF53_UICR_PROGRAM_NET (0x8B) ===")
    if ctx.get('family') == 'nrf52':
        print("SKIP — nRF5340-only (nRF52 has no Net core).")
        return True
    resp = p.transfer([CMD_UICR_NET], timeout_ms=20000)
    print(f"  raw: {resp.hex()}")
    if not _validate_echo(resp, CMD_UICR_NET):
        return False
    if len(resp) < 10:
        return False
    status = resp[1]
    marker, approt = struct.unpack_from("<II", resp, 2)
    print(f"  status={fmt_status(status)}")
    print(f"  SRAM marker     = 0x{marker:08x}  (expect 0x{STUB_DONE:08x})")
    print(f"  Net APPROTECT   = 0x{approt:08x}  (expect 0x{UNLOCK_VAL:08x})")
    if marker == STUB_FAULT:
        print("FAIL: Net stub FAULTED")
        return False
    if status != 0:
        return False
    if approt != UNLOCK_VAL:
        print("FAIL: Net APPROTECT not unlocked")
        return False
    # Marker is informational only. Some nRF5340 silicon variants
    # (DK boards) don't make CPU-side SRAM writes visible via the
    # Net AHB-AP — UICR.APPROTECT readback is the authoritative
    # success criterion.
    if marker != STUB_DONE:
        print(f"  (marker=0x{marker:08x} — not 0xDEADC0DE but UICR programmed; "
              f"AHB-AP/CPU SRAM-view asymmetry on this silicon)")
    print("PASS  Net UICR HwDisabled (UICR.APPROTECT = 0x50FA50FA)")
    return True


def test_mem_roundtrip(p, ctx):
    """Vendor cmds 0x8C WRITE_MEM + 0x88 READ_MEM end-to-end round-trip.
    Writes 4 sentinel words to target SRAM via AHB-AP and reads them back.
    """
    print("\n=== MEM_ROUNDTRIP — WRITE_MEM (0x8C) + READ_MEM (0x88) ===")
    # Pick a SRAM address that's safely valid on both families:
    #   nRF52840: App SRAM 0x20000000+
    #   nRF5340 : App SRAM 0x20000000+, Net SRAM 0x21000000+
    # Use App AHB-AP since it works on both families.
    addr = 0x20000000
    sentinels = [0xCAFEBABE, 0xDEADBEEF, 0x12345678, 0x87654321]
    payload = struct.pack('<IIII', *sentinels)
    req = (bytes([CMD_WRITE_MEM, 0x00, 0x00])
           + struct.pack('<II', 0x23000002, addr)
           + struct.pack('<H', 4)
           + payload)
    resp = p.transfer(req, timeout_ms=10000)
    if not _validate_echo(resp, CMD_WRITE_MEM): return False
    if len(resp) < 2 or resp[1] != 0:
        print(f"FAIL: WRITE_MEM status={fmt_status(resp[1] if len(resp) > 1 else 0xFF)}")
        return False
    print(f"  WRITE_MEM 0x{addr:08x} ← {[hex(s) for s in sentinels]}: OK")

    req = (bytes([CMD_READ_MEM, 0x00, 0x00])
           + struct.pack('<II', 0x23000002, addr)
           + struct.pack('<H', 4))
    resp = p.transfer(req, timeout_ms=10000)
    if not _validate_echo(resp, CMD_READ_MEM): return False
    if len(resp) < 18 or resp[1] != 0:
        print(f"FAIL: READ_MEM status={fmt_status(resp[1] if len(resp) > 1 else 0xFF)}")
        return False
    got = struct.unpack_from('<IIII', resp, 2)
    if got != tuple(sentinels):
        print(f"FAIL: readback mismatch")
        print(f"  expected: {[hex(s) for s in sentinels]}")
        print(f"  got     : {[hex(x) for x in got]}")
        return False
    print(f"  READ_MEM  0x{addr:08x} → bit-perfect match")
    print("PASS  WRITE_MEM/READ_MEM round-trip")
    return True


def test_flash_write_net(p, ctx):
    """Vendor cmd 0x86 NRF53_FLASH_WRITE_NET — Net flash batch write.
    nRF5340-only (no Net core on nRF52). Requires prior ERASE so flash
    is in the 0xFF state (NVMC can't reprogram non-erased flash).
    """
    print("\n=== NRF53_FLASH_WRITE_NET (0x86) ===")
    if ctx.get('family') != 'nrf5340':
        print("SKIP — Net flash exists only on nRF5340 family.")
        return True
    # Require a fresh ERASE first so flash is 0xFF
    print("  (re-erasing first so flash is in 0xFF state)")
    resp = p.transfer([CMD_ERASE], timeout_ms=15000)
    if resp[1] != 0:
        print(f"FAIL: pre-erase status={fmt_status(resp[1])}")
        return False

    test_addr = 0x01000800   # well within Net flash (0x01000000..0x0103FFFF)
    pattern = [0x11111111, 0x22222222, 0x33333333, 0x44444444,
               0x55555555, 0x66666666, 0x77777777, 0x88888888,
               0x99999999, 0xAAAAAAAA, 0xBBBBBBBB, 0xCCCCCCCC,
               0xDEADBEEF, 0xCAFEBABE]
    n = len(pattern)
    payload = struct.pack(f'<{n}I', *pattern)
    req = (bytes([CMD_FLASH_WRITE_NET])
           + struct.pack('<I', test_addr)
           + struct.pack('<H', n)
           + payload)
    resp = p.transfer(req, timeout_ms=10000)
    if not _validate_echo(resp, CMD_FLASH_WRITE_NET): return False
    if len(resp) < 4:
        print(f"FAIL: response too short ({len(resp)} bytes)")
        return False
    status = resp[1]
    written = struct.unpack_from('<H', resp, 2)[0]
    print(f"  WRITE status={fmt_status(status)}  words_written={written}/{n}")
    if status != 0 or written != n:
        return False

    # Read back via 0x88
    req = (bytes([CMD_READ_MEM, 0x00, 0x01])
           + struct.pack('<II', 0x03800042, test_addr)
           + struct.pack('<H', n))
    resp = p.transfer(req, timeout_ms=10000)
    if not _validate_echo(resp, CMD_READ_MEM): return False
    if resp[1] != 0:
        print(f"FAIL: readback status={fmt_status(resp[1])}")
        return False
    got = struct.unpack_from(f'<{n}I', resp, 2)
    mismatches = [(i, hex(g), hex(e)) for i, (g, e) in enumerate(zip(got, pattern)) if g != e]
    if mismatches:
        print(f"FAIL: {len(mismatches)} word mismatches")
        for i, g, e in mismatches[:3]:
            print(f"  word[{i}]: got {g}, expected {e}")
        return False
    print(f"  READ  bit-perfect ({n} words match)")
    print("PASS  Net flash batch write + readback")
    return True


def test_target_info(p, ctx):
    """Vendor cmd 0x89 NRF53_TARGET_INFO — universal ARM identification
    in one packet (DPIDR + AP[0].IDR + CPUID)."""
    print("\n=== NRF53_TARGET_INFO (0x89) ===")
    resp = p.transfer([CMD_TARGET_INFO], timeout_ms=10000)
    if not _validate_echo(resp, CMD_TARGET_INFO): return False
    if len(resp) < 14:
        print(f"FAIL: response too short ({len(resp)} bytes, expected 14)")
        return False
    status = resp[1]
    dpidr, ap_idr, cpuid = struct.unpack_from('<III', resp, 2)
    print(f"  status     = {fmt_status(status)}")
    print(f"  DPIDR      = 0x{dpidr:08x}  (designer 0x{(dpidr>>1)&0x7FF:03x}, "
          f"partno 0x{(dpidr>>20)&0xFF:02x}, version {(dpidr>>12)&0xF})")
    print(f"  AHB-AP[0]  = 0x{ap_idr:08x}")
    print(f"  CPUID      = 0x{cpuid:08x}  (impl 0x{(cpuid>>24)&0xFF:02x}, "
          f"part 0x{(cpuid>>4)&0xFFF:03x})")
    if status != 0:
        return False
    # Cross-check vs ctx['family'] from ping
    fam_expect = {1: 'nrf52', 2: 'nrf5340'}.get((dpidr >> 12) & 0xF)
    if fam_expect and ctx.get('family') and fam_expect != ctx['family']:
        print(f"WARN: family mismatch — DPIDR.version says {fam_expect}, "
              f"ping detected {ctx['family']}")
    print(f"PASS  one-call ID (target family: {fam_expect or '?'})")
    return True


def test_flash_write_app(p, ctx):
    """Vendor cmd 0x87 NRF53_FLASH_WRITE_APP — App flash batch write.
    Family-aware NVMC base:
      - nRF52 family: 0x4001E000
      - nRF5340 App:  0x50039000
    """
    print("\n=== NRF53_FLASH_WRITE_APP (0x87) ===")
    if ctx.get('family') == 'nrf52':
        nvmc_base = 0x4001E000
    elif ctx.get('family') == 'nrf5340':
        nvmc_base = 0x50039000
    else:
        print(f"SKIP — unknown family {ctx.get('family')!r}, can't pick NVMC base")
        return True

    # Re-erase so flash is fresh
    print("  (re-erasing first so flash is 0xFF)")
    resp = p.transfer([CMD_ERASE], timeout_ms=15000)
    if resp[1] != 0:
        print(f"FAIL: pre-erase status={fmt_status(resp[1])}")
        return False

    test_addr = 0x00000800
    pattern = [0xA1A1A1A1, 0xB2B2B2B2, 0xC3C3C3C3, 0xD4D4D4D4,
               0xE5E5E5E5, 0xF6F6F6F6, 0x07070707, 0x18181818,
               0x29292929, 0x3A3A3A3A, 0x4B4B4B4B, 0x5C5C5C5C,
               0xDEADBEEF]
    n = len(pattern)
    payload = struct.pack(f'<{n}I', *pattern)
    req = (bytes([CMD_FLASH_WRITE_APP])
           + struct.pack('<II', nvmc_base, test_addr)
           + struct.pack('<H', n)
           + payload)
    resp = p.transfer(req, timeout_ms=10000)
    if not _validate_echo(resp, CMD_FLASH_WRITE_APP): return False
    if len(resp) < 4:
        print(f"FAIL: response too short ({len(resp)} bytes)")
        return False
    status = resp[1]
    written = struct.unpack_from('<H', resp, 2)[0]
    print(f"  WRITE NVMC=0x{nvmc_base:08x} status={fmt_status(status)}  words_written={written}/{n}")
    if status != 0 or written != n:
        return False

    # Read back via 0x88
    req = (bytes([CMD_READ_MEM, 0x00, 0x00])
           + struct.pack('<II', 0x23000002, test_addr)
           + struct.pack('<H', n))
    resp = p.transfer(req, timeout_ms=10000)
    if not _validate_echo(resp, CMD_READ_MEM): return False
    if resp[1] != 0:
        print(f"FAIL: readback status={fmt_status(resp[1])}")
        return False
    got = struct.unpack_from(f'<{n}I', resp, 2)
    if got != tuple(pattern):
        mm = [(i, hex(g), hex(e)) for i, (g, e) in enumerate(zip(got, pattern)) if g != e]
        print(f"FAIL: {len(mm)} mismatches: {mm[:3]}")
        return False
    print(f"  READ  bit-perfect ({n} words match)")
    print("PASS  App flash batch write + readback")
    return True


def test_recover(p, ctx):
    print("\n=== NRF53_RECOVER (0x84) — full unlock flow ===")
    if ctx.get('family') == 'nrf52':
        print("SKIP — composes UICR_PROGRAM_APP+NET; both nRF5340-only.")
        print("       For nRF52 recover, run ERASE (0x85) — that's the full")
        print("       chip wipe; nRF52 doesn't have the dual-UICR complexity.")
        return True
    resp = p.transfer([CMD_RECOVER], timeout_ms=30000)
    print(f"  raw: {resp.hex()}")
    if not _validate_echo(resp, CMD_RECOVER):
        return False
    if len(resp) < 19:
        return False
    status, ap_count = resp[1], resp[2]
    app_a, app_s, net_m, net_a = struct.unpack_from("<IIII", resp, 3)
    print(f"  status={fmt_status(status)}  ap_count={ap_count}")
    print(f"  App APPROTECT       = 0x{app_a:08x}")
    print(f"  App SECUREAPPROTECT = 0x{app_s:08x}")
    print(f"  Net SRAM marker     = 0x{net_m:08x}")
    print(f"  Net APPROTECT       = 0x{net_a:08x}")
    if status != 0: return False
    # Same marker rule as uicr-net — UICR readbacks are the source of truth
    ok_uicr = (app_a == UNLOCK_VAL and app_s == UNLOCK_VAL
               and net_a == UNLOCK_VAL)
    if not ok_uicr:
        print("FAIL: one or more UICRs not programmed")
        return False
    if net_m != STUB_DONE:
        print(f"  (Net marker=0x{net_m:08x} — not 0xDEADC0DE but UICR programmed)")
    print("PASS  Full RECOVER complete (App + Net UICRs = 0x50FA50FA)")
    return True


TESTS = {
    "ping":              test_ping,
    "target-info":       test_target_info,
    "erase":             test_erase,
    "verify-erase":      test_verify_erase,
    "mem-roundtrip":     test_mem_roundtrip,
    "flash-write-app":   test_flash_write_app,
    "uicr-app":          test_uicr_app,
    "uicr-net":          test_uicr_net,
    "flash-write-net":   test_flash_write_net,
    "recover":           test_recover,
}


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("test", nargs="?", default="all",
                        choices=["all"] + list(TESTS.keys()),
                        help="which test to run (default: all)")
    args = parser.parse_args()

    p = Probe()
    ctx = {}    # accumulates state across tests (family, erased, …)
    try:
        print(f"Probe: VID:PID {VID:04x}:{PID:04x}  bus={p.dev.bus}  "
              f"addr={p.dev.address}  iface={p.itf.bInterfaceNumber}")
        print(f"  bulk OUT EP=0x{p.out_ep.bEndpointAddress:02x}  "
              f"IN EP=0x{p.in_ep.bEndpointAddress:02x}")

        if args.test == "all":
            # Suite order matters for state-aware tests:
            #   ping        — detects family, populates ctx['family']
            #   erase       — wipes chip, populates ctx['erased']
            #   verify-erase — reads flash[0] to confirm wipe
            #   uicr-app    — skipped on nRF52, runs on nRF5340
            #   uicr-net    — same gating
            #   re-erase + recover — only on nRF5340
            results = [
                ("ping",            test_ping(p, ctx)),
                ("target-info",     test_target_info(p, ctx)),
                ("erase",           test_erase(p, ctx)),
                ("verify-erase",    test_verify_erase(p, ctx)),
                ("mem-roundtrip",   test_mem_roundtrip(p, ctx)),
                ("flash-write-app", test_flash_write_app(p, ctx)),
                ("uicr-app",        test_uicr_app(p, ctx)),
                ("uicr-net",        test_uicr_net(p, ctx)),
            ]
            if ctx.get('family') == 'nrf5340':
                results.append(("flash-write-net", test_flash_write_net(p, ctx)))
                print("\n--- re-erase before RECOVER ---")
                results.append(("erase-pre-recover", test_erase(p, ctx)))
                results.append(("recover", test_recover(p, ctx)))

            print("\n" + "=" * 50)
            print(f"Summary  (target family: {ctx.get('family', '?')}):")
            for name, ok in results:
                print(f"  {'✓' if ok else '✗'}  {name}")
            ok_total = all(r[1] for r in results)
        else:
            ok_total = TESTS[args.test](p, ctx)

        sys.exit(0 if ok_total else 1)
    finally:
        p.close()


if __name__ == "__main__":
    main()
