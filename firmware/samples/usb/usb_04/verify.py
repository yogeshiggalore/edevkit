#!/usr/bin/env python3
"""Exercise all 4 usb_04 vendor requests.

Cross-platform (Linux / macOS / Windows). Requires pyusb:

    pip install pyusb

Linux: add a udev rule (see firmware_sample_usb_04.html §12a).
Windows: install WinUSB driver via Zadig against PID 0xede4.
macOS: works out of the box.
"""

import struct
import sys

import usb.core
import usb.util


VID, PID = 0x1209, 0xEDE4
IFACE = 0  # recipient = interface, wIndex = 0

# bRequest values mirror src/control_demo.c
VREQ_GET_UPTIME = 0x01
VREQ_SET_LOG_MSG = 0x02
VREQ_SET_LED_BLINK = 0x03
VREQ_STALL_ME = 0x04

# bmRequestType bytes
RT_IN = usb.util.build_request_type(
    usb.util.CTRL_IN,
    usb.util.CTRL_TYPE_VENDOR,
    usb.util.CTRL_RECIPIENT_INTERFACE,
)  # 0xC1
RT_OUT = usb.util.build_request_type(
    usb.util.CTRL_OUT,
    usb.util.CTRL_TYPE_VENDOR,
    usb.util.CTRL_RECIPIENT_INTERFACE,
)  # 0x41


def main() -> int:
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        print("usb_04 not found — check VID:PID and udev/Zadig", file=sys.stderr)
        return 1

    dev.set_configuration()

    # (1) GET_UPTIME — 3-stage IN
    data = dev.ctrl_transfer(RT_IN, VREQ_GET_UPTIME, 0, IFACE, 8)
    uptime_ms = struct.unpack("<Q", bytes(data))[0]
    print(f"GET_UPTIME      → {uptime_ms} ms ({len(data)} bytes)")

    # (2) SET_LOG_MSG — 3-stage OUT
    msg = b"hello usb_04"
    n = dev.ctrl_transfer(RT_OUT, VREQ_SET_LOG_MSG, 0, IFACE, msg)
    print(f"SET_LOG_MSG     → device accepted {n} bytes")

    # (3) SET_LED_BLINK — 2-stage (no DATA)
    dev.ctrl_transfer(RT_OUT, VREQ_SET_LED_BLINK, 3, IFACE, 0)
    print("SET_LED_BLINK   → count=3 sent in wValue, no DATA stage")

    # (4) STALL_ME — expected to raise
    try:
        dev.ctrl_transfer(RT_IN, VREQ_STALL_ME, 0, IFACE, 0)
        print("STALL_ME        → UNEXPECTED success; device should have STALLed")
        return 2
    except usb.core.USBError as e:
        print(f'STALL_ME        → expected USBError ("{e}")')

    return 0


if __name__ == "__main__":
    sys.exit(main())
