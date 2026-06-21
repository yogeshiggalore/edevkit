# Chapter 1 — The SWD Physical Wire & Line Reset

## What you'll learn

1. **What SWD physically is**: two wires (SWCLK, SWDIO), why we don't need an nRESET pin, and how data flows half-duplex.
2. **How a transaction looks on a logic analyzer**: the 46-bit-or-so packet structure with start, request, ACK, data, parity, turnaround, idle.
3. **How a probe gets the target's debug interface from "I might be in JTAG mode" to "definitely speaking SWD with me"**: the line reset sequence and the legendary `0xE79E` magic.

## Concept

### 1.1 Two wires, two states

ARM defined SWD ("Serial Wire Debug") as the spiritual successor to JTAG, optimised for chip pin count. A debug probe needs only:

```
PROBE                  TARGET
 SWCLK   ──────────→   SWCLK    (probe-driven clock, always)
 SWDIO   ←────────→    SWDIO    (bidirectional data)
 GND     ──────────    GND
 VTREF   ←──────────   VDD      (probe senses target's I/O voltage)
```

Compare that to JTAG: 4 wires (TCK, TMS, TDI, TDO) + optional 5th (TRST). SWD halves the pin count, which matters a lot on small chips.

`SWDIO` is **half-duplex**: the probe drives it when sending bits *to* the target; the target drives it when sending bits *back*. Between handovers there is a short "turnaround" period during which neither side drives — the line goes high-Z and a pull-up resistor (in the probe or target) holds it at a known level. Mishandling turnaround is one of the most common bugs in custom probe firmware.

> 📜 **Spec reference:** ADIv5 §B4.1 *Serial-Wire Debug Protocol*.

### 1.2 The transaction packet, byte by byte

A single SWD transaction (e.g. "read DPIDR") is 46 bits on the wire:

```
clocks →  8                1  3                1            32                  1     ≥1
         ┌─────────┐  ┌──┐  ┌─────┐  ┌──┐  ┌──────────────────┐  ┌──┐  ┌──────────┐
SWDIO    │ req[8]  │~tA~│ ACK │~tA~│        DATA[32]           │  P │   IDLE     │
         └─────────┘  └──┘  └─────┘  └──┘  └──────────────────┘  └──┘  └──────────┘
          probe drv   hi-Z  tgt drv  hi-Z   probe (write)        prty   probe drv
                                            or tgt (read)
```

The request byte's bit layout, MSB-first as printed but LSB-first on the wire:

```
bit 7  6  5  4  3  2  1  0
    ───────────────────────
    1  P  0  A3 A2 RnW APnDP  1    (start=1, park=1, stop=0)
       │              │   │
       │              │   └─ 0 = DP register, 1 = AP register
       │              └─── 0 = write, 1 = read
       └─ even parity over the 4 middle bits
```

So `read DPIDR` (DP register at offset 0x00, RnW=1, A2=A3=0) gives the request byte `0b10100101 = 0xA5`.

The 3-bit `ACK` field, returned by the target:

| Bits | Name      | Meaning |
|------|-----------|---------|
| 001  | `OK`      | Transaction accepted. Data phase follows. |
| 010  | `WAIT`    | Target busy. Probe should retry. |
| 100  | `FAULT`   | Sticky error — DP needs `ABORT` write to clear. |
| 111  | `NO_ACK`  | Target didn't drive at all. Wire is dead, or target asleep, or you didn't issue a line reset, or you violated the "first xact = DPIDR" rule (see Chapter 2). |

Anything else (000, 011, 101, 110) is a **protocol error** — typically a wire/timing problem.

### 1.3 The line reset

When the probe first attaches, or after weird transitions (chip reset, switch from JTAG to SWD), the probe must put the SWD-DP into a known state. The recipe:

```
1. Hold SWDIO high for ≥50 SWCLK rising edges.     ← "line reset"
2. Drive SWDIO low for ≥2 SWCLK rising edges.       ← idle, signals end of line reset
```

That's it. The target's DP latches "line reset received → expect DPIDR next".

If you're switching the SWJ-DP from JTAG to SWD mode (some chips support both), insert a 16-bit magic value between two line resets:

```
line reset (≥50 ones) → 0xE79E (16 bits, LSB-first → 0111 1001 1110 0111 wire bits)
                     → line reset → ≥2 zeros idle
```

The magic value `0xE79E` is the "JTAG-to-SWD" selector encoded in a way that can't be confused with any real JTAG TMS sequence.

> ⚠️ **Gotcha:** `0xE79E` is the *deprecated* legacy switch sequence. ADIv6 chips with dormant-state SWJ require a longer 128-bit "selection alert" sequence. The two are not interchangeable. For nRF52 (DPv1) the deprecated path works fine. For some newer chips (DPv2) you'll need dormant-state wakeup — that's covered in Chapter 5.

## In the spec

- ADIv5 §B4.1.2 — **electrical**: SWDIO is push-pull / tristate; SWCLK is push-pull; pull-up resistor on SWDIO required.
- ADIv5 §B4.1.4 — **packet format**: the 46-bit transaction shape.
- ADIv5 §B4.3 — **DP target states**: post-line-reset is a specific state with strict rules (more in Chapter 2).
- ADIv6 §B4 — superseded electrical/state machine; backwards-compatible for most current chips.

## In our code

edev_dapv2 doesn't bit-bang SWD from CPU. The RP2350's **Programmable I/O (PIO)** state machine does it — a tiny dedicated coprocessor that can toggle pins with deterministic timing.

🛠 **`src/pio/probe.pio`** — the actual PIO assembly. It implements just three primitives:

```
.program probe
    ; instructions sized so that one PIO cycle = one half SWCLK period
    ; pin map: SWCLK = sideset 0, SWDIO = pin 0
    ...
    out pins, 1     side 0       ; drive SWDIO low half
    nop             side 1       ; SWCLK rises with stable data
```

🛠 **`src/hw/probe.c`** — the C wrapper. The functions `probe_write_bits(n, data)` and `probe_read_bits(n)` push counts and data words into the PIO's TX FIFO; the PIO clocks them out / samples them in.

🛠 **`src/dap/dap_swj.c`** — the line-reset / switch-sequence handler. When the host sends a `DAP_SWJ_Sequence` USB command, this is what bit-bangs it onto the wire. Read the file end-to-end — it's only ~160 lines and maps almost 1:1 to §1.3 above.

🛠 **`src/dap/dap_swd.c::swd_transfer()`** — the single-transaction engine. This is the C function that issues one of the 46-bit packets described in §1.2. Worth reading in full.

## Try it

After flashing edev_dapv2 to a Pico 2 and wiring SWCLK / SWDIO / GND / VTREF to any Cortex-M target:

🧪 **Confirm the wire works end-to-end:**

```bash
cd edev_dapv2/tools/edevocd
.venv/bin/pyocd nrf-dpidr -u <probe-uid>
# Expected output:
#   SWD DPIDR 0x6ba02477   (or 0x2ba01477 for nRF52 / many STM32)
#   designer=0x23B version=2 (DPv2) partno=0xBA revision=6
```

If you get this, the entire stack — USB → CMSIS-DAP → PIO → SWD wire → target DP — is working.

🧪 **See OpenOCD do the same thing with debug logging:**

```bash
openocd -d3 -c "adapter driver cmsis-dap; cmsis-dap vid_pid 0x2e8a 0x000c; adapter speed 1000; transport select swd" -f target/nrf52.cfg -c "init; shutdown" 2>&1 | grep -E "SWJ|DPIDR|cmsis_dap_swj"
```

The output shows OpenOCD constructing the exact SWJ sequence we discussed in §1.3 and seeing `DPIDR` come back.

🧪 **Hook a logic analyzer to SWCLK and SWDIO** (Saleae, sigrok, etc.). Set sample rate ≥10× SWCLK frequency. Trigger on SWDIO going high. Run `nrf-dpidr` once. You'll see exactly the §1.2 packet shape — start bit, 4 payload bits, parity, stop, park, turnaround, 3-bit ACK, 32-bit DPIDR data, parity, idle. Beautiful.

## Gotchas

⚠️ **Pull-up matters.** Some target boards have no pull-up on SWDIO and rely on the probe to supply one. edev_dapv2's PIO drives SWDIO actively in both directions but has no fixed pull-up — if the chip floats SWDIO during the turnaround window, you can see glitches. The fix in our PIO is to make the turnaround time configurable (`DAP_SWD_Configure` host command). 1 cycle works for most chips; 4 cycles for paranoia.

⚠️ **Clock too fast = wire noise.** edev_dapv2 PIO supports up to ~25 MHz SWCLK, but a 30 cm jumper-wire harness is good for maybe 1-2 MHz before signal integrity suffers. Always start at 1 MHz; speed up only after the wire's verified short and clean.

⚠️ **First transaction after line reset MUST be DPIDR read.** This deserves its own chapter (Chapter 2) because it bit us during edev_dapv2 development. If you skip this, the target's DP stays in "line-reset state" and silently drops every subsequent write.

⚠️ **`0xE79E` is sometimes too short for ADIv6 (DPv2) chips.** If your target is recent (post-2020-ish, ARMv8-M Mainline including nRF5340/nRF54L), the deprecated SWJ switch may be unreliable. pyocd defaults to the long dormant-state sequence; OpenOCD has an option. edev_dapv2's CLI uses the deprecated sequence today and it works for our targets — but if you see "No ACK" on a new chip after fresh attach, suspect this.

## Further reading

- **Chapter 2** — *The Debug Port* — what the target's DP actually does with the DPIDR read you just sent.
- **Chapter 6** — *Inside the dispatcher* — the full USB-to-PIO path inside edev_dapv2.
- **Saleae blog: "Reverse engineering SWD"** — excellent intro with logic analyzer captures: https://www.saleae.com/blog/swd-cortex-m-debug-port-analysis/
- **ARM IHI 0031 ADIv5 §B4** — the canonical spec. Long, but every paragraph is load-bearing.
