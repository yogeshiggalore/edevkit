# Chapter 2 — The Debug Port (DP)

## What you'll learn

1. **What the DP is** — the "always alive" doorman at the front of every ARM debug interface, and its five small registers.
2. **The DPIDR-first rule** — the most important rule in ADIv5, why it exists, and the silent-failure bug it causes when you violate it.
3. **How to power up the DP** — `CDBGPWRUPREQ`/`CSYSPWRUPREQ` and why this dance is mandatory.

## Concept

### 2.1 What the DP is

If SWD is the *wire*, the **Debug Port (DP)** is the *first thing on the chip that listens to that wire*. It's a tiny piece of hardware (only 5 user-visible registers!) that lives in the chip's debug power domain — alive whenever the chip has VDD, regardless of whether the CPU is running, halted, in deep sleep, or in System OFF (mostly — Chapter 8 covers the cracks).

The DP's job is gatekeeping:

```
PROBE          DP            AP            SYSTEM
SWD wire ──→  4-bit       ──→  bank    ──→  RAM, flash,
              register        of AP regs    Cortex-M debug,
              window                        Nordic peripherals,
                                            ...
```

You send the DP a 4-byte-aligned register address (one of 0x0, 0x4, 0x8, 0xC), a read/write bit, and the data. The DP processes that transaction itself, OR forwards it to the AP you've selected via the `SELECT` register.

### 2.2 The five DP registers

ADIv5 maps just five "always-visible" registers at offsets 0x00..0x0C, distinguished by R/W direction:

| Offset | Read returns      | Write goes to    | Meaning |
|--------|-------------------|-------------------|---------|
| 0x00   | `DPIDR`           | `ABORT`           | Identification / clear sticky errors |
| 0x04   | `CTRL/STAT`       | `CTRL/STAT`       | Power-up request, sticky-error status |
| 0x08   | (depends on bank) | `SELECT`          | Choose AP + bank for upper-bank regs |
| 0x0C   | `RDBUFF`          | (no-op)           | Drain the AP-read pipeline (see §2.4) |

The clever trick: read at 0x00 returns DPIDR; **write at 0x00 goes to ABORT**. So a single 4-bit address bus exposes 5+ logical registers. (Higher banks exist too, accessed by writing the bank into `SELECT`, but you rarely need them.)

### 2.3 `DPIDR` — your handshake with the chip

`DPIDR` is the register that proves the wire works. It has a fixed value baked into the silicon and identifies the chip family. Field layout:

```
bit  31    28│27    20│19    16│15    12│11      1│0
    ┌───────┴────────┴────────┴────────┴─────────┤
    │REV    │PARTNO  │  MIN   │ VERSION│ DESIGNER│1
    └───────┴────────┴────────┴────────┴─────────┘
     4 bits   8 bits   4 bits    4 bits   11 bits  (always 1)
```

Examples we've seen on the bench:

| Chip family            | DPIDR        | DESIGNER | VERSION | Meaning |
|------------------------|--------------|----------|---------|---------|
| nRF52 series           | `0x2BA01477` | 0x23B (ARM) | 1 (DPv1) | A in PARTNO, single-DP chip |
| nRF5340 / nRF54L       | `0x6BA02477` | 0x23B (ARM) | 2 (DPv2) | Has MINDP + DPv2 extensions |
| Most STM32 F0/F1/L0    | `0x2BA01477` | 0x23B (ARM) | 1 (DPv1) | Same as nRF52 |

DPv2 (version=2) chips support **dormant-state wakeup** and **multi-drop SWD** (multiple DPs on one wire — rare). DPv1 is the "classic" version.

> 📜 **Spec reference:** ADIv5 §B2.2.1 *DP target identification register, DPIDR*.

### 2.4 The pipeline: AP reads are posted

Reading from a DP register returns the data on the very same transaction. Easy.

Reading from an **AP** register through the DP is different: AP reads are **posted**. When you issue an AP read at offset 0xC (DRW), the data that comes back on that SWD transaction is the result of the *previous* AP read. The AP read you just issued goes into the DP's internal "RDBUFF" buffer; you collect it on the next AP read, or by reading `DP.RDBUFF` (offset 0x0C).

```
Probe sends:    AP read DRW       AP read DRW       DP read RDBUFF
Probe gets:     <garbage>         <DRW data 1>      <DRW data 2>
                  ↑                 ↑                 ↑
        first AP read      first valid value   final drain
        kicks off pipeline
```

This pipeline lets a probe queue up many AP reads back-to-back without round-trip latency on each one — important for fast memory dumps. (Chapter 4 walks through the consequences in detail.)

> 📜 **Spec reference:** ADIv5 §B2.2.4 *Read Buffer, RDBUFF*.

### 2.5 The ADIv5 §B4.2.2 rule (the gotcha)

After any **line reset** (Chapter 1), the DP enters a specific state where:

> *"The first transaction after a line reset MUST be a read of `DPIDR`. Other transactions in this state are silently ignored."*

Silently. The wire ACKs WAIT or NO_ACK or FAULT, but the actual register write *does not take effect*. This is the trap that wasted hours during edev_dapv2 development:

We had a vendor command that did, in order:

1. line reset
2. `dp_write(ABORT, 0x1F)` to clear sticky errors
3. `dp_write(CTRL/STAT, 0x50000000)` to request power-up
4. poll `CTRL/STAT` for the power-up ACK

Step 4 timed out. Our log said `DP_FAIL`. The wire was fine. The probe was fine. The target was fine.

The bug: step 2 should have been `dp_read(DPIDR)`. The line reset put the DP into "expecting DPIDR" state, so the ABORT write was silently dropped, and the CTRL/STAT write also went into the void. The target sent back ACKs that *looked* successful (because the wire-level handshake completed) — but no register actually changed.

After we fixed it:

```c
// In src/dap/dap_vendor.c::dp_init()
swj_line_reset();
ack = dp_read(DP_ABORT /* =0x00, reads DPIDR */, &scratch);  // ← REQUIRED FIRST XACT
if (ack != DAP_TRANSFER_OK) return ack;
ack = dp_write(DP_ABORT, 0x1Fu);     // now ABORT write is honoured
...
```

`reset OK` in 13 ms. Done.

> ⚠️ **This is the most important rule in ADIv5.** OpenOCD does it correctly. pyocd's `DAPAccess.connect()` does NOT — it expects the target plugin to do the DPIDR read separately. probe-rs does it. Any custom probe firmware MUST do it. Always.

See the dedicated reference memory `[[adiv5-first-xact-dpidr]]` for the exhaustive variant of this discussion.

### 2.6 Power-up: `CDBGPWRUPREQ` + `CSYSPWRUPREQ`

After DPIDR is acknowledged, the DP is alive but the **debug power domain** and the **system power domain** are still suspended. You ask the DP to power them up:

```c
dp_write(DP_CTRL_STAT, 0x50000000);
//                      │└ CDBGPWRUPREQ (bit 28) — power up debug domain
//                      └─ CSYSPWRUPREQ (bit 30) — power up system domain
```

Then poll until both ACKs are visible:

```c
do {
    dp_read(DP_CTRL_STAT, &v);
} while ((v & 0xA0000000) != 0xA0000000);
//                ││
//                │└─ CDBGPWRUPACK (bit 29)
//                └── CSYSPWRUPACK (bit 31)
```

Without this, AHB-AP transactions to memory will hang or return FAULT. The system domain has to be powered for the AHB matrix to respond.

> 📜 **Spec reference:** ADIv5 §B2.2.2 *Control/Status register, CTRL/STAT*.

### 2.7 The five sticky bits

`CTRL/STAT` upper bits report sticky errors. Once set, the DP refuses subsequent transactions until cleared:

| Bit | Name        | Meaning |
|-----|-------------|---------|
| 1   | `STKCMPCLR` | Sticky compare (match-mask read failed) |
| 2   | `STKERRCLR` | Sticky error from AP |
| 3   | `WDATAERRCLR` | Write-data parity error |
| 4   | `READOK`    | Last AP read succeeded |
| 5   | `STICKYORUN`| Overrun detected |

Clear them via `ABORT`:

```c
dp_write(DP_ABORT, 0x1F);  // 0x1F = clear all sticky bits + DAPABORT
```

In our combo command (`dap_vendor.c::do_edev_cortex_m_dump`) we clear sticky between every AP scan iteration. Without that single line, a single bad AP cascades into making the next 5 succeed-on-paper but actually return junk.

## In the spec

- ADIv5 §B2 — *Debug Port architecture*. Reads like a reference card — short, dense.
- ADIv5 §B2.2.1 — `DPIDR`.
- ADIv5 §B2.2.2 — `CTRL/STAT`.
- ADIv5 §B2.2.3 — `ABORT`.
- ADIv5 §B2.2.4 — `RDBUFF`.
- ADIv5 §B4.2.2 — the "first xact must be DPIDR" rule.

## In our code

🛠 **`src/dap/dap_vendor.c::dp_init()`** (~lines 200-260) — the canonical DP wake-up sequence. Line reset → DPIDR read → ABORT clear → power-up → poll. This is the function every other vendor command in our firmware calls first. Worth reading top to bottom.

🛠 **`src/dap/dap_vendor.c::dp_read()` and `dp_write()`** — thin wrappers around `swd_transfer()` that bake the DP/AP and R/W bits into the request byte. They're 3 lines each. The whole point is that *almost all* DP access reduces to choosing 4 bits.

🛠 **`src/dap/dap_swd.c::swd_transfer()`** — the 46-bit packet wire engine. The DP layer sits directly on top.

## Try it

🧪 **Read DPIDR with full debug logging to see the full sequence:**

```bash
.venv/bin/pyocd nrf-dpidr -u <probe-uid> -v 2>&1 | grep -E "DPIDR|line reset"
```

Or for the most detailed view (host-side wire trace) once you've added trace hooks in pyocd:

```bash
.venv/bin/pyocd nrf-dpidr -L "pyocd.probe.pydapaccess=debug" -u <probe-uid> 2>&1 | head -20
```

🧪 **See it the other way — through OpenOCD's eyes:**

```bash
openocd -d3 -c "adapter driver cmsis-dap; cmsis-dap vid_pid 0x2e8a 0x000c; transport select swd" -f target/nrf52.cfg -c "init; shutdown" 2>&1 | grep -E "DPIDR|CSYS|CDBG"
```

You'll see OpenOCD log the DPIDR value, then the `CDBGPWRUPACK` / `CSYSPWRUPACK` confirmation.

## Gotchas

⚠️ **The first-xact rule is silent.** No warning, no error, just downstream weirdness. Whenever you start debugging "my DP works for reads but writes don't stick" — check that you read DPIDR first.

⚠️ **`ABORT` is at offset 0x00 (write). DPIDR is at offset 0x00 (read).** This sounds like a footgun and it sort of is. The probe firmware's `dp_read(0x00, …)` and `dp_write(0x00, …)` go to different physical registers. Many SWD tutorials gloss over this; ours doesn't.

⚠️ **CTRL/STAT power-up takes time.** On most chips it completes in under 100 µs. On some chips with very deep sleep states or hierarchical power domains, it can take 10+ ms. Set a generous timeout (we use 100 × 1 ms = 100 ms total) — too aggressive a timeout will spuriously fail on slow targets.

⚠️ **Sticky errors propagate.** A single bad AP transaction sets a sticky bit; every transaction after that returns FAULT until cleared. If your probe scans 8 APs and the first one fails, the other 7 will *also* fail unless you clear sticky after each one. This is exactly the bug that gave us cascading "AP_FAIL" output before we added inter-AP sticky-clears to `src/dap/dap_vendor.c` (see the AP scan loop in `do_edev_cortex_m_dump`).

## Further reading

- **Chapter 3** — *The Access Port* — what `SELECT` selects, and the rich world on the other side of the DP.
- **Chapter 9** — *Cortex-M debug architecture* — how the Cortex-M's debug-power-domain interacts with the DP's `CDBGPWRUPREQ`.
- **`[[adiv5-first-xact-dpidr]]`** memory — the bug write-up with every reproducer.
- ARM IHI 0031 §B2 — the canonical reference.
