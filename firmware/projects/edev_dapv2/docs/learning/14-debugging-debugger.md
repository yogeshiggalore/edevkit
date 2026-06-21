# Chapter 14 — Debugging Your Debugger

## Status

🚧 **Stub** — to be filled in. Outline below.

## What you'll learn

1. How to capture USB-side traffic between host and probe (Wireshark / usbmon / XHCI Capture).
2. How to capture SWD-side traffic between probe and target (logic analyzer with SWD decoder).
3. How to add wire-level trace inside the probe firmware itself (pioasm `dbg` outputs, UART printf in `src/hw/probe.c`).

## Outline

- **§14.1** "It's not working" triage tree: USB enumeration? CMSIS-DAP capability response? DPIDR? Memory read? Each layer has its own diagnostic move.
- **§14.2** USB-side capture on macOS (XHCI Capture), Windows (USBPcap), Linux (usbmon kernel module).
- **§14.3** SWD-side: Saleae Logic / sigrok with built-in SWD decoder. Sample rate ≥10× SWCLK frequency.
- **§14.4** Firmware-side instrumentation: edev_dapv2 has a UART debug stdio (`pico_enable_stdio_uart`). Add `printf` in `swd_transfer` to dump every transaction.
- **§14.5** Three real bugs and how we found them:
  - Tier 0 / caps byte 0x57 → 0x13 (USB-side: DAP_Info(0xF0) read).
  - Tier 0 / ACK framing fix (SWD-side: probe returned 0x08 alone, illegal).
  - ADIv5 first-xact bug (firmware-side: stepped through `dp_recover` in gdb on the RP2350).

## Memory references

- `[[uh-dapv2-caps-byte-decision]]` — the 0x57/0x13 story.
- `[[uh-dapv2-ack-framing-fix]]` — the bits-0:2 ∈ {1,2,4,7} story.
- `[[uh-dapv2-tier1-diagnosis-2026-06-19]]` — current Tier 1 root cause hunt.
