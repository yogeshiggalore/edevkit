# `edevocd dump-mem` and `edevocd dump-flash` — bulk memory / flash dump
# via iterated DAP_VENDOR 0x86 (EDEV_MEM_READ) calls.
#
# SPDX-License-Identifier: Apache-2.0

import argparse
import json
import logging
import sys
import time
from pathlib import Path
from typing import List, Optional

from .base import SubcommandBase
from ._edev_helpers import (
    open_probe_swd, vcmd_mem_read, vcmd_mem_read_retry,
    detect_chip_family, detect_chip_via_combo, FLASH_MAP,
    parse_int_base_0, parse_size, status_name,
    VCMD_NRF_SYS_RESET, vcmd_ap_write, vcmd_ap_read,
    vcmd_mem_write,
    write_cpu_reg, cpu_halt, cpu_run, cpu_wait_halt,
    REG_R0, REG_R1, REG_R2, REG_MSP, REG_PC, REG_XPSR,
    ADDR_DHCSR, DHCSR_DBGKEY, DHCSR_C_DEBUGEN, DHCSR_C_HALT, DHCSR_S_HALT,
)
from ..flash_algos.cortex_m_read import (
    ALGO_THUMB,
    DEFAULT_ALGO_ADDR, DEFAULT_BUFFER_ADDR, DEFAULT_STACK_TOP,
    DEFAULT_BUFFER_WORDS_MAX,
    NET_CORE_ALGO_ADDR, NET_CORE_BUFFER_ADDR, NET_CORE_STACK_TOP,
    NET_CORE_BUFFER_WORDS_MAX,
)

# Cortex-M debug registers used for the post-handshake CPU halt.
# Per Nordic docs: WDT defaults to "pause while CPU halted by debugger",
# so halting the CPU here also pauses the watchdog → APPROTECT.DISABLE
# key survives → AHB-AP stays open for the full dump.
ADDR_DHCSR        = 0xE000EDF0
DHCSR_DBGKEY      = (0xA05F << 16)
DHCSR_C_DEBUGEN   = (1 << 0)
DHCSR_C_HALT      = (1 << 1)
DHCSR_S_HALT      = (1 << 17)


# Nordic CTRL-AP register layout (debugger-side, per nRF5340/52/91/54L docs).
# These are the same offsets across the entire Nordic family.
CTRL_AP_RESET               = 0x00   # RESET — soft reset; works even when AHB-AP is locked
CTRL_AP_APPROTECT_DISABLE   = 0x10   # APPROTECT.DISABLE — handshake key for non-secure debug
CTRL_AP_SECUREAPPROTECT_DIS = 0x14   # SECUREAPPROTECT.DISABLE — handshake key for secure debug
CTRL_AP_ERASEPROTECT_DIS    = 0x1C   # ERASEPROTECT.DISABLE — handshake key for ERASEALL

# Standard Nordic "unlock" key — matches the value the chip's SystemInit()
# writes to its CPU-side CTRL-AP register on boot when UICR.APPROTECT is
# Unprotected. Any non-zero key works as long as both sides match; this
# is the convention.
NORDIC_UNLOCK_KEY = 0x50FA50FA

# Nordic CTRL-AP slot per family. The chip-family detection table tells
# us which one to use.
NORDIC_CTRL_AP_SLOTS = {
    "nRF52840":    1,
    "nRF52833":    1,
    "nRF52832":    1,
    "nRF5340-App": 2,
    "nRF5340-Net": 3,
    "nRF91xx":     4,
    "nRF54L15":    2,
}

LOG = logging.getLogger(__name__)


# ──────────────────────────────────────────────────────────────────
#                    Bulk-read iterator
# ──────────────────────────────────────────────────────────────────

CHUNK_WORDS_DEFAULT = 64  # matches DAP_VENDOR 0x86's EDEV_MEM_MAX_WORDS in firmware


# ────────────────────────────────────────────────────────────────────
#  Algorithm-based bulk read — runs cortex_m_read.S on the target CPU
#  so flash reads bypass the AHB-AP anti-tamper throttle.
# ────────────────────────────────────────────────────────────────────

def algo_bulk_read(dap, addr: int, size_bytes: int,
                   algo_addr:   int = DEFAULT_ALGO_ADDR,
                   buffer_addr: int = DEFAULT_BUFFER_ADDR,
                   stack_top:   int = DEFAULT_STACK_TOP,
                   batch_words: int = DEFAULT_BUFFER_WORDS_MAX,
                   on_progress=None,
                   json_progress: bool = False,
                   time_budget_sec: float = 30.0) -> tuple[bytes, list[str]]:
    """Bulk-read `size_bytes` from `addr` by running the Thumb-2 algo on
    the target CPU.

    Sequence per batch:
      1. Set R0 = current source addr
      2. Set R1 = batch word count
      3. Set R2 = RAM buffer addr
      4. Set MSP = stack top
      5. Set PC = algo entry
      6. Set XPSR = 0x01000000  (Thumb bit)
      7. Run CPU (clear DHCSR.C_HALT)
      8. Poll DHCSR.S_HALT until 1  (algo's BKPT halts the CPU)
      9. Read N words from RAM buffer via AHB-AP block read

    All AHB-AP traffic per N=1024-word batch:
      ~6 register writes + 1 N-word RAM read.
    Per 1 MB dump: ~16 batches × 7 ops ≈ 112 AHB-AP transactions.
    Compare to host-driven mode: 16384 AHB-AP transactions (147× more).

    Caller must have already done APPROTECT handshake + halt the CPU
    BEFORE calling this. If AHB-AP isn't open we'll FF-fill and return.
    """
    if size_bytes <= 0:
        return b"", []
    word_count = (size_bytes + 3) // 4
    out = bytearray()
    errors: list[str] = []
    start = time.monotonic()

    # 1) Upload the algorithm bytes (16 bytes = 4 words) — one mem_write
    #    call. This is the ONLY AHB-AP "code load" per session.
    algo_words = []
    pad = ALGO_THUMB + b"\x00" * ((4 - len(ALGO_THUMB) % 4) % 4)
    for i in range(0, len(pad), 4):
        algo_words.append(int.from_bytes(pad[i:i+4], "little"))
    st, _ = vcmd_mem_write(dap, algo_addr, algo_words)
    if st != 0:
        errors.append(f"  algo upload failed (status={status_name(st)}); FF-filling")
        return b"\xff" * size_bytes, errors
    LOG.info("algo uploaded to 0x%08X (%d bytes); reading via on-target Thumb code",
             algo_addr, len(ALGO_THUMB))

    cur_addr   = addr & 0xFFFFFFFC
    words_left = word_count

    while words_left > 0:
        elapsed = time.monotonic() - start
        if elapsed > time_budget_sec:
            errors.append(f"  time budget {time_budget_sec:.1f}s exhausted; FF-filling rest")
            out += b"\xff" * (words_left * 4)
            break

        n = min(batch_words, words_left)

        # 2) Set up CPU registers for this batch.
        reg_setup = [
            ("R0",   REG_R0,   cur_addr),
            ("R1",   REG_R1,   n),
            ("R2",   REG_R2,   buffer_addr),
            ("MSP",  REG_MSP,  stack_top),
            ("PC",   REG_PC,   algo_addr | 1),    # T bit on PC
            ("XPSR", REG_XPSR, 0x01000000),
        ]
        fail_reason = None
        for name, reg, val in reg_setup:
            ok, reason = write_cpu_reg(dap, reg, val)
            if not ok:
                fail_reason = f"{name}<=0x{val:08X}: {reason}"
                break
        if fail_reason:
            errors.append(f"  CPU register setup failed at 0x{cur_addr:08X} ({fail_reason}); FF-filling batch")
            out += b"\xff" * (n * 4)
            cur_addr   += n * 4
            words_left -= n
            continue

        # 3) Run the algo (clear C_HALT) — CPU executes our Thumb code.
        if not cpu_run(dap):
            errors.append(f"  cpu_run failed at 0x{cur_addr:08X}; FF-filling batch")
            out += b"\xff" * (n * 4)
            cur_addr   += n * 4
            words_left -= n
            continue

        # 4) Wait for the algo to hit its BKPT (halts the CPU).
        if not cpu_wait_halt(dap, timeout_ms=500):
            errors.append(f"  algo timeout at 0x{cur_addr:08X}; halting + FF-filling batch")
            cpu_halt(dap)
            out += b"\xff" * (n * 4)
            cur_addr   += n * 4
            words_left -= n
            continue

        # 5) Read the result buffer from RAM. ONE AHB-AP block read per batch.
        #    We use the existing vcmd_mem_read which caps at 64 words per call,
        #    so for n>64 we loop. Even so, this is RAM (not flash) so it's
        #    free of the chip's flash anti-tamper.
        captured = []
        sub_addr = buffer_addr
        sub_left = n
        while sub_left > 0:
            chunk = min(64, sub_left)
            st_r, words = vcmd_mem_read(dap, sub_addr, chunk)
            if st_r != 0 or not words:
                # RAM read failed — partial batch, FF-fill rest of batch.
                break
            captured.extend(words)
            sub_addr += len(words) * 4
            sub_left -= len(words)

        if len(captured) == n:
            for w in captured:
                out += w.to_bytes(4, "little")
        else:
            # Got partial; pad with FF for the unreadable tail.
            for w in captured:
                out += w.to_bytes(4, "little")
            out += b"\xff" * ((n - len(captured)) * 4)
            errors.append(f"  partial batch at 0x{cur_addr:08X}: got {len(captured)}/{n} words from RAM")

        cur_addr   += n * 4
        words_left -= n

        if on_progress is not None:
            done_bytes = len(out)
            el = time.monotonic() - start
            on_progress(done_bytes, size_bytes, done_bytes / el if el > 0 else 0)

        if json_progress:
            el = time.monotonic() - start
            evt = {
                "type": "progress",
                "chunk": (word_count - words_left + batch_words - 1) // batch_words,
                "total_chunks": (word_count + batch_words - 1) // batch_words,
                "addr": cur_addr,
                "real_bytes": sum(1 for b in out if b != 0xFF),
                "total_bytes": size_bytes,
                "elapsed_ms": int(el * 1000),
                "budget_ms": int(time_budget_sec * 1000),
            }
            import sys as _sys
            print("__PROG__ " + json.dumps(evt), file=_sys.stderr, flush=True)

    return bytes(out[:size_bytes]), errors


def bulk_read(dap, addr: int, size_bytes: int, on_progress=None,
              chunk_words: int = CHUNK_WORDS_DEFAULT,
              rehandshake_fn=None,
              random_walk: bool = False,
              shuffle_seed: int = 0xDA9,
              time_budget_sec: float = 30.0,
              json_progress: bool = False) -> tuple[bytes, list[str]]:
    """Read `size_bytes` from `addr` by iterating DAP_VENDOR 0x86.

    Adaptive retry strategy bounded by `time_budget_sec`. nrfjprog/J-Link
    grind through transient FAULTs over ~7s for 1 MB on a healthy chip;
    we match that with a similar global cap, falling to FF-fill once the
    budget is spent. This guarantees the operation completes in bounded
    time regardless of chip state.

      Tier 1 (always): 2 light retries — no chip reset, just SWJ resend.
                       ~2 ms each. Most successful chunks finish here.
      Tier 2 (adaptive): smaller-burst fallback (16 → 4 words). Only
                         attempted if we have ≥50% time budget remaining.
      Tier 3 (rare): CTRL-AP soft-reset rehandshake. Only runs every
                     RECOVERY_INTERVAL chunks and only if time is left.

    On consecutive failures (>MAX_TOTAL_DEAD_CHUNKS in a row) we
    short-circuit and FF-fill the rest — chip is locked, no use grinding.

    `json_progress`: emit one-line JSON progress events on stderr each
    chunk for the webdbg SSE bridge to consume.
    """
    if size_bytes <= 0:
        return b"", []
    word_count = (size_bytes + 3) // 4
    errors: list[str] = []
    addr_base = addr & 0xFFFFFFFC
    start = time.monotonic()

    # Build a list of (chunk_index, chunk_addr, chunk_size_words) so we
    # can address chunks by index, fill them out-of-order, then assemble.
    plan = []
    words_done = 0
    while words_done < word_count:
        n = min(chunk_words, word_count - words_done)
        plan.append((len(plan), addr_base + words_done * 4, n))
        words_done += n
    out = [b'\xff' * (sz * 4) for _, _, sz in plan]  # default FF-fill

    # Visit order: linear by default, shuffled if random_walk.
    visit_order = list(range(len(plan)))
    if random_walk and len(visit_order) > 4:
        # Deterministic shuffle so repeated runs are reproducible.
        import random as _rnd
        rng = _rnd.Random(shuffle_seed)
        rng.shuffle(visit_order)

    # Time-bounded retry knobs. Defaults targeted at ≤30s for 1 MB
    # (J-Link reference is ~7s; we allow ~4× headroom).
    LIGHT_TRIES        = 2          # plain retry, no chip reset (~2 ms each)
    FALLBACK_SIZES     = [16, 4]    # smaller bursts; ~5 ms each
    RECOVERY_INTERVAL  = 64         # only do heavy/ultra recovery every Nth chunk
    HEAVY_TRIES        = 1          # CTRL-AP soft-reset rehandshake (~250ms)
    ULTRA_TRIES        = 1          # DAP disconnect/connect (~300ms)
    MAX_TOTAL_DEAD_CHUNKS = 50      # give up sooner — chip is clearly hostile
    dead_chunks_in_a_row = 0
    budget_exhausted = False

    # Ultra-recovery: a full DAP_Disconnect/DAP_Connect cycle resets the
    # probe's DP state machine end-to-end. When sticky DP_FAILs persist
    # through CTRL-AP soft resets, this is sometimes the only thing that
    # opens a fresh transaction window. Empirically gets ~8 words per
    # cycle on heavily anti-tamper-locked Nordic chips.
    def _ultra_recover():
        try:
            dap.disconnect()
        except Exception:
            pass
        try:
            from ..probe.pydapaccess.dap_access_api import DAPAccessIntf as _DAI
            dap.connect(_DAI.PORT.SWD)
            dap.swj_sequence(51, 0xffffffffffffff)
            dap.swj_sequence(16, 0xe79e)
            dap.swj_sequence(51, 0xffffffffffffff)
            dap.swj_sequence(8,  0x00)
            return rehandshake_fn() if rehandshake_fn is not None else True
        except Exception:
            return False

    def _attempt(target_addr, target_count):
        """One read attempt. Returns (status, words_list)."""
        try:
            return vcmd_mem_read(dap, target_addr, target_count)
        except Exception:
            return (0xFE, [])

    def _light_recover():
        try:
            dap.swj_sequence(51, 0xffffffffffffff)
            dap.swj_sequence(16, 0xe79e)
            dap.swj_sequence(51, 0xffffffffffffff)
            dap.swj_sequence(8,  0x00)
        except Exception:
            pass

    for chunk_seq, idx in enumerate(visit_order):
        elapsed = time.monotonic() - start
        if elapsed > time_budget_sec:
            budget_exhausted = True
            errors.append(f"  time budget {time_budget_sec:.1f}s exhausted at chunk {chunk_seq}/{len(visit_order)} — FF-filling rest")
            break

        _, chunk_addr, chunk_size = plan[idx]
        captured_words = []
        cursor_addr  = chunk_addr
        words_left   = chunk_size

        # === Tier 1: light retries (no chip reset) — always run ===
        for _ in range(LIGHT_TRIES):
            if words_left == 0: break
            st, words = _attempt(cursor_addr, words_left)
            if words:
                captured_words.extend(words)
                cursor_addr += len(words) * 4
                words_left  -= len(words)
                if st == 0 and words_left == 0:
                    break
            _light_recover()

        # Budget-aware: only do expensive recovery if there's ≥30% time
        # left AND we haven't given up. This keeps total runtime bounded.
        time_left = time_budget_sec - (time.monotonic() - start)
        plenty_of_time = time_left > (time_budget_sec * 0.3)
        do_heavy = plenty_of_time and (chunk_seq % RECOVERY_INTERVAL == 0)

        # === Tier 2: smaller-burst fallback ===
        if words_left > 0 and plenty_of_time:
            for burst in FALLBACK_SIZES:
                if words_left == 0: break
                n = min(burst, words_left)
                st, words = _attempt(cursor_addr, n)
                if words:
                    captured_words.extend(words)
                    cursor_addr += len(words) * 4
                    words_left  -= len(words)
                if not words:
                    _light_recover()

        # === Tier 3: heavy rehandshake (CTRL-AP soft-reset) ===
        if words_left > 0 and do_heavy and rehandshake_fn is not None:
            for _ in range(HEAVY_TRIES):
                if not rehandshake_fn(): break
                st, words = _attempt(cursor_addr, words_left)
                if words:
                    captured_words.extend(words)
                    cursor_addr += len(words) * 4
                    words_left  -= len(words)
                    if st == 0 and words_left == 0:
                        break

        # === Tier 4: ultra-recovery (DAP_Disconnect/Connect) ===
        if words_left > 0 and do_heavy:
            for _ in range(ULTRA_TRIES):
                if not _ultra_recover(): break
                st, words = _attempt(cursor_addr, words_left)
                if words:
                    captured_words.extend(words)
                    cursor_addr += len(words) * 4
                    words_left  -= len(words)
                    if st == 0 and words_left == 0:
                        break

        # Assemble into the output slot for this chunk.
        if captured_words:
            blob = b''.join(w.to_bytes(4, "little") for w in captured_words)
            # Pad with FF if we didn't get the full chunk_size words.
            blob += b'\xff' * (chunk_size * 4 - len(blob))
            out[idx] = blob
            if len(captured_words) == chunk_size:
                dead_chunks_in_a_row = 0
            else:
                # Got partial — counts as live progress, reset dead streak.
                dead_chunks_in_a_row = 0
                errors.append(
                    f"  chunk @ 0x{chunk_addr:08X}: partial ({len(captured_words)}/{chunk_size} words)"
                )
        else:
            dead_chunks_in_a_row += 1
            errors.append(
                f"  FF-filled chunk @ 0x{chunk_addr:08X} (count={chunk_size}); "
                f"dead-streak={dead_chunks_in_a_row}/{MAX_TOTAL_DEAD_CHUNKS}"
            )
            if dead_chunks_in_a_row >= MAX_TOTAL_DEAD_CHUNKS:
                errors.append("  too many consecutive dead chunks — chip unrecoverable, stopping")
                break

        if on_progress is not None:
            done_bytes = sum(len(b.rstrip(b'\xff')) for b in out)  # rough live bytes
            elapsed = time.monotonic() - start
            rate = done_bytes / elapsed if elapsed > 0 else 0
            on_progress(done_bytes, size_bytes, rate)

        if json_progress:
            elapsed = time.monotonic() - start
            real_so_far = sum(1 for b in b''.join(out) if b != 0xFF)
            evt = {
                "type": "progress",
                "chunk": chunk_seq + 1,
                "total_chunks": len(visit_order),
                "addr": chunk_addr,
                "real_bytes": real_so_far,
                "total_bytes": size_bytes,
                "elapsed_ms": int(elapsed * 1000),
                "budget_ms": int(time_budget_sec * 1000),
            }
            import sys as _sys
            print("__PROG__ " + json.dumps(evt), file=_sys.stderr, flush=True)

    if budget_exhausted:
        # Emit one final progress event noting truncation.
        if json_progress:
            evt = {
                "type": "budget_exhausted",
                "completed_chunks": chunk_seq,
                "total_chunks": len(visit_order),
            }
            import sys as _sys
            print("__PROG__ " + json.dumps(evt), file=_sys.stderr, flush=True)

    return bytes(b''.join(out)[:size_bytes]), errors


def hex_dump(buf: bytes, base_addr: int, out=sys.stdout, width: int = 16) -> None:
    """Print a canonical hex dump (16 bytes per line) to `out`."""
    for off in range(0, len(buf), width):
        line = buf[off : off + width]
        hex_part = " ".join(f"{b:02x}" for b in line)
        ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in line)
        out.write(f"  0x{base_addr + off:08X}:  {hex_part:<48}  |{ascii_part}|\n")


def _stderr_progress(done: int, total: int, rate: float) -> None:
    pct = 100 * done / total if total > 0 else 0
    sys.stderr.write(
        f"\r  reading {done}/{total} bytes ({pct:5.1f}%, {rate / 1024:.1f} KiB/s)…"
    )
    sys.stderr.flush()


# ──────────────────────────────────────────────────────────────────
#                    `dump-mem` subcommand
# ──────────────────────────────────────────────────────────────────


class DumpMemSubcommand(SubcommandBase):
    """Bulk memory dump from any address."""
    NAMES = ["dump-mem"]
    HELP  = "Dump arbitrary memory range to stdout (hex) or file (binary)."
    DEFAULT_LOG_LEVEL = logging.INFO

    @classmethod
    def get_args(cls) -> List[argparse.ArgumentParser]:
        p = argparse.ArgumentParser(description="dump-mem", add_help=False)
        g = p.add_argument_group("dump-mem")
        g.add_argument("addr", type=parse_int_base_0,
                       help="Start address (hex / dec). 4-byte aligned recommended.")
        g.add_argument("size", type=parse_size,
                       help="Number of bytes. Accepts K/M/G suffix (e.g. 1K, 1M).")
        g.add_argument("-o", "--output", metavar="FILE",
                       help="Write raw binary to FILE instead of hex dump to stdout.")
        g.add_argument("-u", "--uid", "--probe", dest="unique_id")
        g.add_argument("-f", "--frequency", type=int, default=1_000_000)
        g.add_argument("--no-progress", action="store_true",
                       help="Suppress the progress line on stderr.")
        return [cls.CommonOptions.LOGGING, p]

    def invoke(self) -> int:
        try:
            with open_probe_swd(self._args.unique_id, self._args.frequency) as dap:
                progress = None if self._args.no_progress else _stderr_progress
                buf, errors = bulk_read(
                    dap,
                    self._args.addr & 0xFFFFFFFF,
                    self._args.size,
                    on_progress=progress,
                )
                if progress is not None:
                    sys.stderr.write("\n")

                if self._args.output:
                    Path(self._args.output).write_bytes(buf)
                    LOG.info("wrote %d bytes → %s", len(buf), self._args.output)
                else:
                    hex_dump(buf, self._args.addr & 0xFFFFFFFF)

                if errors:
                    for line in errors:
                        LOG.warning(line)
                    LOG.warning("captured %d of %d requested bytes",
                                len(buf), self._args.size)
                    return 2
                return 0
        except Exception as e:
            LOG.error("error: %s", e)
            return 1


# ──────────────────────────────────────────────────────────────────
#                    `dump-flash` subcommand
# ──────────────────────────────────────────────────────────────────


class DumpFlashSubcommand(SubcommandBase):
    """Auto-detect chip family, dump its flash (full or portion)."""
    NAMES = ["dump-flash"]
    HELP  = "Auto-detect target, dump flash (full chip or --start/--length portion)."
    DEFAULT_LOG_LEVEL = logging.INFO

    @classmethod
    def get_args(cls) -> List[argparse.ArgumentParser]:
        p = argparse.ArgumentParser(description="dump-flash", add_help=False)
        g = p.add_argument_group("dump-flash")
        g.add_argument("-o", "--output", metavar="FILE",
                       help="Write raw binary to FILE (default: hex dump to stdout).")
        g.add_argument("--region", default="flash", metavar="NAME",
                       help="Region to read: flash (default), uicr, ficr, rram, all.")
        g.add_argument("--family", metavar="NAME",
                       help="Override detected family (e.g. nRF52840, nRF5340-Net). "
                            "Skips auto-detect.")
        g.add_argument("--start", type=parse_int_base_0, default=None,
                       help="Read starting at this offset into the region (default 0).")
        g.add_argument("--length", type=parse_size, default=None,
                       help="Length in bytes (default: rest of region). K/M/G suffix OK.")
        g.add_argument("-u", "--uid", "--probe", dest="unique_id")
        g.add_argument("-f", "--frequency", type=int, default=1_000_000)
        g.add_argument("--no-progress", action="store_true")
        g.add_argument("--reset-halt", action="store_true",
                       help="Reset target and halt at vector BEFORE reading "
                            "(via DAP_VENDOR 0x83). Use when running firmware "
                            "re-locks the AHB-AP between accesses (nRF5340, "
                            "production locked targets). Destroys runtime state.")
        g.add_argument("--no-auto-reset", action="store_true",
                       help="By default, if the initial read returns 0 bytes "
                            "(chip locked debug during the planning phase), we "
                            "auto-retry with reset+halt. Pass this flag to skip "
                            "that fallback and accept the 0-byte result.")
        g.add_argument("--unlock", action="store_true",
                       help="Perform Nordic APPROTECT handshake before reading: "
                            "writes the standard key (0x50FA50FA) to CTRL-AP."
                            "APPROTECT.DISABLE on the chip's Nordic CTRL-AP slot. "
                            "Combined with the firmware's CPU-side key written at "
                            "boot via SystemInit, this opens the AHB-AP on chips "
                            "where UICR.APPROTECT is Unprotected (NCS default).")
        g.add_argument("--unlock-key", type=parse_int_base_0, default=NORDIC_UNLOCK_KEY,
                       metavar="KEY32",
                       help=f"Override handshake key (default 0x{NORDIC_UNLOCK_KEY:08X}).")
        g.add_argument("--unlock-apsel", type=int, default=None,
                       help="Override Nordic CTRL-AP slot (default: auto from family — "
                            "AP[1] nRF52, AP[2] nRF5340-App/nRF54L, AP[3] nRF5340-Net, AP[4] nRF91).")
        g.add_argument("--chunk-words", type=int, default=CHUNK_WORDS_DEFAULT,
                       metavar="N",
                       help=f"Words per vendor-cmd-0x86 call (1..{CHUNK_WORDS_DEFAULT}, "
                            f"default {CHUNK_WORDS_DEFAULT}). On chips with read-burst "
                            "throttling, smaller values may capture more data per dp_init "
                            "cycle. Try 4 or 8 if 64 stops mid-burst.")
        g.add_argument("--random-walk", action="store_true",
                       help="Visit chunks in a shuffled order to avoid cascading "
                            "lock states on per-address-protected chips. Output is "
                            "assembled in linear order; only the read sequence is "
                            "scrambled. Helps on Nordic chips that re-lock after "
                            "a few sequential bursts.")
        g.add_argument("--time-budget", type=float, default=30.0, metavar="SEC",
                       help="Hard upper bound on total bulk-read time in seconds. "
                            "Default 30 s. Reference: nrfjprog --readcode takes ~7 s "
                            "on a healthy nRF5340. Going over the budget FF-fills "
                            "remaining chunks rather than grinding indefinitely.")
        g.add_argument("--json-progress", action="store_true",
                       help="Emit one-line JSON progress events on stderr prefixed "
                            "with '__PROG__'. Consumed by the webdbg SSE bridge.")
        g.add_argument("--use-algo", action="store_true", default=True,
                       help="Read flash by running a 16-byte Thumb-2 algorithm on "
                            "the TARGET CPU instead of issuing per-word AHB-AP reads. "
                            "Bypasses Nordic anti-tamper throttling because the chip "
                            "doesn't monitor CPU-internal AHB transfers. Default ON "
                            "for nRF5340 / nRF52 / nRF54L. Requires CPU halt to work.")
        g.add_argument("--no-use-algo", dest="use_algo", action="store_false",
                       help="Disable the on-target read algorithm (force host-driven "
                            "per-word AHB-AP reads). Use when debugging the algo or "
                            "comparing the two read paths head-to-head.")
        return [cls.CommonOptions.LOGGING, p]

    def invoke(self) -> int:
        try:
            with open_probe_swd(self._args.unique_id, self._args.frequency) as dap:
                auto_reset_armed = (
                    not self._args.reset_halt
                    and not self._args.no_auto_reset
                )

                if self._args.family:
                    family = self._args.family
                    if family not in FLASH_MAP:
                        LOG.error("unknown family %r — known: %s",
                                  family, ", ".join(FLASH_MAP.keys()))
                        return 1
                    regions = FLASH_MAP[family]
                    LOG.info("using forced family: %s", family)

                    # If user wants reset+halt too, fire it separately. With
                    # --family they don't need detection, so a simple
                    # vendor-cmd 0x83 is fine here.
                    if self._args.reset_halt:
                        try:
                            resp = dap.vendor(VCMD_NRF_SYS_RESET, [1])
                            st = resp[0] if resp else 0xFF
                            if st == 0:
                                LOG.info("reset+halt OK — CPU halted at reset vector")
                            else:
                                LOG.warning("reset+halt: %s — continuing anyway",
                                            status_name(st))
                        except Exception as e:
                            LOG.warning("reset+halt error: %s — continuing", e)
                else:
                    # Auto-detect. Two paths:
                    #
                    # --reset-halt: use combo cmd 0x8A which does reset+halt
                    #   AND AP scan in ONE firmware call. Eliminates the
                    #   inter-vendor-cmd gap where chips with APPROTECT
                    #   re-latch and close the DP before we can scan.
                    #
                    # Otherwise: standard host-side AP scan via detect_chip_family.
                    if self._args.reset_halt:
                        LOG.info("invoking combo cmd 0x8A (reset+halt + detect in one call)")
                        info = detect_chip_via_combo(dap)
                    else:
                        info = detect_chip_family(dap)
                        # Auto-recovery: if detection failed, fall back to
                        # combo-based detection (which forces reset+halt).
                        if not info["detected"] and auto_reset_armed:
                            LOG.warning("chip detection failed (notes: %s)", info["notes"])
                            LOG.info("auto-retrying via combo cmd 0x8A (forces reset+halt; "
                                     "disable with --no-auto-reset)…")
                            info = detect_chip_via_combo(dap)
                            auto_reset_armed = False  # used our one shot

                    if not info["detected"]:
                        LOG.error("chip family not auto-detected — pass --family or --addr/--size via dump-mem")
                        LOG.error("notes: %s", info["notes"])
                        return 1
                    family = info["family"]
                    regions = info["flash_regions"]
                    LOG.info("detected: %s (DPIDR 0x%08X) — %s",
                             family, info["dpidr"], info["notes"])

                # Resolve the region(s) to read. Treat 'flash' and 'rram'
                # as synonyms — nRF54L uses RRAM and lists its region as
                # 'rram'; everyone else lists 'flash'. We hand-wave because
                # at the dump-API layer they're the same thing.
                REGION_ALIASES = {"flash": ("flash", "rram"), "rram": ("rram", "flash")}
                if self._args.region == "all":
                    to_read = regions
                else:
                    candidates = REGION_ALIASES.get(self._args.region, (self._args.region,))
                    matches = [r for r in regions if r[0] in candidates]
                    if not matches:
                        LOG.error("region %r not in %s — try one of: %s",
                                  self._args.region, family,
                                  ", ".join(r[0] for r in regions))
                        return 1
                    to_read = matches

                # Apply --start / --length if a single region is selected.
                if len(to_read) == 1 and (self._args.start is not None or self._args.length is not None):
                    name, base, size = to_read[0]
                    start = self._args.start or 0
                    if start >= size:
                        LOG.error("--start 0x%X is past end of region (size 0x%X)", start, size)
                        return 1
                    avail = size - start
                    length = self._args.length if self._args.length is not None else avail
                    if length > avail:
                        LOG.warning("--length 0x%X truncated to region end (0x%X)", length, avail)
                        length = avail
                    to_read = [(name, base + start, length)]

                total_bytes_planned = sum(size for _, _, size in to_read)
                LOG.info("planning to read %d region(s), total %d bytes (%.1f KiB)",
                         len(to_read), total_bytes_planned, total_bytes_planned / 1024)

                # ─── APPROTECT handshake (Nordic) ───
                # Per Nordic docs, the chip's firmware writes 0x50FA50FA to
                # CTRL-AP.APPROTECT.DISABLE (CPU-side) during SystemInit if
                # UICR.APPROTECT is Unprotected. We write the SAME key to
                # the debugger-side register; once both match, AHB-AP opens.
                # This is the missing piece that lets us read flash on
                # production Nordic chips with USE_UICR firmware config.
                if self._args.unlock:
                    apsel = self._args.unlock_apsel
                    if apsel is None:
                        apsel = NORDIC_CTRL_AP_SLOTS.get(family)
                    if apsel is None:
                        LOG.warning("--unlock: don't know Nordic CTRL-AP slot for family %r; "
                                    "pass --unlock-apsel <N> to override", family)
                    else:
                        # Write BOTH APPROTECT and SECUREAPPROTECT keys. On
                        # TrustZone-enabled cores (nRF5340 App, nRF54L), the
                        # bulk of flash sits in the secure region by default
                        # (TF-M secure firmware). Without SECUREAPPROTECT also
                        # disabled, AHB-AP reads of secure addresses FAULT.
                        LOG.info("APPROTECT handshake: writing key 0x%08X to "
                                 "AP[%d].APPROTECT.DISABLE (0x%02X)",
                                 self._args.unlock_key, apsel, CTRL_AP_APPROTECT_DISABLE)
                        try:
                            st = vcmd_ap_write(dap, apsel, CTRL_AP_APPROTECT_DISABLE,
                                              self._args.unlock_key)
                            if st == 0:
                                LOG.info("APPROTECT.DISABLE key written OK")
                            else:
                                LOG.warning("APPROTECT.DISABLE write returned %s — continuing",
                                            status_name(st))
                        except Exception as e:
                            LOG.warning("APPROTECT.DISABLE write error: %s — continuing", e)

                        # SECUREAPPROTECT — only applies to TrustZone cores.
                        # nRF5340 App + nRF54L15 have it; nRF52 and Net core don't.
                        if family in ("nRF5340-App", "nRF54L15"):
                            LOG.info("SECUREAPPROTECT handshake: writing key 0x%08X to "
                                     "AP[%d].SECUREAPPROTECT.DISABLE (0x%02X)",
                                     self._args.unlock_key, apsel,
                                     CTRL_AP_SECUREAPPROTECT_DIS)
                            try:
                                st = vcmd_ap_write(dap, apsel, CTRL_AP_SECUREAPPROTECT_DIS,
                                                  self._args.unlock_key)
                                if st == 0:
                                    LOG.info("SECUREAPPROTECT.DISABLE key written OK")
                                else:
                                    LOG.warning("SECUREAPPROTECT.DISABLE write returned %s",
                                                status_name(st))
                            except Exception as e:
                                LOG.warning("SECUREAPPROTECT.DISABLE write error: %s", e)

                        # Verify: read AHB-AP.CSW DbgStatus (bit 6) + SPIStatus (bit 23).
                        ahb_open = False
                        try:
                            st_csw, csw = vcmd_ap_read(dap, 0, 0x00)
                            if st_csw == 0:
                                dbgstatus = (csw >> 6) & 1
                                spistatus = (csw >> 23) & 1
                                ahb_open = bool(dbgstatus)
                                LOG.info("AP[0].CSW = 0x%08X  →  DbgStatus=%d, SPIStatus=%d",
                                         csw, dbgstatus, spistatus)
                                if dbgstatus and spistatus:
                                    LOG.info("→ both NON-SECURE and SECURE AHB transfers PERMITTED")
                                elif dbgstatus:
                                    LOG.info("→ NON-SECURE transfers OK, SECURE still blocked")
                                else:
                                    LOG.warning("→ AHB transfers BLOCKED — APPROTECT still in effect")
                        except Exception:
                            pass

                        # CRITICAL: Halt the CPU NOW that AHB-AP is open.
                        # Per Nordic docs (and our deep dive of the WDT spec):
                        #   "The default configuration of the WDT will pause
                        #    it, if the CPU is halted."
                        # The Ring's WDT is what clears APPROTECT.DISABLE per
                        # spec ("only reset by pin/brown-out/WDT reset"). With
                        # the CPU halted, WDT pauses → key survives → AHB-AP
                        # stays open → we can dump the whole flash in peace.
                        # This is the step nrfjprog/J-Link does that we missed.
                        if ahb_open:
                            try:
                                st_halt, _ = vcmd_mem_write(
                                    dap, ADDR_DHCSR,
                                    [DHCSR_DBGKEY | DHCSR_C_HALT | DHCSR_C_DEBUGEN],
                                )
                                if st_halt == 0:
                                    LOG.info("CPU halt write OK (DHCSR ← C_HALT+C_DEBUGEN) "
                                             "— WDT now paused, APPROTECT.DISABLE will persist")
                                    # Confirm S_HALT bit.
                                    st_r, words = vcmd_mem_read_retry(dap, ADDR_DHCSR, 1)
                                    if st_r == 0 and words and (words[0] & DHCSR_S_HALT):
                                        LOG.info("CPU confirmed HALTED (DHCSR.S_HALT=1) — "
                                                 "chip is frozen, safe to bulk-read")
                                    else:
                                        LOG.warning("CPU halt write OK but S_HALT not set "
                                                    "(DHCSR=0x%08X) — may not be effective",
                                                    words[0] if words else 0)
                                else:
                                    LOG.warning("CPU halt write returned %s — proceeding "
                                                "anyway, but WDT might re-engage protection",
                                                status_name(st_halt))
                            except Exception as e:
                                LOG.warning("CPU halt error: %s — proceeding anyway", e)

                # If --unlock is set, build a re-handshake callable so
                # bulk_read can re-write the key between chunks. This keeps
                # the chip unlocked across the natural per-chunk dp_init
                # cycle that otherwise re-engages chip-side protection.
                rehandshake_fn = None
                if self._args.unlock:
                    handshake_apsel = (self._args.unlock_apsel
                                       if self._args.unlock_apsel is not None
                                       else NORDIC_CTRL_AP_SLOTS.get(family))
                    if handshake_apsel is not None:
                        handshake_key   = self._args.unlock_key
                        handshake_is_tz = family in ("nRF5340-App", "nRF54L15")

                        def _rehandshake() -> bool:
                            """Re-handshake via CTRL-AP soft reset.

                            Per Nordic docs: CTRL-AP.APPROTECT.DISABLE keys
                            survive soft reset (only pin/brown-out/WDT reset
                            clears them). So we can soft-reset the chip via
                            CTRL-AP.RESET to give the chip's firmware a fresh
                            boot — its SystemInit() will re-write its
                            CPU-side key, our debugger-side key is still in
                            place, → handshake matches again, AHB-AP opens.

                            This is the trick that lets us read past the
                            chip's WDT-driven re-lock cycles.
                            """
                            import time as _t
                            try:
                                # Re-write keys (idempotent — they may be
                                # cleared by WDT reset; writing every time
                                # is safe + cheap).
                                st1, _ = (vcmd_ap_write(dap, handshake_apsel,
                                                       CTRL_AP_APPROTECT_DISABLE,
                                                       handshake_key), None)
                                if handshake_is_tz:
                                    vcmd_ap_write(dap, handshake_apsel,
                                                 CTRL_AP_SECUREAPPROTECT_DIS,
                                                 handshake_key)
                                # CTRL-AP.RESET soft-reset: write 1 (assert),
                                # pause, write 0 (deassert). Works even when
                                # AHB-AP is locked because CTRL-AP is always
                                # reachable. Per Nordic CTRL-AP docs §5.
                                vcmd_ap_write(dap, handshake_apsel, CTRL_AP_RESET, 1)
                                _t.sleep(0.05)
                                vcmd_ap_write(dap, handshake_apsel, CTRL_AP_RESET, 0)
                                # Give the chip's firmware MORE time to boot
                                # + SystemInit() + write its CPU-side key.
                                # nRF5340 boot ROM + clock-up takes ~150-200ms.
                                _t.sleep(0.20)
                                return True
                            except Exception as e:
                                LOG.debug("rehandshake exception: %s", e)
                                return False
                        rehandshake_fn = _rehandshake

                progress = None if self._args.no_progress else _stderr_progress
                total_failed = 0
                all_blobs = []   # [(name, base, bytes, errors)]
                # auto_reset_armed inherited from the detection-step retry
                # logic above — if we already used our reset shot there, we
                # won't double-reset for the read step.

                for region_idx, (name, base, size) in enumerate(to_read):
                    LOG.info("reading region %r @ 0x%08X size 0x%X (%d bytes)…",
                             name, base, size, size)
                    use_algo = self._args.use_algo and (family in (
                        "nRF5340-App", "nRF5340-Net", "nRF52840", "nRF52833",
                        "nRF52832", "nRF54L15",
                    ))
                    if use_algo:
                        # Pick RAM layout per core. Net core has smaller RAM
                        # so we use the dedicated NET_CORE_* constants.
                        if family == "nRF5340-Net":
                            algo_addr   = NET_CORE_ALGO_ADDR
                            buffer_addr = NET_CORE_BUFFER_ADDR
                            stack_top   = NET_CORE_STACK_TOP
                            batch_words = NET_CORE_BUFFER_WORDS_MAX
                        else:
                            algo_addr   = DEFAULT_ALGO_ADDR
                            buffer_addr = DEFAULT_BUFFER_ADDR
                            stack_top   = DEFAULT_STACK_TOP
                            batch_words = DEFAULT_BUFFER_WORDS_MAX
                        LOG.info("using on-target Thumb-2 read algo "
                                 "(RAM 0x%08X, buf 0x%08X, batch %d words)",
                                 algo_addr, buffer_addr, batch_words)
                        buf, errors = algo_bulk_read(
                            dap, base, size,
                            algo_addr=algo_addr, buffer_addr=buffer_addr,
                            stack_top=stack_top, batch_words=batch_words,
                            on_progress=progress,
                            json_progress=self._args.json_progress,
                            time_budget_sec=self._args.time_budget,
                        )
                    else:
                        buf, errors = bulk_read(
                            dap, base, size, on_progress=progress,
                            chunk_words=self._args.chunk_words,
                            rehandshake_fn=rehandshake_fn,
                            random_walk=self._args.random_walk,
                            time_budget_sec=self._args.time_budget,
                            json_progress=self._args.json_progress,
                        )
                    if progress is not None:
                        sys.stderr.write("\n")

                    # Auto-recovery: if the FIRST region reads 0 bytes and the
                    # user didn't explicitly request reset+halt, fall back to
                    # reset+halt and retry. Locked chips (production nRF5340)
                    # lock debug between detection and bulk read; reset+halt
                    # freezes the CPU at vector before its firmware re-locks.
                    if (region_idx == 0 and len(buf) == 0 and auto_reset_armed):
                        LOG.warning("initial read got 0 bytes — chip likely "
                                    "locked debug between detect and read.")
                        LOG.info("auto-retrying with reset+halt "
                                 "(disable with --no-auto-reset)…")
                        try:
                            resp = dap.vendor(VCMD_NRF_SYS_RESET, [1])  # halt=1
                            st = resp[0] if resp else 0xFF
                            if st == 0:
                                LOG.info("reset+halt OK — re-reading region")
                                buf, errors = bulk_read(dap, base, size,
                                                       on_progress=progress,
                                                       chunk_words=self._args.chunk_words)
                                if progress is not None:
                                    sys.stderr.write("\n")
                            else:
                                LOG.warning("reset+halt: %s — keeping 0-byte result",
                                            status_name(st))
                        except Exception as e:
                            LOG.warning("auto reset+halt failed: %s — keeping result", e)
                        auto_reset_armed = False  # don't retry again per-region

                    all_blobs.append((name, base, buf, errors))
                    if len(buf) < size:
                        total_failed += size - len(buf)
                    for line in errors:
                        LOG.warning(line)

                if self._args.output:
                    out_path = Path(self._args.output)
                    if len(all_blobs) == 1:
                        out_path.write_bytes(all_blobs[0][2])
                        LOG.info("wrote %d bytes → %s", len(all_blobs[0][2]), out_path)
                    else:
                        # Multi-region: write one file per region, with name suffix.
                        for name, base, buf, _ in all_blobs:
                            sub = out_path.parent / f"{out_path.stem}-{name}{out_path.suffix}"
                            sub.write_bytes(buf)
                            LOG.info("wrote %d bytes → %s", len(buf), sub)
                else:
                    for name, base, buf, _ in all_blobs:
                        print(f"\n── {name} @ 0x{base:08X}  ({len(buf)} bytes) ──")
                        hex_dump(buf, base)

                if total_failed > 0:
                    total_got = sum(len(buf) for _, _, buf, _ in all_blobs)
                    if total_got == 0:
                        # Total failure — chip is in deep lock. Give the user
                        # the concrete recovery instruction.
                        LOG.error(
                            "Read 0 bytes. Chip's DP/AP is locked AND reset+halt could "
                            "not reach it. The chip is in its deepest lock state."
                        )
                        LOG.error(
                            "RECOVERY: briefly disconnect+reconnect target's 3V/VTREF wire "
                            "to power-cycle it, then immediately re-run this command. "
                            "Production Nordic chips with UICR.APPROTECT enabled cannot be "
                            "fully read without ERASEALL (destructive); the first ~200 bytes "
                            "are typically accessible in the post-boot window."
                        )
                    else:
                        LOG.warning("captured partial data — %d bytes failed (chip likely "
                                    "APPROTECT-locked or in deep sleep)", total_failed)
                    return 2
                return 0
        except Exception as e:
            LOG.error("error: %s", e)
            return 1
