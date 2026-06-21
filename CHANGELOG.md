# Changelog

All notable changes to the edevkit project are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **docs: USB troubleshooting appendix** — New section in `usb/appendix/troubleshooting.html` documenting cascaded USB hub failures with CMSIS-DAP bulk transfers on macOS. Root-causes TT jitter, interface-binding races, and V_BUS droop; provides four-point fix checklist. Cross-referenced from `usb/partI/01_why_usb.html` (2026-05-08).

- **docs: Shell helper shadowing detection guide** — Added callout in `shell_helpers.html` explaining how to identify and remove shadow definitions of `edev_*` functions that conflict with the canonical `$EDEVKIT_REPO/tools/env_helpers.sh` source. Explains Pattern B (shell-rc sourcing) vs Pattern C (venv activate) to prevent shadowing (2026-05-08).

- **firmware: edev_run + edev_rtt_monit helpers** — Ported back into `tools/env_helpers.sh` after their absence surfaced from doc sweeps. `edev_run` = `edev_flash` followed by `probe-rs attach` (deliberately goes through the canonical mcuboot+app SWD download, not `probe-rs run`'s single-ELF flash). `edev_rtt_monit` = pure `probe-rs attach` against the build's `zephyr.elf` for read-only RTT. Both honor the same `EDEV_PROBE` / `EDEV_PROBE_PROTOCOL` / `EDEV_PROBE_SPEED` overrides as `edev_flash`. Internal `_edev_find_elf` helper added to mirror the sysbuild → plain ELF fallback ladder. `edev_help` and `shell_helpers.html` updated; `firmware_workflow.html`, `firmware_sample_walkthrough.html` Phase 6, and `firmware_sample_usb_02.html` §10 reverted from the explicit two-step flow back to the simpler `edev_run` form (2026-05-08).

- **docs: usb_03 sample design spec** — New `tools/web/pages/firmware_sample_usb_03.html` (1038 lines). Locks the design for the third USB Book sample: PID `0xede3`, class `"strings_demo"`, scope = all six standard string descriptors (LANGID, Manufacturer, Product, Serial, Configuration, Interface) plus a runtime-formatted `EDV-XXXX-YYYY-ZZZZ` serial derived from `hwinfo_get_device_id()`. Pairs with USB Book Ch 07 (not yet written). Authoring order: doc-first design spec; the `firmware/samples/usb/usb_03/` sample directory is to be scaffolded later from this doc as the spec. Introduces four new API/spec gotchas G11–G14 on top of usb_02's G1–G10. Every Zephyr USBD API claim verified against `$ZEPHYR_BASE/include/zephyr/usb/usbd.h` (Zephyr v4.4.99) before publication, per the hard rule in `reference_zephyr_usbd_api.md` (2026-05-08).

- **docs: USB Book Ch 14 — Inside RP2350** — New `tools/web/pages/usb/partIII/14_rp2350_usb.html` (524 lines). First chapter to ship in Part III. Opens the RP2350 USB hardware subsystem from a firmware-engineer perspective: the four blocks (controller, PHY, DPRAM, PLL_USB), the three CPU↔USB channels (MMIO at 0x50110000, DPRAM at 0x50100000, IRQ vector 14), the absence of a DMA path, the full register-bit layout for `MAIN_CTRL` / `SIE_CTRL` / `INTS` / endpoint-control words / buffer-control words, the ten new RP2350 features vs RP2040 (PHY isolation, GPIO muxing, NAK poll, multi-hub fix, DPRAM double-read fix, etc.), and the single USB errata (RP2350-E12, clk_sys must be > 48 MHz). Every register address, bit field, and quoted text verified against the RP2350 datasheet build d126e9e (2025-07-29), §12.7 (pp. 1141–1181). `usb_book.html` updated to link the chapter (no longer marked `unwritten`). Light datasheet-anchored touch-ups added to Part I: Ch 02 (27 Ω external series-termination requirement, RP2350 pin numbers 46/48/51 on QFN-80, programmable 1.2 / 2.3 kΩ pull-up via `SIE_CTRL.RPU_OPT`), Ch 04 (RP2350 silicon-level MPS limits per §12.7.1.1.1), Ch 05 (firmware-driven `SET_ADDRESS` handling on RP2350 specifically). Two new appendix entries added: `troubleshooting.html#rp2350-e12` (the clk_sys errata) and `troubleshooting.html#phy-iso-not-cleared` (the one bit that breaks RP2040-driver ports). Datasheet research delegated to a research agent; the extract is preserved at `/tmp/RP2350_USB_datasheet_extract.md` for next-session reference (2026-05-08).

### Changed

- **firmware: edev_flash error handling** — Enhanced `tools/env_helpers.sh:248–289` to surface `probe-rs reset` failures (previously silent with `|| true`), print a cold-boot hint (USB unplug or RUN pin pulse) on reset failure, and document the RP2350 SYSRESETREQ quirk where soft reset doesn't always re-run bootrom cleanly. Updated `shell_helpers.html` edev_flash description to match (2026-05-08).

- **docs: firmware_workflow.html helper guidance** — Replaced outdated hardcoded helper definitions with a reference to `shell_helpers.html` (the canonical source). Updated shadowing warning to note the 2026-05-08 unification of helper sourcing and how to detect stale shadows via `type -a` (2026-05-08).

### Deprecated

- Shell-rc helper shadowing — Users should not manually define `edev_*` functions in `~/.zshrc`, `~/.bashrc`, or `$EDEV_VENV/bin/activate`. Use Pattern B (shell-rc sourcing) or Pattern C (venv activate sourcing) exclusively; see `shell_helpers.html § "Install"` (2026-05-08).

### Known Gaps

- _(none open at this time)_
