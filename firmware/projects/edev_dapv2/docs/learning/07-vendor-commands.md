# Chapter 7 — Vendor Commands (`DAP_VENDOR 0x80..0x9F`)

## Status

🚧 **Stub** — to be filled in. Outline below.

## What you'll learn

1. What the CMSIS-DAP vendor range is for and how to make a host invoke a vendor command.
2. Our naming convention: each edev_dapv2 vendor command mirrors an `nrfjprog` CLI flag.
3. How to design vendor commands that minimise "host gap" (sleepy-target survival).

## Outline

- **§7.1** Why CMSIS-DAP reserved 0x80–0xBF: vendor extensions for unbrick, custom flash algos, on-probe sequences. Standard hosts ignore vendor responses they don't recognise.
- **§7.2** The pyocd `DAPAccess.vendor(index, data)` API — index 0 = command 0x80, index 31 = command 0x9F.
- **§7.3** OpenOCD's `cmsis-dap cmd <hex>` Tcl passthrough — raw vendor invocation from any tool.
- **§7.4** Our vendor command catalogue (in `src/dap/dap_vendor.c`):
  - `0x83 EDEV_NRF_SYS_RESET` — AIRCR.SYSRESETREQ + optional halt-at-vector (Chapter 10).
  - `0x86 EDEV_MEM_READ` — AHB-AP block read.
  - `0x87 EDEV_MEM_WRITE` — AHB-AP block write.
  - `0x88 EDEV_AP_READ` — raw AP register read (for IDR, BASE, etc.).
  - `0x89 EDEV_AP_WRITE` — raw AP register write.
  - `0x8A EDEV_CORTEX_M_DUMP` — single-call combo: dp_init + reset+halt + AP scan + SCB read + ROM walk.
- **§7.5** Designing a combo command: pack everything into one firmware call so the host can't introduce a sleep gap.
- **§7.6** Status-byte conventions (`V_OK=0`, `V_DP_FAIL=2`, `V_AP_FAIL=3`, etc.) and how the host decodes them.

## Source pointers

- `src/dap/dap_config.h` — the `DAP_CMD_EDEV_*` defines.
- `src/dap/dap_vendor.c` — all vendor command implementations.
- `tools/edevocd/pyocd/subcommands/_edev_helpers.py` — host-side helpers.

## Memory references

- `[[nrfjprog-reset-impl-2026-06-19]]` — the 0x83 ship story.
- `[[cortex-m-dump-shipped-2026-06-19]]` — the 0x86–0x8A ship story.
