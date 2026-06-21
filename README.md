# edevkit

> Open-source **embedded developer kits** for ARM + RISC-V work — one
> board on your desk that replaces a drawer full of dongles.

**edevkit** is a family of single-board USB-powered developer kits. Each
unit folds a debug probe, a stack of protocol testers, and a host
GUI / CLI into one device, so a developer working on an embedded board
can flash, debug, log, sniff, drive, emulate, and bring up new firmware
without keeping six other tools on the bench.

A single edevkit unit gives you, in one USB-C cable's worth of wiring:

- **ARM + RISC-V debug probe** — CMSIS-DAP v2 over USB, SWD + JTAG,
  works with pyOCD / OpenOCD / probe-rs / GDB
- **SWO trace capture** — ITM/TPIU output decoded and streamed to host
- **Two UART bridges** — one for the target's log console, one for
  developer-driven protocol testing (independent voltage domains)
- **I²C protocol tester** — master, slave, or sniffer at any address,
  up to 3.4 MHz
- **SPI / DSPI / QSPI tester** — master / slave / sniffer in every
  CPHA/CPOL mode, up to 50 MHz QSPI master
- **Device emulation engine** — YAML-driven; plays the role of a real
  SPI flash chip / I²C sensor / UART AT-modem so you can develop the
  *host* side without the actual peripheral on hand
- **OTA firmware updates** for the kit itself — MCUboot + SMP, no
  unplugging
- **Host stack** — a Tauri desktop GUI for visual inspection plus a
  Rust CLI for scripting and CI

The current product is **edevkit1**, in design (RP2350B Cortex-M33,
Zephyr 4.x, MCUboot). Future generations (`edevkit2`, …) will share
this same repo, toolchain, host stack, and docs hub — only the
hardware-specific design doc gets a version-numbered name.

---

## I'm new here — where do I start?

**Open the installation guide directly in your browser.** It's a
self-contained HTML page — no server, no build step, no setup required.

```bash
git clone https://github.com/yogeshiggalore/edevkit.git
cd edevkit

# macOS:
open tools/web/pages/installation.html

# Linux:
xdg-open tools/web/pages/installation.html

# Windows (PowerShell):
start tools/web/pages/installation.html
```

Or just double-click the file in your file manager. The same applies to
every other doc under `tools/web/pages/` — they're all standalone HTML
with embedded CSS, browseable via `file://` without any tooling.

The install guide walks you through (cross-platform):

1. System prerequisites (cmake, ninja, gperf, dtc, …)
2. Cloning the repo
3. Python venv + `west`
4. Zephyr workspace
5. Zephyr SDK
6. RP2350 PIO assembler
7. Rust + Tauri (host)
8. KiCad 9 + KiBot (hardware)
9. Enclosure CAD (CadQuery / FreeCAD)
10. Slicer (OrcaSlicer)
11. Bench tools (sigrok, mcumgr, serial)
12. Editor / IDE setup
13. Shell helpers
14. Smoke tests
15. Final checklist

Estimated total time: **1.5–3 hours** (mostly unattended downloads).

---

## I'm already set up — give me the docs hub

Once your venv is active, run the local docs hub for searchable
cross-referenced docs (light/dark theme, keyboard shortcuts, auto-discovers
new pages):

```bash
source "$EDEV_VENV/bin/activate"
cd tools/web
python server.py
# open http://127.0.0.1:8765/
```

The hub serves the same files you can already open via `file://` —
plus a searchable index, sidebar nav, and theme toggle.

---

## Repository layout

```
edevkit/
├── firmware/           Zephyr workspace + applications
│                       (per-product subdirs as the family grows)
├── hardware/           KiCad 9 schematic + PCB
├── host/               Rust workspace — Tauri GUI + CLI
├── enclosure/          CadQuery / FreeCAD source for 3D-printed shells
├── documents/          Non-HTML artifacts (PDFs, captures, diagrams)
└── tools/
    └── web/            Local docs hub (FastAPI)
        ├── pages/      ← all project HTML docs (open via file:// or hub)
        ├── server.py
        ├── static/     theme + JS
        └── templates/  Jinja2 templates
```

## Products

| Product   | Status     | Hardware                                  |
|-----------|------------|-------------------------------------------|
| edevkit1  | In design  | RP2350B (Cortex-M33) · Zephyr 4.x · MCUboot |

Future generations (edevkit2, …) will use this same repo, this same
toolchain, and these same docs hub conventions — only product-specific
content gets a version-numbered name.

## Documentation

All docs live under [`tools/web/pages/`](tools/web/pages/):

- [edevkit1 design spec](tools/web/pages/edevkit1.html) — v1 architecture (source of truth)
- [Installation guide](tools/web/pages/installation.html) — cross-platform onboarding

Drop any new `.html` into `tools/web/pages/` and it's immediately
discoverable by the docs hub (and openable via `file://`).

## Contributing

This is an open-hardware project: every contribution should be
reproducible from open-source tooling. No proprietary CAD, no
closed-source build steps. See the design spec's Decision Log section
for what's settled and what's still open.

## License

TBD.
