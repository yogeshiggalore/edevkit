# Chapter 4 — Memory Access via AHB-AP

## What you'll learn

1. **The CSW/TAR/DRW dance** — three AP registers that combine into a complete memory access primitive.
2. **The auto-increment trick** — one setup sequence reads 64 words back-to-back with the chip's AHB matrix doing the address arithmetic for free.
3. **Reading a 1 MB flash image** — how `edevocd dump-flash` chains many vendor-cmd round trips into a complete image, what stops it, and what to do when it stops mid-stream.

## Concept

### 4.1 Three registers, two transactions

By the end of Chapter 3 you can read any **AP register** by issuing a DP `SELECT` + AP read + DP `RDBUFF`. But you want to read *memory* — the chip's flash, RAM, peripherals — not the AP's own status registers. That's what the **MEM-AP** is for, and it exposes exactly three special registers:

| AP offset | Name  | Purpose |
|-----------|-------|---------|
| `0x00`    | `CSW` | **Control & Status** — what kind of bus access (size, increment, security) |
| `0x04`    | `TAR` | **Target Address** — the memory address to read/write |
| `0x0C`    | `DRW` | **Data Read/Write** — every actual transfer happens here |

A single 32-bit memory read is three (or four) wire transactions:

```
1. AP write CSW = 0x23000012    (Size=Word, AddrInc=Single, standard HPROT)
2. AP write TAR = <address>
3. AP read  DRW                  ← posted, returns previous DRW; here it's stale
4. DP read  RDBUFF               ← this is your actual data word
```

The "stale" read at step 3 is the posted-pipeline behaviour from Chapter 2. For a single-word read, three transactions become four. For a multi-word read, the cost amortises (§4.4).

### 4.2 CSW bit fields

`CSW` is where most of the implementation-specific subtlety lives:

```
bit 31  30  29   27   24  23   12  11    7    5  4   2   0
    ┌──┴───┴────┴──────┴────────┴────────┴────┴─┴────┴────┐
    │NSE│NS │MD │   Prot   │   reserved   │M │AI│ S │ Size│
    └──┴───┴────┴──────────┴────────────────┴───┴──────────┘
     │   │    │     │                       │   │     │
     │   │    │     │                       │   │     └─ 0:7 byte, halfword, word, ... DWord(8)
     │   │    │     │                       │   └─── AddrInc: 0=off, 1=single, 2=packed
     │   │    │     │                       └─── DbgStatus (TrInProg, etc.)
     │   │    │     └─── HPROT bits — bus-protocol attributes (priv, cache, …)
     │   │    └─ MasterDbg — implementation-specific
     │   └─── HNONSEC for ARMv8-M: 0=secure, 1=non-secure
     └─ NSE (newer Trustzone bit)
```

edev_dapv2 uses two CSW values:

| Constant | Value        | Used when |
|----------|--------------|-----------|
| `CSW_WORD_NOINC` | `0x23000052` | single-word read/write (we rewrite TAR each time anyway) |
| `CSW_WORD_INC`   | `0x23000012` | bulk read/write (one TAR write, many DRW reads/writes) |

The `0x23` HPROT byte sets "data access, privileged, debug" — universally accepted. The `0x02` low byte is Size=Word. Bit 4 (AddrInc=Single) is set in `_INC` and clear (well — actually still set due to legacy in our codebase) in `_NOINC`.

> 📜 **Spec reference:** ADIv5 §C2 *Mem-AP Architecture* — every CSW field documented.

### 4.3 Single-word read in code

The most basic primitive in our firmware (`src/dap/dap_vendor.c::mem_r32`):

```c
static uint8_t mem_r32(uint32_t addr, uint32_t *out)
{
    uint8_t ack;
    uint32_t scratch;
    ack = ap_write(AP_CSW, CSW_WORD_NOINC);
    if (ack != DAP_TRANSFER_OK) return ack;
    ack = ap_write(AP_TAR, addr);
    if (ack != DAP_TRANSFER_OK) return ack;
    ack = ap_read_post(AP_DRW, &scratch);   /* posted; value is stale */
    if (ack != DAP_TRANSFER_OK) return ack;
    return dp_read(DP_RDBUFF, out);          /* the actual data */
}
```

Four wire transactions per 32-bit read. At our typical 1 MHz SWCLK that's roughly 200 µs/word, or 5 KB/s. Painful for a 1 MB flash dump (≈3 minutes). That's why §4.4 exists.

### 4.4 Auto-increment + posted reads

When you set `CSW.AddrInc=Single`, every DRW read makes the AHB-AP internally increment `TAR` by 4 (or 1, or 2 depending on Size). So you can:

1. Write CSW with AddrInc=Single.
2. Write TAR with the starting address.
3. Read DRW N times.
4. Drain the last value with DP RDBUFF.

Each DRW read is **one** wire transaction (because AP reads are posted — the value you get is the *previous* DRW read's data). So N words cost N+1 transactions instead of 4N:

```
                            Time →
   Probe:  CSW   TAR   DRW   DRW   DRW   DRW   …   RDBUFF
                          \    \    \    \           \
                           \    \    \    \           \
                            word[0]     word[N-2]    word[N-1]
                                word[1]   word[N-1]
                                  word[2]   ...
```

For N=64 words = 256 bytes, that's 1 CSW + 1 TAR + 64 DRW + 1 RDBUFF = 67 transactions. At 1 MHz SWCLK ≈ 3.5 ms. About **70× faster** than naive single-word reads.

`DAP_VENDOR 0x86 EDEV_MEM_READ` implements exactly this pattern in firmware:

🛠 **`src/dap/dap_vendor.c::do_edev_mem_read`** — the bulk-read implementation. ~30 lines of C. Worth reading top to bottom.

### 4.5 What stops a bulk read

The AHB-AP's auto-increment has hard limits:

1. **1 KB address boundary.** Most AHB matrices stop auto-incrementing at a 1 KB boundary. If your read crosses 0xC00 → 0x1000, the second half silently returns the same data as the first. ADIv5 says you MUST re-write TAR at each 1 KB boundary. edev_dapv2's firmware doesn't yet (mem_read max is 64 words = 256 bytes which never crosses), but a dumper that goes past 256 bytes needs to handle this. Our host-side `bulk_read()` chunks at 64 words and rewrites TAR via a fresh vendor-cmd call between chunks — so this is naturally avoided.

2. **Trustzone / APPROTECT.** A protected address triggers FAULT on the next DRW read. The mid-burst failure shows up as "got N/64 words, then ack=FAULT". Our combo-dump experience on the Ring's locked nRF5340: 10 words readable then FAULT (Chapter 12 expands).

3. **Address-not-mapped.** Reading at e.g. 0xDEADBEEF — many chips' AHBs return a bus-error response, which surfaces as FAULT.

4. **Bus busy / sleep state.** The system bus is gated by the chip's clock controller. If the CPU is in deep sleep AND the AHB clock is gated, even valid addresses return FAULT.

edev_dapv2's response in every case: stop, save what we got, surface the partial-data warning. Never pretend to return more bytes than we read.

### 4.6 Putting it together — `edevocd dump-flash`

The host CLI iterates `DAP_VENDOR 0x86` calls of up to 64 words each, accumulates the bytes, writes a file. The full algorithm:

```python
def bulk_read(dap, addr, size_bytes):
    out = bytearray()
    cursor = addr & ~3                    # 4-byte align
    remaining_words = (size_bytes + 3) // 4
    while remaining_words > 0:
        n = min(64, remaining_words)
        status, words = vcmd_mem_read_retry(dap, cursor, n)
        if status != 0 or len(words) < n:
            # partial — save what we got, abort
            for w in words: out += w.to_bytes(4, "little")
            break
        for w in words: out += w.to_bytes(4, "little")
        cursor += n * 4
        remaining_words -= n
    return bytes(out[:size_bytes])
```

For nRF5340 1 MB flash: 4096 vendor-cmd round trips, each ~3 ms = ~12 seconds total. The progress bar on stderr (`_stderr_progress` in `edev_dump_cmd.py`) shows live bytes-per-second.

### 4.7 Family-aware defaults

Different chips have different flash regions:

| Family | Flash base | Flash size | Notes |
|--------|------------|------------|-------|
| nRF52840 | 0x00000000 | 1 MB | + UICR @ 0x10001000 (4 KB), FICR @ 0x10000000 |
| nRF52833 / 832 | 0x00000000 | 512 KB | (some 832 variants 256 KB) |
| nRF5340 App | 0x00000000 | 1 MB | + UICR @ 0x00FF8000, FICR @ 0x00FF0000 |
| nRF5340 Net | 0x01000000 | 256 KB | per-core address space |
| nRF91 | 0x00000000 | 1 MB | UICR/FICR similar to nRF5340 |
| nRF54L15 | 0x00000000 | 1.5 MB | **RRAM**, not flash — 4-byte word writes, no per-page erase |
| STM32 F-series | 0x08000000 | varies | aliased at 0x00000000 too |

edevocd's `FLASH_MAP` (in `_edev_helpers.py`) records this. `detect_chip_family()` reads DPIDR + AP IDR fingerprint and picks the matching entry. If detection is ambiguous (same DPIDR for nRF5340 vs nRF54L) or partial (some APs locked), the user overrides with `--family <name>`.

## In the spec

- ADIv5 §C2 — Mem-AP architecture and CSW field definitions.
- ADIv5 §C2.5 — auto-increment behaviour, the 1 KB boundary rule.
- ARMv8-M ARM §B11 — Trustzone implications for HNONSEC and SPIDEN.

## In our code

🛠 **`src/dap/dap_vendor.c::mem_r32`** + **`mem_w32`** — single-word primitives.

🛠 **`src/dap/dap_vendor.c::do_edev_mem_read`** + **`do_edev_mem_write`** — `DAP_VENDOR 0x86`/`0x87` bulk handlers (up to 64 words/call).

🛠 **`tools/edevocd/pyocd/subcommands/edev_dump_cmd.py::bulk_read`** — host-side iterator that chains chunks.

🛠 **`tools/edevocd/pyocd/subcommands/_edev_helpers.py::FLASH_MAP`** — per-family memory map.

🛠 **`tools/edevocd/pyocd/subcommands/_edev_helpers.py::detect_chip_family`** — DPIDR + AP IDR → family table lookup.

## Try it

🧪 **Read 256 bytes from anywhere:**

```bash
.venv/bin/pyocd dump-mem 0xE000ED00 256 -u <probe-uid>
# Hex-dumps to stdout. Or save to file:
.venv/bin/pyocd dump-mem 0xE000ED00 256 -o /tmp/scb.bin -u <probe-uid>
```

🧪 **Dump the first 1 KB of flash, auto-detecting the chip family:**

```bash
.venv/bin/pyocd dump-flash --start 0 --length 1K -u <probe-uid>
```

You should see the vector table — the first 4 bytes are the initial stack pointer (e.g. `0x2004a960`), the next 4 are the reset vector address, then NMI / HardFault / etc.

🧪 **Dump the entire flash to a binary file:**

```bash
.venv/bin/pyocd dump-flash -o /tmp/firmware.bin -u <probe-uid>
# Takes ~12 s for 1 MB. Progress shows on stderr.
file /tmp/firmware.bin           # macOS: "data"
xxd /tmp/firmware.bin | head     # inspect
```

🧪 **Run it in a logic analyzer.** Hook SWCLK + SWDIO to a Saleae. Trigger on SWCLK rising edge. You'll see the CSW/TAR/DRW pattern from §4.4 — one slow setup, then 64 fast back-to-back DRW reads, then a single RDBUFF drain.

## Gotchas

⚠️ **AHB-AP gets locked dynamically.** On production Nordic chips with APPROTECT enabled, the chip's running firmware locks the AHB-AP within seconds of boot. First few reads work, then everything FAULTs. Diagnosis: AP IDR reads still succeed (Chapter 3) but memory reads stop. See `[[cortex-m-dump-shipped-2026-06-19]]` for our live capture on the Ring.

⚠️ **The 1 KB boundary rule is real.** edev_dapv2 doesn't trip on it because we cap bulk reads at 64 words (256 bytes < 1 KB always-safely). If you raise the limit, you MUST add TAR-rewrite logic at the boundary.

⚠️ **Reading peripherals can have side effects.** A read of `NVIC.ICPR` clears interrupts. A read of a UART data register pops a byte. Be careful when dumping arbitrary regions — stick to flash/RAM for safe casual reads.

⚠️ **`AddrInc=Packed` is rare and chip-specific.** Don't use it. `Single` is universal.

⚠️ **CSW HPROT can affect what's reachable.** With wrong HPROT, secure regions on ARMv8-M chips may FAULT even when chip is unlocked. edev_dapv2's 0x23 HPROT is a generic-Cortex-M choice; for Trustzone you may need to set NSE/HNONSEC bits.

## Further reading

- **Chapter 9** — *Cortex-M debug architecture* — what's at the SCS addresses you dump in §4.4.
- **Chapter 11** — *CoreSight ROM tables* — walk components by reading their config registers.
- **Chapter 12** — *Nordic specifics* — why APPROTECT-locked chips give partial dumps.
- `[[cortex-m-dump-shipped-2026-06-19]]` memory — the bulk SCB read + partial-data story.
- ADIv5 §C — Mem-AP normative reference.
