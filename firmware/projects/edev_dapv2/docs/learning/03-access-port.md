# Chapter 3 — The Access Port (AP) + nRF5340 Case Study

## What you'll learn

1. **What an AP is** and why a single chip has several of them.
2. **How to enumerate every AP on a target** (`apsel` scan + `IDR` decode), and what each AP's `IDR` actually tells you.
3. **A real case study**: identifying an unknown DPv2 chip on the wire from nothing but its AP topology — we'll arrive at "this is an nRF5340" without ever reading a single byte of memory.

## Concept

### 3.1 What's behind the DP

The DP is just the doorman. Behind it sits a varying number of **Access Ports (APs)**, each of which exposes a different subsystem of the chip to the debugger:

```
   PROBE──SWD──→  DP  ─────┬────→  AP[0]   (most commonly: AHB-AP for App-core memory)
                           ├────→  AP[1]   (could be: AHB-AP for Net-core, or APB-AP, or Nordic CTRL-AP)
                           ├────→  AP[2]   (Trustzone-AP, JTAG-AP, …)
                           ┊      …
                           └────→  AP[255] (rare; most chips have ≤8)
```

The DP routes a transaction to a specific AP based on the `APSEL` field of `DP.SELECT`. Once routed, the AP processes the request — typically by performing a transaction on some internal bus (AHB, APB, AXI) to actual hardware.

### 3.2 AP register layout

Each AP exposes 256 bytes of register space (64 four-byte registers, banked 4 banks of 16). The fixed-purpose ones:

| Offset | Name   | Direction | Purpose |
|--------|--------|-----------|---------|
| 0x00   | `CSW`  | R/W       | Control & status (size, address-increment, security, …) |
| 0x04   | `TAR`  | R/W       | Target address for memory access |
| 0x0C   | `DRW`  | R/W       | Data read/write register — the actual transfer happens here |
| 0xF8   | `BASE` | R         | Base address of attached CoreSight ROM table |
| 0xFC   | `IDR`  | R         | Identification register — class, designer, type, version |

`CSW`, `TAR`, `DRW` are the bulk of what you use for memory access (Chapter 4). `IDR` and `BASE` are what you use during *discovery* — figuring out what's on the wire.

### 3.3 Reading an AP register

Because the DP only has a 4-bit address bus, reaching offsets 0xF8 and 0xFC requires banking. You point at the bank with `DP.SELECT`, then read the AP at offsets 0x00/0x04/0x08/0x0C (which within that bank correspond to 0xF0/0xF4/0xF8/0xFC):

```c
// Read AP[apsel].IDR:
dp_write(DP_SELECT, (apsel << 24) | (0xF << 4));   // APSEL=apsel, APBANKSEL=0xF
ap_read(0xC, &junk);                                // posted — discard value
dp_read(DP_RDBUFF, &idr);                           // ← here's the actual IDR
```

Note the **posted-read pattern** from Chapter 2: the value you get on the AP read transaction is *the previous AP read*. You drain the real value via `RDBUFF`.

### 3.4 Decoding `IDR`

`IDR` is 32 bits of "what am I":

```
bit  31  28│27  24│23     17│16  13│12   8│7   4│3   0
    ┌─────┴──────┴─────────┴──────┴──────┴─────┴─────┐
    │ REV │ JEPC │ JEP106  │CLASS │ VAR  │ TYPE│ 0   │
    └─────┴──────┴─────────┴──────┴──────┴─────┴─────┘
       │     │       │        │       │      │
       │     └─┬─────┘        │       │      │
       │       │              │       │      │
       │  Continuation+ID     │       │      └─ Reserved
       │  of designer         │       └─── Type within class
       │  (JEP-106 codes)     └─── 0x8=MEM-AP, 0x9=JTAG-AP, others custom
       │
       └─ Revision
```

The **CLASS** field is the load-bearing one:

| CLASS | Meaning |
|-------|---------|
| 0x0   | Custom / non-standard (Nordic CTRL-AP, e.g.) |
| 0x1   | Reserved |
| 0x8   | **MEM-AP** — memory-mapped subsystem (AHB, APB, AXI) |
| 0x9   | JTAG-AP — exposes a downstream JTAG TAP chain |

For CLASS=8 MEM-APs, the **TYPE** field tells you which bus protocol:

| TYPE | Protocol             |
|------|----------------------|
| 0x1  | AMBA AHB-3 (Cortex-M memory bus most often)|
| 0x2  | AMBA APB-2/3 (slower peripheral bus)        |
| 0x4  | AMBA AXI3/4 (Cortex-A class) |
| 0x6  | AMBA AHB-5 (newer M-class)   |
| 0x7  | AMBA APB-4/5                 |
| 0x8  | AMBA AXI5                    |

The **JEP-106 designer code** (cont + id) identifies the chip vendor. Most useful values:

| (cont, id) → composite | Vendor |
|-------------------|--------|
| (4, 0x3B) → 0x23B | ARM |
| (2, 0x44) → 0x144 | Nordic Semiconductor |
| (4, 0x7F) → 0x47F | Apple |
| (0, 0x20) → 0x020 | STMicroelectronics |
| (1, 0x15) → 0x095 | NXP |

(edev_dapv2 maintains the table in `tools/edevocd/pyocd/subcommands/edev_info_cmd.py::DESIGNER_LABEL`.)

### 3.5 Scanning every AP

The recipe is just a loop:

```c
for (apsel = 0; apsel < 8; apsel++) {
    dp_write(DP_SELECT, (apsel << 24) | (0xF << 4));   // APBANKSEL = 0xF (bank with IDR/BASE)
    ap_read(0xC, &junk);
    dp_read(DP_RDBUFF, &idr);
    if (idr == 0) {
        // empty slot — apsel not populated
    } else {
        // decode idr; if CLASS=8, read BASE too:
        dp_write(DP_SELECT, (apsel << 24) | (0xF << 4));
        ap_read(0x8, &junk);                    // offset 0xF8 within bank
        dp_read(DP_RDBUFF, &base);
    }
    dp_write(DP_ABORT, 0x1F);  // clear sticky between iterations (Chapter 2)
}
```

The implementation in our firmware is `src/dap/dap_vendor.c::do_edev_cortex_m_dump()` — the AP scan loop runs all this in **one** vendor command so the chip doesn't get a chance to sleep between iterations.

> ⚠️ **Clear sticky between iterations.** If AP[N] returns FAULT, the DP latches a sticky error and AP[N+1..7] all fail too. The clear is one line; forgetting it makes the scan appear to find one AP instead of four. We hit this exact bug.

## A real case study — identifying the Ring on our bench

This whole tour pays off when you point edev_dapv2 at an unknown chip and ask "what is this?" — without flashing anything, reading any memory, or even halting the CPU.

### 3.6 The data

Running `edevocd info` against an unidentified target on our bench:

```
DPIDR  0x6BA02477
  designer  0x23B   (ARM)
  version   2       (DPv2)
  partno    0xBA
  revision  6

APs (8 slots scanned):
  AP[0]   IDR=0x84770001   MEM-AP (AMBA AHB-3)   designer ARM   BASE=0xE00FE003
  AP[1]   IDR=0x84770001   MEM-AP (AMBA AHB-3)   designer ARM   BASE=0xE00FE002
  AP[2]   IDR=0x12880000   custom/non-standard   designer Nordic   BASE=0
  AP[3]   IDR=0x12880000   custom/non-standard   designer Nordic   BASE=0
  AP[4..7]  empty
```

### 3.7 The deductions

**Deduction 1: it's an ARM-based chip.** DPIDR designer = 0x23B = ARM. Trivial.

**Deduction 2: it's DPv2.** Version field = 2. That rules out old chips and tells us we'll need dormant-state wakeup for some host tools — Chapter 1's gotcha.

**Deduction 3: it has TWO ARM AHB-APs.** AP[0] and AP[1] both report CLASS=8, TYPE=1, designer=ARM. AHB-3 is the bus protocol Cortex-M uses for memory. Two of them on one chip means **two Cortex-M cores** — each gets its own AHB-AP, so the debugger can attach to one core without disturbing the other.

**Deduction 4: it has TWO Nordic custom APs.** AP[2] and AP[3] have CLASS=0 (custom), designer=Nordic, identical IDRs. On Nordic chips, the custom AP is always the **CTRL-AP** — Nordic's recovery-and-control extension that exists *in parallel* to the standard AHB-AP. Two of them means **per-core CTRL-APs**.

**Putting it together:** two ARM AHB-APs + two Nordic CTRL-APs + Cortex-M class + DPv2. That topology is unique to one chip family: **Nordic nRF5340**. App core + Network core, each with its own AHB-AP and its own CTRL-AP.

We never read CPUID. We never halted the chip. We never wrote a byte. From four `IDR` register reads (32 bits each), we got a positive ID. That's how powerful AP scanning is.

### 3.8 The other AP[1] hint

There's one more breadcrumb in the data: `AP[0].BASE = 0xE00FE003`, `AP[1].BASE = 0xE00FE002`. They differ in bit 0 / bit 1, which is the BASE register's "Format" + "Present" flags:

```
BASE bit 0: 1=ROM table present
BASE bit 1: 1=32-bit ROM table format (ADIv5), 0=8-bit (legacy)
```

`0xE00FE003` = present + 32-bit format → App core's ROM table is at `0xE00FE000` and accessible.
`0xE00FE002` = present (0) + 32-bit format → Net core's ROM table indicated at same address but **not currently accessible**.

That nuance — that the Net core's debug ROM table is "advertised but locked" — is itself another fingerprint of nRF5340: Nordic ships the Net core in a debug-locked state by default. Chapter 12 dives into why.

## In the spec

- ADIv5 §B3 — *Access Port architecture*.
- ADIv5 §B3.4 — `IDR` format (class, designer, type).
- ADIv5 §B3.5 — `BASE` format (ROM table base).
- ADIv5 §B4.2.4 — bank selection via `SELECT.APBANKSEL`.

## In our code

🛠 **`src/dap/dap_vendor.c::do_edev_cortex_m_dump`** — the per-AP scan loop, with sticky-clears between iterations.

🛠 **`src/dap/dap_vendor.c::select_ap_bank`** — three lines that build the `SELECT` register value from `apsel` and `apreg`:

```c
static uint8_t select_ap_bank(uint8_t apsel, uint8_t apreg)
{
    uint32_t sel = ((uint32_t)apsel << 24) | ((uint32_t)apreg & 0xF0u);
    return dp_write(DP_SELECT, sel);
}
```

🛠 **`tools/edevocd/pyocd/subcommands/edev_info_cmd.py::_decode_ap_idr`** — host-side decoder. Three lookup tables (`AP_CLASS`, `MEM_AP_TYPE`, `DESIGNER_LABEL`) and `IDR` becomes human-readable.

🛠 **`DAP_VENDOR 0x88 EDEV_AP_READ`** — the firmware-side primitive that lets the host read any AP register at any apsel without doing the full SELECT/READ/RDBUFF dance itself. (Chapter 7 details vendor commands.)

## Try it

🧪 **Scan all APs on whatever target is attached:**

```bash
cd edev_dapv2/tools/edevocd
.venv/bin/pyocd info -u <probe-uid>
```

The `APs` section of the output is everything we discussed in §3.5–3.7. Compare to other targets:

| Chip                | What you should see |
|---------------------|---------------------|
| nRF52840            | AP[0] AHB-AP (ARM) + AP[1] Nordic CTRL-AP |
| nRF5340             | AP[0,1] AHB-AP (ARM) + AP[2,3] Nordic CTRL-AP |
| nRF91 series        | AP[0] AHB-AP + AP[4] Nordic CTRL-AP (gap is intentional) |
| STM32F4 / F7        | AP[0] AHB-AP + AP[1] APB-AP |
| RP2040 / RP2350     | AP[0] Cortex-M0+/M33 AHB-AP only |

🧪 **Read a specific AP register directly:**

```bash
# Read AP[0].CSW
.venv/bin/python -c "
from pyocd.probe.pydapaccess import DAPAccess
from pyocd.probe.pydapaccess.dap_access_api import DAPAccessIntf
from pyocd.subcommands._edev_helpers import vcmd_ap_read

dap = next(d for d in DAPAccess.get_connected_devices() if 'E464' in d.get_unique_id())
dap.open()
dap.connect(DAPAccessIntf.PORT.SWD)
dap.swj_sequence(51, 0xffffffffffffff); dap.swj_sequence(16, 0xe79e)
dap.swj_sequence(51, 0xffffffffffffff); dap.swj_sequence(8, 0)
print('AP[0].CSW =', hex(vcmd_ap_read(dap, 0, 0x00)[1]))
"
```

## Gotchas

⚠️ **AP read of empty slot may NOT return zero.** Some chips return `IDR=0`, others return 0xFFFFFFFF, others FAULT. edev_dapv2 treats `IDR=0` as "empty" but you should also handle the other cases in production code.

⚠️ **`BASE = 0xFFFFFFFF`** means "no ROM table on this AP". Don't try to walk it — you'll just get garbage. `BASE = 0` means "no debug components attached".

⚠️ **Multi-drop SWD chips have a `TARGETID` register** (DP register at extended bank 2). edev_dapv2 doesn't currently handle multi-drop. If you ever see two chips on one SWD wire, that's a separate flowchart entirely — ADIv5 §B5.

⚠️ **`SELECT` is per-DP, not per-AP.** Writing to `SELECT` changes the routing for ALL subsequent AP transactions until the next `SELECT` write. Forgetting to re-`SELECT` after changing banks is a classic bug.

## Further reading

- **Chapter 4** — *Memory access via AHB-AP* — uses everything in this chapter as the substrate.
- **Chapter 11** — *CoreSight ROM tables* — what `BASE` points at, and how to walk it to find DWT, FPB, ITM, ETM.
- **Chapter 12** — *Nordic specifics* — what's actually inside those `0x12880000` CTRL-APs and how Nordic uses them for ERASEALL recovery.
- The **AP IDR list** maintained in our `nrf-uh-dapv2-master-brief` memory has CTRL-AP IDR values for every nRF family.
