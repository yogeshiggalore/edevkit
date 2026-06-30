#!/usr/bin/env python3
"""
test_nrf53_vendor_cmds.py — exercise NRF53_* vendor commands.

Sends raw CMSIS-DAP vendor commands (0x84..0x8B) over the bulk OUT/IN
endpoints to a Pico running edev_dapv2_zephyr firmware, validates the
wire-format responses, and reports pass/fail.

Requires pyusb (`pip install pyusb`). On macOS may also need:
  brew install libusb

Hardware setup expected:
  - Pico running edev_dapv2_zephyr (≥ step 5, commit 652b9cc or later)
  - The Pico's USB OTG port connected to the host PC running this script
  - SWD wires from Pico GPIO2 (SWCLK) + GPIO3 (SWDIO) to a Nordic target
    (nRF52840 dongle / nRF5340 DK / Ring under test). Keep the target on
    a charger so its debug domain doesn't power down mid-test.

Usage:
  python3 test_nrf53_vendor_cmds.py                 # run all tests
  python3 test_nrf53_vendor_cmds.py erase           # just NRF53_ERASE
  python3 test_nrf53_vendor_cmds.py uicr-app        # just App UICR program
  python3 test_nrf53_vendor_cmds.py uicr-net        # just Net UICR program
  python3 test_nrf53_vendor_cmds.py recover         # full RECOVER

Exit code 0 if all selected tests passed, non-zero otherwise.

DESTRUCTIVE — every test except --dry-run wipes the target chip's flash.
Don't run against hardware whose firmware you want to keep.
"""
import argparse
import struct
import sys

import usb.core
import usb.util

VID = 0x2E8A
PID = 0x000C
VENDOR_IFACE_CLASS = 0xFF

# Vendor command bytes (CMSIS-DAP ID_DAP_VENDOR0..31 space, 0x80..0x9F)
CMD_RECOVER  = 0x84
CMD_ERASE    = 0x85
CMD_UICR_APP = 0x8A
CMD_UICR_NET = 0x8B

# Mirror of nrf53_status_t
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
                f"Plug the Pico (running edev_dapv2_zephyr) into a USB port and retry.")
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
        """Send one CMSIS-DAP packet, return response bytes."""
        self.out_ep.write(bytes(cmd_bytes), timeout=timeout_ms)
        # Zephyr port reports packet size 512 via DAP_Info; read that.
        # Older pico-sdk builds use 64 — read up to 512 either way; the
        # actual response length comes back in the bulk transfer.
        resp = self.in_ep.read(512, timeout=timeout_ms)
        return bytes(resp)


def _validate_echo(resp, expected_cmd):
    if not resp:
        print("FAIL: empty response")
        return False
    if resp[0] == ID_DAP_INVALID:
        print("FAIL: probe returned ID_DAP_INVALID — vendor cmd handler not "
              "installed. Did you flash a build ≥ step 2 (commit 07957ef)?")
        return False
    if resp[0] != expected_cmd:
        print(f"FAIL: echo byte 0x{resp[0]:02x} ≠ expected 0x{expected_cmd:02x}")
        return False
    return True


def test_erase(p):
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
        print(f"FAIL: status {STATUS.get(status, '?')}")
        return False
    if ap_count < 1:
        print("FAIL: no CTRL-APs found — wrong target attached?")
        return False
    print(f"PASS  ({ap_count} CTRL-AP(s) erased)")
    return True


def test_uicr_app(p):
    print("\n=== NRF53_UICR_PROGRAM_APP (0x8A) ===")
    resp = p.transfer([CMD_UICR_APP], timeout_ms=15000)
    print(f"  raw: {resp.hex()}")
    if not _validate_echo(resp, CMD_UICR_APP):
        return False
    if len(resp) < 10:
        print(f"FAIL: response too short ({len(resp)} bytes)")
        return False
    status = resp[1]
    approt, secure = struct.unpack_from("<II", resp, 2)
    print(f"  status={fmt_status(status)}")
    print(f"  APPROTECT       = 0x{approt:08x}  (expect 0x{UNLOCK_VAL:08x})")
    print(f"  SECUREAPPROTECT = 0x{secure:08x}  (expect 0x{UNLOCK_VAL:08x})")
    if status != 0:
        return False
    if approt != UNLOCK_VAL:
        print("FAIL: APPROTECT not unlocked")
        return False
    if secure != UNLOCK_VAL:
        print("FAIL: SECUREAPPROTECT not unlocked")
        return False
    print("PASS  App UICR HwDisabled")
    return True


def test_uicr_net(p):
    print("\n=== NRF53_UICR_PROGRAM_NET (0x8B) — on-target stub ===")
    resp = p.transfer([CMD_UICR_NET], timeout_ms=20000)
    print(f"  raw: {resp.hex()}")
    if not _validate_echo(resp, CMD_UICR_NET):
        return False
    if len(resp) < 10:
        print(f"FAIL: response too short ({len(resp)} bytes)")
        return False
    status = resp[1]
    marker, approt = struct.unpack_from("<II", resp, 2)
    print(f"  status={fmt_status(status)}")
    print(f"  SRAM marker     = 0x{marker:08x}  (expect 0x{STUB_DONE:08x})")
    print(f"  Net APPROTECT   = 0x{approt:08x}  (expect 0x{UNLOCK_VAL:08x})")
    if marker == STUB_FAULT:
        print("FAIL: Net stub FAULTED — check probe serial log for PC + xPSR")
        return False
    if status != 0:
        return False
    if marker != STUB_DONE:
        print("FAIL: Net stub didn't complete (marker mismatch)")
        return False
    if approt != UNLOCK_VAL:
        print("FAIL: Net APPROTECT not unlocked")
        return False
    print("PASS  Net UICR HwDisabled (via on-target stub)")
    return True


def test_recover(p):
    print("\n=== NRF53_RECOVER (0x84) — full unlock flow ===")
    resp = p.transfer([CMD_RECOVER], timeout_ms=30000)
    print(f"  raw: {resp.hex()}")
    if not _validate_echo(resp, CMD_RECOVER):
        return False
    if len(resp) < 19:
        print(f"FAIL: response too short ({len(resp)} bytes)")
        return False
    status, ap_count = resp[1], resp[2]
    app_approt, app_secure, net_marker, net_approt = struct.unpack_from(
        "<IIII", resp, 3)
    print(f"  status={fmt_status(status)}  ap_count={ap_count}")
    print(f"  App APPROTECT       = 0x{app_approt:08x}  (expect 0x{UNLOCK_VAL:08x})")
    print(f"  App SECUREAPPROTECT = 0x{app_secure:08x}  (expect 0x{UNLOCK_VAL:08x})")
    print(f"  Net SRAM marker     = 0x{net_marker:08x}  (expect 0x{STUB_DONE:08x})")
    print(f"  Net APPROTECT       = 0x{net_approt:08x}  (expect 0x{UNLOCK_VAL:08x})")
    if status != 0:
        return False
    ok = True
    if app_approt != UNLOCK_VAL: print("FAIL: App APPROTECT"); ok = False
    if app_secure != UNLOCK_VAL: print("FAIL: App SECUREAPPROTECT"); ok = False
    if net_marker != STUB_DONE:  print("FAIL: Net stub marker"); ok = False
    if net_approt != UNLOCK_VAL: print("FAIL: Net APPROTECT"); ok = False
    if ok:
        print("PASS  end-to-end RECOVER succeeded")
    return ok


TESTS = {
    "erase":    test_erase,
    "uicr-app": test_uicr_app,
    "uicr-net": test_uicr_net,
    "recover":  test_recover,
}


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("test", nargs="?", default="all",
                        choices=["all"] + list(TESTS.keys()),
                        help="which test to run (default: all)")
    args = parser.parse_args()

    p = Probe()
    try:
        print(f"Probe: VID:PID {VID:04x}:{PID:04x}  bus={p.dev.bus}  "
              f"addr={p.dev.address}  iface={p.itf.bInterfaceNumber}")
        print(f"  bulk OUT EP=0x{p.out_ep.bEndpointAddress:02x}  "
              f"IN EP=0x{p.in_ep.bEndpointAddress:02x}")

        if args.test == "all":
            # Suite order:
            #   1) ERASE        — wipes chip, confirms CTRL-AP works
            #   2) UICR_APP     — App-side host writes work
            #   3) UICR_NET     — on-target stub flow works
            #   4) ERASE again  — re-wipe to set up RECOVER from a clean state
            #   5) RECOVER      — composition: ERASE+APP+NET all in one
            results = [
                ("ERASE",    test_erase(p)),
                ("UICR_APP", test_uicr_app(p)),
                ("UICR_NET", test_uicr_net(p)),
            ]
            print("\n--- re-erasing for clean RECOVER test ---")
            results.append(("ERASE-pre-recover", test_erase(p)))
            results.append(("RECOVER", test_recover(p)))
            print("\n" + "=" * 40)
            print("Summary:")
            for name, ok in results:
                print(f"  {'✓' if ok else '✗'}  {name}")
            ok_total = all(r[1] for r in results)
        else:
            ok_total = TESTS[args.test](p)

        sys.exit(0 if ok_total else 1)
    finally:
        p.close()


if __name__ == "__main__":
    main()
