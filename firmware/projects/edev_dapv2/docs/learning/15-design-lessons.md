# Chapter 15 — Designing Your Own Probe: Lessons Learned

## Status

🚧 **Stub** — to be filled in. Outline below.

## What you'll learn

1. What we'd do differently if we started edev_dapv2 from scratch today.
2. The five biggest decisions: substrate (pico-sdk vs Zephyr), USB framework, PIO vs bit-bang, host stack choice, vendor-cmd discipline.
3. What's still on the roadmap and the trade-offs of each addition.

## Outline

- **§15.1** Substrate retrospective: pico-sdk + TinyUSB was the right call. Zephyr would have added 2-4× initial effort for marginal benefit.
- **§15.2** PIO as the SWD bit-banger: zero contention with USB, deterministic timing, parameterised by host-side `DAP_SWJ_Clock`. Re-do choice: yes.
- **§15.3** Single-firmware-call vendor commands as a hedge against host-stack issues: paid off when Tier 1 made probe-rs unable to read DPIDR. Re-do choice: yes, but earlier in the project.
- **§15.4** Forking pyocd vs writing a thin Python client from scratch: forking won — edevocd inherited target databases, probe enumeration, USB plumbing for free.
- **§15.5** Documentation discipline: writing this very book *alongside* implementation, not after. Pays off when a future contributor (or future-you) tries to add Tier 2 / SWCLK keep-alive.
- **§15.6** Roadmap:
  - ⭐ Tier 2: continuous SWCLK keep-alive PIO (Chapter 8).
  - Vendor 0x84 / 0x85: CTRL-AP debug-reset + recover (Chapter 12).
  - Core register read via DCRSR/DCRDR.
  - SWO ring buffer (Milestone 7).
