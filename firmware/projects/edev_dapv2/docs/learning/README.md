# Learning ARM Debug from Scratch

A hands-on tour of how a debug probe actually talks to an ARM Cortex-M chip — written alongside the **edev_dapv2** firmware we're building from scratch.

If you've ever wondered:

- *How does a 2-wire SWD probe read memory from a sealed chip?*
- *What's a DP? What's an AP? Why are there so many of them?*
- *Why does ARM say "the first transaction after a line reset MUST be a DPIDR read"?*
- *How is CMSIS-DAP v2 different from CMSIS-DAP v1, and which one am I writing?*
- *Why does my probe work in OpenOCD but not in pyocd?*

…this is for you.

## Who this is for

**Primary audience:** someone who has written a little embedded C, knows what GPIO and USB are at a high level, but has never *implemented* a debug probe. You've probably *used* J-Link or DAPLink before — now you want to know what's actually happening inside.

**Prerequisites we assume:**

- Comfortable reading C and Python.
- Know what hexadecimal is. Will look up bit-field offsets without flinching.
- Vaguely know that USB has descriptors and endpoints; you don't need to be an expert.
- *Zero* prior knowledge of SWD, JTAG, CMSIS-DAP, CoreSight, or ARM debug architecture.

**Not for you (yet):**

- "How do I use OpenOCD to flash my STM32" — that's a *user* of debug probes. We're building one.
- "How do I write Cortex-M assembly" — orthogonal. We don't run code on the target; we *observe and manipulate* a target that's already running its own code.

## How this differs from ARM's "Beginner's Guide" textbook

[ARM has a great beginner's textbook on Cortex-M *application* development](https://github.com/arm-education/A-Beginners-Guide-to-Designing-Embedded-System-Applications-on-Arm-Cortex-M-Microcontrollers) — running code on the chip. **This document covers the dual problem: looking *into* the chip from outside.** The two together give you the full picture of an embedded system.

We borrow ARM's "learn-by-doing" philosophy. Every chapter ties to:

- **Real source code** in our `edev_dapv2` firmware (file:line references throughout).
- **Real commands** you can run on your own RP2350 + target chip.
- **Real bugs we hit while implementing** — the ADIv5 first-xact gotcha, the cascading sticky-error problem, the nRF5340 sleepy-target wall.

## How to read this

Sequential is best, but each chapter stands on its own once you've done Chapters 1-2. Each chapter follows the same shape:

- **What you'll learn** — three bullets, the load-bearing concepts.
- **Concept** — the theory, with diagrams.
- **In the spec** — pointers to the canonical ARM document section (so you can verify and go deeper).
- **In our code** — file:line references to where edev_dapv2 implements this.
- **Try it** — commands to run on your own hardware to see it work.
- **Gotchas** — real failure modes we caught, with the fixes.
- **Further reading** — chapters that build on this one + external links.

## Table of Contents

### Part I — The wire

| # | Chapter | Status |
|---|---|---|
| 1 | [The SWD physical wire & line reset](01-swd-wire-and-line-reset.md) | ✅ |
| 2 | [The Debug Port (DP)](02-debug-port.md) | ✅ |
| 3 | [The Access Port (AP) + nRF5340 case study](03-access-port.md) | ✅ |
| 4 | [Memory access via AHB-AP (CSW, TAR, DRW)](04-memory-access.md) | ✅ |

### Part II — The probe

| # | Chapter | Status |
|---|---|---|
| 5 | [CMSIS-DAP v2 protocol](05-cmsis-dap-v2.md) | ✅ |
| 6 | [Inside the dispatcher: how a USB packet becomes SWD bits](06-dispatcher.md) | stub |
| 7 | [Vendor commands (`DAP_VENDOR 0x80..0x9F`)](07-vendor-commands.md) | stub |
| 8 | [Sleepy targets & continuous SWCLK keep-alive](08-sleepy-targets.md) | stub |

### Part III — The target

| # | Chapter | Status |
|---|---|---|
| 9 | [Cortex-M debug architecture (SCS, DHCSR, DEMCR)](09-cortex-m-debug.md) | stub |
| 10 | [Software reset: AIRCR.SYSRESETREQ without an nRESET wire](10-software-reset.md) | stub |
| 11 | [CoreSight ROM tables and component discovery](11-rom-table.md) | stub |
| 12 | [Nordic specifics: CTRL-AP, APPROTECT, ERASEALL](12-nordic-specifics.md) | ✅ |

### Part IV — The host

| # | Chapter | Status |
|---|---|---|
| 13 | [The host stack: pyocd vs probe-rs vs OpenOCD](13-host-stack.md) | stub |
| 14 | [Debugging your debugger: tools and techniques](14-debugging-debugger.md) | stub |
| 15 | [Designing your own probe: lessons learned](15-design-lessons.md) | stub |

## Conventions

- **`monospace`** for register names, source paths, command lines.
- **Bold** for new terms on first introduction.
- 📜 callouts mark spec references.
- 🛠 callouts mark "look at this code".
- 🧪 callouts mark "run this command".
- ⚠️  callouts mark gotchas worth burning into memory.

## Source code map

Every chapter cross-references one or more of these:

```
edev_dapv2/
├── src/
│   ├── dap/                 ← CMSIS-DAP v2 dispatcher + handlers
│   │   ├── dap.c            (dispatcher — Ch 6)
│   │   ├── dap_swd.c        (SWD transfer engine — Ch 1, 4)
│   │   ├── dap_swj.c        (line reset, SWJ_Pins — Ch 1, 2)
│   │   ├── dap_vendor.c     (our extension commands — Ch 7, 9, 10)
│   │   ├── dap_config.h     (command IDs, ACK constants — Ch 5)
│   │   └── dap_internal.h   (shared declarations)
│   ├── hw/
│   │   ├── probe.c          (PIO driver — Ch 1)
│   │   └── probe.h
│   ├── pio/probe.pio        (the actual wire bit-banger — Ch 1)
│   └── usb/                 (USB descriptors — Ch 5)
└── tools/edevocd/             (host-side; pyocd fork with vendor cmds)
```

## Acknowledgements

The canonical ARM documents we lean on throughout:

- **ARM IHI 0031** — *ARM Debug Interface Architecture Specification (ADIv5)*. The bible for SWD, DP, AP, MEM-AP.
- **ARM DDI 0553** — *ARMv8-M Architecture Reference Manual*. SCB, AIRCR, DHCSR, DEMCR semantics.
- **ARM IHI 0029** — *CoreSight Architecture Specification*. ROM tables, component identification.
- **ARM CMSIS-DAP v2 specification** — the on-the-wire USB protocol we implement.

The [ARM Education *Beginner's Guide*](https://github.com/arm-education/A-Beginners-Guide-to-Designing-Embedded-System-Applications-on-Arm-Cortex-M-Microcontrollers) for the writing model — "learn by doing", every concept tied to a runnable thing.
