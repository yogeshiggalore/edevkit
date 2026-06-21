# Chapter 9 — Cortex-M Debug Architecture (SCS, DHCSR, DEMCR)

## Status

🚧 **Stub** — to be filled in. Outline below.

## What you'll learn

1. The **System Control Space (SCS)** at `0xE000ED00..0xE000EDFC` and what each register does.
2. **DHCSR** as the halt/run/step control panel and the S_* status bits that tell you what state the core is in.
3. **DEMCR** as the vector-catch register — how `VC_CORERESET` makes a software reset halt at the reset vector instead of running.

## Outline

- **§9.1** SCS overview: NVIC + SCB + SysTick all share the same address block.
- **§9.2** CPUID decoding — Implementer, Architecture, PartNo (M0..M85 table).
- **§9.3** DHCSR field walk: `C_DEBUGEN`, `C_HALT`, `C_STEP`, `S_HALT`, `S_SLEEP`, `S_LOCKUP`, `S_RESET_ST`.
- **§9.4** DEMCR field walk: `VC_CORERESET`, `VC_HARDERR`, etc. — vector-catch points.
- **§9.5** Other interesting regs: AIRCR (Chapter 10), CCR (configuration), SHCSR (system handler control), MPU TYPE, CPACR (FPU enable).
- **§9.6** The ARMv8-M additions: Secure / Non-Secure banks, S_SDE bit.

## Source pointers

- `tools/edevocd/pyocd/subcommands/edev_info_cmd.py` — `ADDR_*` constants + `_decode_dhcsr_flags`, `_decode_cpuid`.
- `src/dap/dap_vendor.c` — `DHCSR_*`, `DEMCR_*`, `AIRCR_*` defines.

## Try it

- `edevocd info` (Chapter 5 example) — fills the "Cortex-M core" and "Debug state" cards from these registers.
