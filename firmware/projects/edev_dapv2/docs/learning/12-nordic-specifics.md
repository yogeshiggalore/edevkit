# Chapter 12 — Nordic Specifics: CTRL-AP, APPROTECT, ERASEALL

## What you'll learn

1. **What CTRL-AP is** and why Nordic chips have it in addition to the standard ARM AHB-AP.
2. **APPROTECT** — the access-port-protection lock that ships *enabled* on every modern Nordic chip — and the **two-way handshake** that opens it.
3. **ERASEALL** — the destructive last-resort recovery for chips that are fully locked.
4. **Why our `edevocd dump-flash --unlock` works on production Nordic devices** that "should" be impossible to read.

## Concept

### 12.1 The Nordic debug topology

Every modern Nordic chip (nRF52 newer build codes, nRF53, nRF54L, nRF91x1) ships with **two access ports** per CPU core:

```
PROBE ──SWD──→  DP  ──┬─→  AHB-AP   (standard ARM — drives the CPU bus)
                      │     ↑
                      │     └── BLOCKED by APPROTECT on factory chips
                      │
                      └─→  CTRL-AP  (Nordic-custom — always accessible)
                            │
                            ├── soft reset
                            ├── ERASEALL (mass erase)
                            ├── APPROTECT.DISABLE (handshake unlock)
                            ├── SECUREAPPROTECT.DISABLE (TZ-aware unlock)
                            └── Mailbox (firmware↔debugger comm)
```

The standard AHB-AP is what you'd use to read memory — but it's gated by APPROTECT. The CTRL-AP, however, is **always reachable** from the debugger regardless of protection state. It's the back door Nordic gives you to:

- Reset the chip
- Mass-erase the chip (and thereby unlock it)
- Negotiate a *handshake* with running firmware to open the AHB-AP

### 12.2 Per-family AP slot layout

| Family | AHB-AP (App) | AHB-AP (Net) | CTRL-AP (App) | CTRL-AP (Net) |
|--------|-------------|-------------|---------------|---------------|
| **nRF52** (832/833/840)  | AP[0] | — | **AP[1]** | — |
| **nRF5340**              | AP[0] | AP[1] | **AP[2]** | **AP[3]** |
| **nRF91** (Modem)        | AP[0] | — | **AP[4]** (gap intentional) | — |
| **nRF54L** (15, M20A)    | AP[0] | — | **AP[2]** | — |

Detection helper in `tools/edevocd/pyocd/subcommands/_edev_helpers.py::FLASH_MAP` keys off this topology + DPIDR.

### 12.3 The APPROTECT handshake — the secret most "unbrick" docs gloss over

Nordic's chips ship from the factory with `UICR.APPROTECT = Enabled`. On every hard reset, this latches the AHB-AP into a locked state. Standard debuggers see "AHB-AP is dead" and give up.

**Here's the trick most documentation buries**: APPROTECT is not a one-way lock. It's a **two-party handshake**. Per Nordic's *Debug and trace > Access port protection* docs:

> "Both the debugger and on-chip firmware must write the **same non-zero 32-bit KEY value** into their respective `CTRLAP.APPROTECT.DISABLE` registers (CPU-side and debugger-side) to disable the access port protection to non-secure mode."

When both sides have matching non-zero keys in their `APPROTECT.DISABLE` registers, the AHB-AP's `DBGEN` signal goes high and the AHB-AP becomes usable.

**The standard Nordic Connect SDK firmware does its half by default.** With `CONFIG_NRF_APPROTECT_USE_UICR` (the NCS default for nRF53 / nRF54L), the chip's `SystemInit()` reads `UICR.APPROTECT`; if it's Unprotected (`0x50FA50FA`), the boot code writes `0x50FA50FA` to its CPU-side `CTRL-AP.APPROTECT.DISABLE` register before main() runs.

**A production Nordic device whose firmware uses USE_UICR is debug-handshake-capable.** It looks locked from outside, but as soon as a debugger writes the matching key, the AHB-AP opens.

### 12.4 The handshake in wire ops

Debugger-side `APPROTECT.DISABLE` lives at offset 0x10 within the Nordic CTRL-AP. The full handshake is just one AP register write:

```
1. Select Nordic CTRL-AP bank 0:
     DP.SELECT = (apsel << 24)              ; APBANKSEL = 0 (low bank, where 0x10 lives)
2. Write the key:
     AP[apsel].0x10 = 0x50FA50FA            ; same key the firmware wrote
3. (Verify) Read AHB-AP.CSW (AP[0].0x00) and check DbgStatus bit 6.
     If 1 → AHB transfers PERMITTED → flash readable.
```

`edevocd dump-flash --unlock` does exactly this. In code (`edev_dump_cmd.py::invoke`):

```python
if self._args.unlock:
    apsel = NORDIC_CTRL_AP_SLOTS[family]              # e.g. 2 for nRF5340-App
    vcmd_ap_write(dap, apsel, 0x10, 0x50FA50FA)       # APPROTECT.DISABLE
    if family in ("nRF5340-App", "nRF54L15"):
        vcmd_ap_write(dap, apsel, 0x14, 0x50FA50FA)   # SECUREAPPROTECT.DISABLE
    st, csw = vcmd_ap_read(dap, 0, 0x00)              # AP[0].CSW
    # Now bit 6 (DbgStatus) should be 1, bit 23 (SPIStatus) for TrustZone targets too.
```

That's the whole magic. Three AP transactions and the "locked" chip is now readable.

> 📜 **Spec reference:** [nRF5340 docs — *Debug and trace > Access port protection*](https://docs.nordicsemi.com/bundle/ps_nrf5340/page/debugandtrace.html#ariaid-title9), and [*CTRL-AP > Disabling access port protection*](https://docs.nordicsemi.com/bundle/ps_nrf5340/page/ctrl-ap.html#ariaid-title6).

### 12.5 SECUREAPPROTECT (for TrustZone chips)

nRF5340 App-core and nRF54L have ARM TrustZone. The flash at `0x00000000` is the **secure region** by default (TF-M or similar runs there). With `APPROTECT.DISABLE` alone, you'd unlock non-secure debug but still FAULT on secure addresses.

To get full read access you must ALSO write `SECUREAPPROTECT.DISABLE` (offset 0x14, same key). After both, AP[0].CSW shows both `DbgStatus=1` and `SPIStatus=1` → both secure and non-secure transfers permitted. edevocd does this automatically when family is `nRF5340-App` or `nRF54L15`.

### 12.6 Why our reads still stop mid-burst on the Ring

Even with both handshakes done and CSW showing `DbgStatus=1, SPIStatus=1`, we observe consistent failure after **5–96 32-bit reads** mid-burst. The pattern is identical regardless of starting address (tested 0x0, 0x100, 0x10000).

Most likely causes (all chip-side, none our fault):

1. **Watchdog reset**: the running firmware has a WDT; on timeout the chip soft-resets, which **clears** `CTRL-AP.APPROTECT.DISABLE` per Nordic docs (*"...only reset by pin reset, brown-out reset, or watchdog timer reset"*). Handshake must be redone.
2. **Firmware actively re-enforcing**: the running firmware writes other security registers (SPU, SAU) that block specific transactions.
3. **Bus arbitration**: the firmware is a heavy bus master and our debugger transactions get starved/timed-out on the AHB matrix.
4. **Per-session anti-tamper escalation**: empirically the Ring's lock state *escalates* the more debug activity it sees. The first read after a long quiet period works (we get a fresh window of 5–96 words). After dozens of rapid lock cycles, even the disconnect/reconnect window stops opening. The chip appears to track a debug-activity counter and tighten countermeasures the more it's poked.

J-Link works around this with **continuous SWCLK keep-alive** — a slow background clock that signals "debugger is here" and prevents the chip from entering re-lock states. edev_dapv2's planned Tier 2 ⭐ PIO program does the same. See `[[08-sleepy-targets]]`.

### 12.6a Cross-validation against J-Link/nrfjprog (2026-06-19)

We wired one Ring to J-Link/nrfjprog and a sister Ring to edev_dapv2/edevocd. Both running the same firmware build. Result:

| Source | First 32 bytes | SP | Reset |
|---|---|---|---|
| J-Link/nrfjprog (Ring1) | `60 a9 04 20 39 33 00 00 6b 9a 00 00 25 33 00 00 …` | 0x2004A960 | 0x00003339 |
| edevocd/edev_dapv2 (Ring2) | `60 a9 04 20 39 33 00 00 6b 9a 00 00 25 33 00 00 …` | 0x2004A960 | 0x00003339 |

**384 / 384 bytes byte-identical** where both tools captured. The conclusion is unambiguous: **edevocd's handshake-and-bulk-read protocol is correct**. Every byte edevocd returns is real flash content, no aliasing, no stale pipeline data, no DRW-shift bug. See `[[edevocd-validated-2026-06-19]]`.

What still differs is **recovery rate**:

| Metric | nrfjprog/J-Link | edevocd/edev_dapv2 |
|---|---|---|
| Bytes typically recovered from 1 MB Ring | ~36.6% (~384 KB) | ~1 KB best case per session |
| Longest single contiguous run | 3,738 bytes | 256 bytes |
| Time spent per dump | several minutes | seconds |
| Errors encountered during dump | 12 `JLinkARM error -256` warnings | many `DP_FAIL`/`AP_FAIL` |
| Output correctness where captured | identical | identical |

The 36× recovery gap is **endurance**, not correctness. nrfjprog grinds through 5,685 separate small successful runs by retrying obsessively over minutes. edevocd ships a 4-tier retry hierarchy (light SWJ resend → smaller burst → CTRL-AP soft-reset rehandshake → DAP_Disconnect/Connect cycle) but matching nrfjprog's persistence likely requires the additional firmware-side ⭐ continuous SWCLK keep-alive of `[[08-sleepy-targets]]`.

### 12.7 The destructive last resort — ERASEALL

If `UICR.APPROTECT = Protected` (factory default; can be overridden by firmware writing Unprotected to UICR), the handshake doesn't work — debugger-side writes to `APPROTECT.DISABLE` are silently ignored.

The only way to read a fully-locked chip is to **erase it first**:

```
1. Select CTRL-AP, bank 0.
2. Write 1 to ERASEALL register (offset 0x004).
3. Poll ERASEALLSTATUS (offset 0x008) until ready (~200 ms typical, 30 s max).
4. After ERASEALL completes, AHB-AP is temporarily unlocked until the next reset.
   → flash + UICR + RAM are now ALL ZEROED. The chip is blank.
5. (re-program desired firmware before next reset to keep it unlocked)
```

This is what `nrfjprog --recover` does. It's a **destructive** operation — you lose the running firmware. For a production device (e.g. Ring) this is not what you want; it bricks the user's app.

`UICR.ERASEPROTECT` can further block even ERASEALL — if both APPROTECT and ERASEPROTECT are set, the chip is **truly unrecoverable** without authenticated debug (ADAC).

## In our code

🛠 **`tools/edevocd/pyocd/subcommands/edev_dump_cmd.py::invoke`** — the `--unlock` flag implementation. Three lines per write, completely host-side, no firmware change required.

🛠 **`tools/edevocd/pyocd/subcommands/edev_dump_cmd.py::NORDIC_CTRL_AP_SLOTS`** — per-family AP slot table.

🛠 **`src/dap/dap_vendor.c::do_edev_ap_write`** — the underlying `DAP_VENDOR 0x89` that performs the raw AP register write our handshake needs.

🛠 **`_edev_helpers.py::detect_chip_family`** + **`detect_chip_via_combo`** — discovery code that maps AP topology to family for slot lookup.

## Try it

🧪 **Run the handshake against any Nordic chip**:

```bash
cd edev_dapv2/tools/edevocd
.venv/bin/pyocd dump-flash --unlock --family nRF5340-App --start 0 --length 1024 -u <probe-uid>
```

Look for these stderr lines:

```
APPROTECT handshake: writing key 0x50FA50FA to AP[2].APPROTECT.DISABLE (0x10)
APPROTECT.DISABLE key written OK
SECUREAPPROTECT handshake: writing key 0x50FA50FA to AP[2].SECUREAPPROTECT.DISABLE (0x14)
SECUREAPPROTECT.DISABLE key written OK
AP[0].CSW = 0x03800052  →  DbgStatus=1, SPIStatus=1
→ both NON-SECURE and SECURE AHB transfers PERMITTED
```

`DbgStatus=1` is the proof: the handshake worked. Then you'll see however many words the chip lets you read before its anti-extraction kicks in.

🧪 **Webdbg button**: *Dump 1 KiB (Nordic APPROTECT handshake)* — same operation from the browser.

## Gotchas

⚠️ **The handshake only works if `UICR.APPROTECT = Unprotected`.** That's the NCS default with `CONFIG_NRF_APPROTECT_USE_UICR` (most non-paranoid production firmware). Truly-locked chips (factory APPROTECT enabled in UICR) refuse the write — they're ERASEALL-or-nothing.

⚠️ **`APPROTECT.LOCK` is write-once and prevents further unlock.** If firmware writes `APPROTECT.LOCK = Locked`, the `APPROTECT.DISABLE` register becomes read-only until next reset. Our handshake-write returns OK but does nothing.

⚠️ **Soft reset doesn't clear `APPROTECT.DISABLE`** — but **pin reset, brown-out reset, and watchdog reset DO**. If the chip's WDT fires, you have to redo the handshake.

⚠️ **`SECUREAPPROTECT` requires `APPROTECT` to be unlocked first.** Order matters: write the non-secure key first, then the secure key. edevocd does this.

⚠️ **Net core on nRF5340 needs its own handshake at AP[3].** The two cores' debug ports are independent. edevocd's default is App-core; pass `--family nRF5340-Net --unlock-apsel 3` for the network core.

## Further reading

- **Chapter 3** — *Access Port* — the AP fundamentals this builds on.
- **Chapter 4** — *Memory access via AHB-AP* — what's possible once the handshake opens the lock.
- **Chapter 8** — *Sleepy targets + SWCLK keep-alive* — Tier 2 ⭐ fix for the mid-burst re-lock we still see on the Ring.
- `[[flash-dump-shipped-2026-06-19]]` memory — `--unlock` shipping log.
- `[[nrf-family-reset-recover-matrix]]` memory — per-family CTRL-AP register offsets.
- Nordic docs:
  - [*Debug and trace > Access port protection*](https://docs.nordicsemi.com/bundle/ps_nrf5340/page/debugandtrace.html#ariaid-title9)
  - [*CTRL-AP > Disabling access port protection*](https://docs.nordicsemi.com/bundle/ps_nrf5340/page/ctrl-ap.html#ariaid-title6)
  - [*Allowing debugger access to nRF5340* (Nordic Blog)](https://devzone.nordicsemi.com/nordic/nordic-blog/b/blog/posts/allowing-debugger-access-to-an-nrf5340)
