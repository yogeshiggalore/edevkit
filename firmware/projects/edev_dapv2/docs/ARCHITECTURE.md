# edev_dapv2 — architecture

A from-scratch CMSIS-DAP v2 debug-probe firmware for RP2350.

The substrate is **pico-sdk + TinyUSB**: it handles boot ROM glue, clock
tree, USB controller registers, PIO assembly, and UF2 packaging. Every
byte *above* that layer is ours — descriptors, MS OS 2.0 BOS chain,
vendor-class driver, DAP command dispatcher, every command handler,
SWD/JTAG/SWO PIO programs, ring buffers.

The goal is a probe that probe-rs / pyocd / openocd all enumerate
out-of-the-box, reads IDCODE on a real Cortex-M target, and serves SWO
trace on a dedicated bulk endpoint.

## Scope of v0.1

| Layer | In | Out |
| --- | --- | --- |
| MCU | RP2350A (Cortex-M33, NS) | RP2040, RP2350 RISC-V cores |
| Board | Generic Pico 2 / XIAO RP2350 | edevkit1 PCB |
| Protocols | SWD, JTAG, SWO (UART) | Manchester SWO, MSC, RTT, DAPv1 (HID) |
| Vendor cmds | none (spec-clean) | Nordic UNBRICK / flash algos |
| Host stack | probe-rs, pyocd, openocd | Keil, IAR (untested) |

Spec-clean means a host that supports any other CMSIS-DAP v2 probe must
treat us as drop-in: no special VID, no driver `.inf`, no host-side
config.

## Stack choice

We considered three options:

| | Bare-metal | **pico-sdk + TinyUSB** | Zephyr 4 + USB-Device-Next |
| --- | --- | --- | --- |
| Time to LED blink | days | minutes | hours |
| Time to USB enum | days–weeks | hours | day |
| Online help | almost none | very strong | strong |
| Lines we write before reaching CMSIS-DAP work | ~1500 | ~50 | ~300 |
| What's unique about *our* code | infrastructure | the protocol | the protocol |

`pico-sdk + TinyUSB` wins because the engineering hours we have are best
spent on the layer that's actually novel — clean CMSIS-DAP v2 code —
rather than re-deriving USB enumeration for the thousandth time. yapicoprobe
made this same call and that's why it's the de-facto OSS reference probe.

We do **not** vendor ARM's reference `DAP.c`. Every command is
re-implemented in our tree so the codebase stays in one license, one
style, one place. We use TinyUSB for descriptor handling, EP0 control
transfers, and bulk-endpoint plumbing; everything CMSIS-DAP we write
ourselves.

We also write a **custom TinyUSB class driver** (via
`usbd_app_driver_get_cb()`) for the DAP vendor interface, rather than
using TinyUSB's stock `vendor` class. The stock class has internal RX/TX
FIFOs that hide packet boundaries — for a request/response protocol where
"one USB OUT URB = one DAP command packet" must hold, the FIFOs are noise.
Our class driver hands each OUT URB straight into a request ring slot and
sends each response from a response ring slot. yapicoprobe and dap_v2 both
do this.

## Reference baseline

After deep-reading [yapicoprobe](https://github.com/rgrr/yapicoprobe)
(yapicoprobe is the firmware behind both PicoLink and the practical
"works with everything" expectation on hosts), the [ARM CMSIS-DAP
reference firmware](https://github.com/ARM-software/CMSIS-DAP), the
RP2350 datasheet (USB / clocks / PIO chapters), and the host-side code in
pyocd / openocd / probe-rs, the conclusions that drive our design are:

1.  **The dispatcher is trivial.** It's a switch on byte 0 of the OUT
    packet, with a static table giving each command's minimum payload
    length and (for variable-length commands) a known shape for parsing
    the count byte. Atomic-command bundling (0x7F) just re-enters the
    dispatcher for the bundled commands.
2.  **The bit-banger is a single PIO program with `out pc, 8` dispatch.**
    yapicoprobe's `probe.pio` does SWD writes, SWD reads, JTAG, and SWJ
    sequences from one 68-instruction program. We copy that idea but
    re-write the program; we don't vendor `probe.pio`.
3.  **Parity in C, not PIO.** `__builtin_popcount(val) & 1` after read.
    Keeps PIO small and parity logic debuggable.
4.  **MS OS 2.0 BOS descriptors are mandatory** for Windows to bind
    WinUSB without `.inf` install. We embed Compatible-ID `"WINUSB"` and
    a `DeviceInterfaceGUIDs` registry property in a BOS chain returned
    from `tud_descriptor_bos_cb()`.
5.  **iInterface string MUST contain the literal `"CMSIS-DAP"`** — every
    host scans for that substring as a hard fallback when VID/PID isn't
    on its whitelist (verified in pyocd, openocd, probe-rs sources).
6.  **Advertise packet size 512 and packet count 4** via
    DAP_Info(0xFF/0xFE). USB FS bulk max is 64 bytes wire-side, but a
    *logical* DAP packet up to 512 bytes spans multiple bulk transfers
    and hosts open the throttle once we say we support it.
7.  **The host owns retry policy.** WAIT/FAULT recovery is configured
    via `DAP_TransferConfigure(idle, wait_retry, match_retry)` and then
    the firmware retries internally — the host expects only the *final*
    ACK.
8.  **DAP_FW_VER must report `"2.2.0"` or higher.** probe-rs 0.31+ rejects
    older versions at probe-open time (see project memory
    `reference_probe_rs_bcd_device_gate`). Our protocol surface meets the
    2.2.0 spec.

## VID / PID strategy

| | |
| --- | --- |
| idVendor | `0x2E8A` (Raspberry Pi Trading) |
| idProduct | `0x000C` (CMSIS-DAP — RP-official Debug Probe) |
| bcdDevice | `0x0220` (edev_dapv2 v2.2.0 protocol; clears probe-rs gate) |

We piggy-back on the Raspberry Pi Debug Probe identity. That pair is
hard-coded into every CMSIS-DAP host's whitelist, so the probe is
discoverable with zero host-side configuration. The `bcdDevice` value
clears the probe-rs 0.31+ gate that rejects bcdDevice < 0x0220 (a
specific bug-prevention gate documented in project memory).

Strings:

| iManufacturer | `"Edevkit"` |
| iProduct | `"edev_dapv2 CMSIS-DAP"` |
| iSerial | RP2350 64-bit chip-id → `"E464-XXXX-XXXX-XXXX"` |
| iInterface | `"edev_dapv2 CMSIS-DAP v2 Interface"` — **must contain** `CMSIS-DAP` |

## USB topology (v0.1)

```
Configuration 1
└── Interface 0 — Vendor (0xFF/0x00/0x00)        "edev_dapv2 CMSIS-DAP v2 ..."
    ├── EP 0x01 OUT  bulk 64B    DAP command stream
    ├── EP 0x81 IN   bulk 64B    DAP response stream
    └── EP 0x82 IN   bulk 64B    SWO trace stream  (advertised; v0.2 hooks data)
```

Plus the BOS chain:

```
BOS
├── PlatformDeviceCapability  PlatformCapabilityUUID = MS OS 2.0 GUID
│       └── MS OS 2.0 Descriptor Set
│           ├── Configuration Subset
│           │   └── Function Subset (interface 0)
│           │       ├── CompatibleID = "WINUSB\0\0"
│           │       └── RegistryProperty DeviceInterfaceGUIDs = "{CDB3B5AD-…}"
```

No CDC, no MSC, no extra interfaces in v0.1. We nail CMSIS-DAP v2 first,
then extend.

## Pin map (generic Pico 2 / XIAO RP2350)

Single voltage domain, 3.3 V only, no level shifters in v0.1.

| GPIO | Function | PIO use | Notes |
| ---: | --- | --- | --- |
| 2 | SWCLK / TCK | PIO0 SM0 side-set | output, push-pull |
| 3 | SWDIO / TMS | PIO0 SM0 in/out | bidir, pindirs flips |
| 4 | TDI | PIO0 SM0 (JTAG mode) | output |
| 5 | TDO | PIO0 SM0 (JTAG mode) | input |
| 6 | nRESET | SIO open-drain | drive 0 only |
| 8 | SWO RX | PIO0 SM1 UART decode | input |
| 25 | LED (Pico 2) | SIO | sign-of-life |

PIO budget v0.1:

| PIO0 | SM0 | SM1 | SM2 | SM3 |
| --- | --- | --- | --- | --- |
| | SWD/JTAG (probe.pio) | SWO UART RX | (free) | (free) |

PIO1, PIO2 reserved.

## Source tree

```
edev_dapv2/
├── CMakeLists.txt
├── pico_sdk_import.cmake
├── pico-sdk -> ../dap_v2/pico-sdk      # symlink, not committed copy
├── README.md
├── docs/
│   ├── ARCHITECTURE.md                 this file
│   ├── PROTOCOL.md                     our subset of CMSIS-DAP, byte-level
│   ├── BRINGUP.md                      step-by-step on real hardware
│   └── RP2350_REGISTERS_CHEATSHEET.md  curated register reference
├── src/
│   ├── main.c                          init order, event loop
│   ├── tusb_config.h                   TinyUSB compile-time config
│   ├── usb/
│   │   ├── usb_descriptors.c           device/config/interface/EP + strings
│   │   ├── usb_descriptors.h
│   │   ├── usb_bos.c                   BOS + MS OS 2.0 descriptor set
│   │   ├── usb_dap_class.c             custom TinyUSB class driver for DAP itf
│   │   └── usb_dap_class.h
│   ├── dap/
│   │   ├── dap.c                       dispatcher, command-length table
│   │   ├── dap.h
│   │   ├── dap_info.c                  0x00 subcommand handlers
│   │   ├── dap_general.c               0x01 HostStatus, 0x09 Delay, 0x0A Reset
│   │   ├── dap_swj.c                   0x10 Pins, 0x11 Clock, 0x12 Sequence
│   │   ├── dap_swd.c                   0x13 Cfg, 0x1D Seq, 0x05 Tfr, 0x06 Blk
│   │   ├── dap_jtag.c                  0x14 Seq, 0x15 Cfg, 0x16 IDCODE
│   │   ├── dap_swo.c                   0x17..0x1E
│   │   ├── dap_atomic.c                0x7F ExecuteCommands
│   │   └── dap_config.h                capability bits, packet size/count
│   ├── pio/
│   │   ├── probe.pio                   SWD + JTAG unified bit-banger
│   │   └── swo_uart.pio                SWO UART RX
│   ├── hw/
│   │   ├── rp2350.h                    curated raw-register cheat-sheet
│   │   ├── probe.c                     PIO program loader + bit-bang shim
│   │   ├── probe.h
│   │   ├── chip_id.c                   bootrom unique ID → serial string
│   │   ├── chip_id.h
│   │   └── target.c/.h                 nRESET drive helpers
│   ├── util/
│   │   ├── ring.c/.h                   lock-free SPSC ring for ISR ↔ main
│   │   └── led.c/.h                    sign-of-life blink
│   └── version.h                       auto-generated git-rev string
└── tools/
    └── (later)
```

`src/hw/rp2350.h` is *not* a re-implementation of pico-sdk's headers —
it's a curated cheat-sheet of the ~30 registers we care about, with the
ones pico-sdk hides poorly (PADS_BANK0 with ISO bit, PIO config in SET
mode, etc.) directly addressable. When we need to bypass an SDK
abstraction for performance or visibility, this file is the contract.

## Build approach

CMake 3.13+, arm-none-eabi-gcc 13+. The standard pico-sdk import pattern.

```
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPICO_BOARD=pico2 ..
make -j

ls *.uf2 *.elf
# edev_dapv2.uf2  edev_dapv2.elf
```

To flash: hold BOOTSEL on the Pico 2, plug USB, drag-drop `edev_dapv2.uf2`.

## Dispatcher design

DAP packet format: byte 0 is the command, payload length is determined
by the command. We compute total length statically for fixed-size
commands and via a parsed count byte for variable-length ones.

```c
static uint16_t dap_packet_length(const uint8_t *pkt, uint16_t avail);
static uint16_t dap_dispatch    (const uint8_t *req, uint16_t req_len,
                                 uint8_t *resp, uint16_t resp_cap);
```

Flow:

```
USB OUT URB ──► usb_dap_class.c (custom class driver)
            ──► ring_push(&req_ring, packet)
            ──► tud_task() loop wakes dap_task()
dap_task    ──► ring_pop(&req_ring) → dap_dispatch() → ring_push(&resp_ring)
            ──► usb_dap_class.c sends bulk IN
USB IN  ── completion ──► next ring slot
```

No FreeRTOS, no threads. Single cooperative loop. ISRs nudge the main
loop via tud_task event; no manual `__SEV()` needed because the
TinyUSB-pico-sdk integration already does this.

## Test plan summary (full version in `docs/BRINGUP.md`)

| Milestone | Command | Pass criterion |
| --- | --- | --- |
| M1 boot | (none — LED blinks) | LED blinks ~1 Hz after 0.5 s |
| M2 USB enum | `lsusb -v -d 2e8a:000c` | `bInterfaceClass 0xff` + iInterface contains CMSIS-DAP |
| M3 discover | `pyocd list` / `probe-rs list` | device shown, "edev_dapv2 CMSIS-DAP" |
| M4 info | `pyocd commander` | DAP_Info round-trips |
| M5 SWD | `probe-rs info --chip RP2040` on a real Pico | IDCODE reads, ROM table dumps |
| M6 flash | `probe-rs run --chip nRF52840 blink.elf` | LED on target blinks |
| M7 JTAG | `openocd -f interface/cmsis-dap.cfg -c "transport select jtag"` | TAP scan succeeds |
| M8 SWO | host `pyocd --trace` while target ITM-prints | bytes appear |

Each milestone is independent; we ship known-good state at every step.

## What we explicitly do NOT do

-   No CDC ACM in v0.1. Add in v0.2 as a 2nd interface.
-   No MSC drag-drop reflash. BOOTSEL covers it.
-   No RTT viewer. SWO carries `printf` over ITM stim port 0 already.
-   No vendor commands (0x80–0xBF). Reserved for v0.2+.
-   No FreeRTOS, no rtos. Cooperative loop is sufficient and easier to
    reason about.
-   No multicore. Core 0 does everything. Core 1 stays in WFI.
-   No vendoring of ARM's `DAP.c`. We re-implement every command.
