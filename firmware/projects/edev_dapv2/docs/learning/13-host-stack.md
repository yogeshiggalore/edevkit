# Chapter 13 — The Host Stack: pyocd vs probe-rs vs OpenOCD

## Status

🚧 **Stub** — to be filled in. Outline below.

## What you'll learn

1. The four OSS host tools that speak CMSIS-DAP, what each is good at, and where they differ in protocol nuances.
2. Why edev_dapv2 chose to fork pyocd as the lab bench and use probe-rs as the production backend.
3. How to read each tool's logs to understand what it's sending on the wire — useful when a probe works in one tool but not another.

## Outline

- **§13.1** The matrix (cross-checked 2026-06-19):
  - **pyocd** — Apache-2.0, Python, smallest LOC, best for hacking. Built-in `vendor()` API.
  - **probe-rs** — MIT/Apache-2.0, Rust, best nRF target coverage, "production fork target".
  - **OpenOCD** — GPL-2.0, C, oldest, most universal, generic `cmsis-dap cmd <hex>` Tcl passthrough.
  - **(edevocd)** — our pyocd fork with added vendor cmd subcommands.
- **§13.2** Each tool's "connect" choreography — they all do roughly the same SWJ wakeup + DPIDR read, but in subtly different orders that occasionally matters.
- **§13.3** When to use which:
  - DAP-wire debug: pyocd or edevocd (Python is fastest to instrument).
  - Flashing firmware: probe-rs (best target db + CMSIS Flash Algorithm support).
  - Emergency raw vendor invocation: OpenOCD `cmsis-dap cmd …`.
- **§13.4** Diagnosing "works in OpenOCD, fails in pyocd": the dormant-state SWJ wakeup gap; the first-xact rule (Chapter 2); ACK framing strictness (only OpenOCD is lenient).

## Source pointers

- `tools/webdbg/` — the cross-tool browser bench that runs the same DPIDR / reset / info action through all four tools.

## Memory references

- `[[oss-debug-tool-comparison]]` — the verified matrix with LOC/license/build-time.
- `[[edevocd-fork]]` — our pyocd fork's location and discipline.
- `[[probe-rs-uh-dapv2-protocol-error]]` — the Tier 1 ACK-framing issue.
