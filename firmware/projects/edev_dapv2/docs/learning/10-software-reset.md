# Chapter 10 — Software Reset: AIRCR.SYSRESETREQ Without an nRESET Wire

## Status

🚧 **Stub** — to be filled in. Outline below.

## What you'll learn

1. How a single 32-bit memory write at `0xE000ED0C` resets the entire SoC.
2. Why this works without any physical nRESET pin connected (it's internal to the Cortex-M).
3. The full wire sequence — the same one `nrfjprog --reset` issues over J-Link.

## Outline

- **§10.1** AIRCR layout: VECTKEY (write 0x05FA to "unlock"), SYSRESETREQ (bit 2), VECTRESET (legacy, M-class only on ARMv7-M).
- **§10.2** The full sequence (mirrors `nrfjprog --sysreset`):
  1. Halt CPU (DHCSR write).
  2. Optional: set DEMCR.VC_CORERESET if you want post-reset halt.
  3. Write AIRCR = 0x05FA0004.
  4. Sleep 10 ms.
  5. dp_init recovery (DP wakes back up).
  6. Poll DHCSR.S_RESET_ST clears.
- **§10.3** Why ignoring the ACK on the AIRCR write is correct (pyocd's `_perform_reset` does this; we do too): the reset can cut the SWD transaction short.
- **§10.4** Live measurement: 13 ms end-to-end through our DAP_VENDOR 0x83.
- **§10.5** When SYSRESETREQ doesn't work: APPROTECT-locked targets, where AIRCR writes are rejected by the AHB-AP — fall back to CTRL-AP RESET (Chapter 12).

## Source pointers

- `src/dap/dap_vendor.c::do_edev_nrf_sys_reset` — the firmware implementation, with inline commentary.
- `tools/edevocd/pyocd/subcommands/nrf_reset_cmd.py` — the host CLI.

## Memory references

- `[[nrfjprog-reset-impl-2026-06-19]]` — the design + ship story for our 0x83 command.
- `[[swd-only-reset-paths]]` — taxonomy of all reset options when nRESET isn't wired.
- `[[adiv5-first-xact-dpidr]]` — the bug we caught during this command's bring-up.
