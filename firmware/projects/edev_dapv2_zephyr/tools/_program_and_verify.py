#!/usr/bin/env python3
"""
_program_and_verify.py — flash a merged .hex onto a connected nRF5340
via the edev_dapv2_zephyr probe vendor commands and verify by reading
flash back + hash-comparing per segment.

Usage:
  python3 _program_and_verify.py /path/to/uh_ringpro351_vXXXXX_merged.hex
  python3 _program_and_verify.py file1.hex file2.hex   # back-to-back
"""
import argparse
import hashlib
import struct
import sys
import time

from intelhex import IntelHex

from test_nrf53_vendor_cmds import (
    Probe, fmt_status,
    CMD_RECOVER, CMD_ERASE, CMD_FLASH_WRITE_NET, CMD_FLASH_WRITE_APP,
    CMD_TARGET_INFO, CMD_READ_MEM,
    dap_connect_and_reset,
)

NVMC_APP = 0x50039000          # nRF5340 App NVMC
APP_CSW  = 0x23000002
NET_CSW  = 0x03800042

# USB-FS 64 B packet → max payload words
MAX_WRITE_APP_WORDS = 13       # 0x87 header = 11 bytes
MAX_WRITE_NET_WORDS = 14       # 0x86 header = 7 bytes
MAX_READ_WORDS      = 15       # 0x88 header = 13 bytes, response 2+4*N ≤ 64

GREEN = lambda s: f"\033[32m{s}\033[0m" if sys.stdout.isatty() else s
RED   = lambda s: f"\033[31m{s}\033[0m" if sys.stdout.isatty() else s
CYAN  = lambda s: f"\033[36m{s}\033[0m" if sys.stdout.isatty() else s
YELL  = lambda s: f"\033[33m{s}\033[0m" if sys.stdout.isatty() else s

def is_net(addr): return 0x01000000 <= addr < 0x02000000

def pad_to_words(data: bytes) -> bytes:
    """Pad to 4-byte boundary with 0xFF (erased-flash value)."""
    if len(data) % 4 == 0:
        return data
    return data + b"\xff" * (4 - (len(data) % 4))


def _usb_reset_and_reconnect(p):
    """Hard-reset the probe's USB endpoint when SWD transport wedges, then
    re-arm DAP. Returns a fresh Probe object — the caller must use it."""
    import usb.core
    import usb.util
    try:
        usb.util.release_interface(p.dev, p.itf.bInterfaceNumber)
    except Exception:
        pass
    try:
        p.dev.reset()
    except Exception:
        pass
    time.sleep(0.5)
    # Re-find + re-claim
    new_p = Probe()
    dap_connect_and_reset(new_p)
    return new_p


def _retry_on_transient(p, build_req, expect_cmd, timeout_ms, retries=8):
    """Retry the request on FAULT/PROTO/NO_ACK/WAIT — re-anchor link
    between attempts. Empirically up to ~5 retries needed on sustained
    Net flash writes; budget 8 for safety. Each retry does a line-reset
    + DPIDR via dap_connect_and_reset to clear sticky bits."""
    last_status = -1
    for attempt in range(retries):
        req = build_req()
        try:
            resp = p.transfer(req, timeout_ms=timeout_ms)
        except Exception as e:
            return False, f"USB error: {e}", 0
        if resp[0] != expect_cmd:
            return False, f"echo mismatch 0x{resp[0]:02x}", 0
        last_status = resp[1]
        if last_status == 0:  # OK
            written = struct.unpack("<H", resp[2:4])[0]
            return True, fmt_status(last_status), written
        # Transient transport errors — re-anchor and retry
        if last_status in (1, 2, 3, 4):  # WAIT / FAULT / NO_ACK / PROTO
            try:
                dap_connect_and_reset(p)
            except Exception:
                pass
            continue
        # Hard error — don't retry
        return False, fmt_status(last_status), 0
    return False, f"{fmt_status(last_status)} after {retries} attempts", 0


def write_batch_app(p, addr, words, timeout_ms=3000):
    def build():
        return (bytes([CMD_FLASH_WRITE_APP])
                + struct.pack("<II", NVMC_APP, addr)
                + struct.pack("<H", len(words))
                + b"".join(struct.pack("<I", w) for w in words))
    ok, status, written = _retry_on_transient(p, build, 0x87, timeout_ms)
    return (ok and written == len(words)), f"{written}/{len(words)} ({status})"

def write_batch_net(p, addr, words, timeout_ms=3000):
    def build():
        return (bytes([CMD_FLASH_WRITE_NET])
                + struct.pack("<I", addr)
                + struct.pack("<H", len(words))
                + b"".join(struct.pack("<I", w) for w in words))
    ok, status, written = _retry_on_transient(p, build, 0x86, timeout_ms)
    return (ok and written == len(words)), f"{written}/{len(words)} ({status})"

def read_words(p, csw, addr, n_words, timeout_ms=3000, retries=8):
    ap_index = 0 if csw == APP_CSW else 1
    req = (bytes([CMD_READ_MEM, 0x00, ap_index])
           + struct.pack("<II", csw, addr)
           + struct.pack("<H", n_words))
    last_status = -1
    for attempt in range(retries):
        resp = p.transfer(req, timeout_ms=timeout_ms)
        if resp[0] != 0x88:
            raise RuntimeError(f"READ_MEM @ 0x{addr:08x}: echo 0x{resp[0]:02x}")
        last_status = resp[1]
        if last_status == 0:
            return resp[2:2 + 4*n_words]
        if last_status in (1, 2, 3, 4):  # WAIT/FAULT/NO_ACK/PROTO
            try:
                dap_connect_and_reset(p)
            except Exception:
                pass
            continue
        break
    raise RuntimeError(f"READ_MEM @ 0x{addr:08x}: {fmt_status(last_status)} after {retries} attempts")


def write_segment(p_ref, addr, data: bytes):
    """Write a contiguous segment. p_ref is a list of [Probe] so we can
    swap it on USB reset. Returns (ok, p_ref[0])."""
    data = pad_to_words(data)
    n_words = len(data) // 4
    addr_end = addr + len(data)
    is_n = is_net(addr)
    writer = write_batch_net if is_n else write_batch_app
    chunk_words = MAX_WRITE_NET_WORDS if is_n else MAX_WRITE_APP_WORDS
    cur = addr
    usb_resets = 0
    while cur < addr_end:
        remaining_words = (addr_end - cur) // 4
        batch_words = min(chunk_words, remaining_words)
        offset = (cur - addr)
        batch = [struct.unpack_from("<I", data, offset + i*4)[0]
                 for i in range(batch_words)]
        ok, detail = writer(p_ref[0], cur, batch)
        if ok:
            cur += batch_words * 4
            continue
        # Exhausted in-call retries — USB reset doesn't help (empirically
        # makes the AHB-AP state worse), so just give up on this segment.
        print(f"    {RED('WRITE FAIL')} @ 0x{cur:08x}: {detail}")
        return False
    return True


def read_segment(p, addr, length: int) -> bytes:
    """Read a contiguous range back into bytes, chunked by MAX_READ_WORDS."""
    csw = NET_CSW if is_net(addr) else APP_CSW
    out = bytearray()
    # Round up to word boundary for the read; trim trailing pad afterwards
    needed_words = (length + 3) // 4
    cur_addr = addr
    remaining = needed_words
    while remaining > 0:
        n = min(MAX_READ_WORDS, remaining)
        chunk = read_words(p, csw, cur_addr, n)
        out.extend(chunk)
        cur_addr += n * 4
        remaining -= n
    return bytes(out[:length])


def program_and_verify(p_ref, hex_path: str) -> bool:
    p = p_ref[0]
    print()
    print(CYAN("═" * 70))
    print(CYAN(f"  {hex_path}"))
    print(CYAN("═" * 70))

    # ── Parse hex ──────────────────────────────────────────────────
    ih = IntelHex(hex_path)
    segs = list(ih.segments())
    total = sum(end - start for start, end in segs)
    print(f"  Parsed: {len(segs)} segments, {total:,} bytes")

    # ── Erase + recover (also gets us into a known good state) ────
    print(f"\n  {CYAN('1. ERASE + RECOVER')}")
    t0 = time.time()
    resp = p.transfer([CMD_ERASE], timeout_ms=20000)
    if resp[1] != 0:
        print(f"    {RED('FAIL')} ERASE: {fmt_status(resp[1])}")
        return False
    print(f"    erase  : {time.time()-t0:.2f}s  ({resp[2]} CTRL-APs)")

    t0 = time.time()
    resp = p.transfer([CMD_RECOVER], timeout_ms=30000)
    if resp[1] != 0:
        print(f"    {RED('FAIL')} RECOVER: {fmt_status(resp[1])}")
        return False
    print(f"    recover: {time.time()-t0:.2f}s  (UICRs unlocked)")

    # RECOVER does its own ERASE inside, so flash is clean afterwards.

    # ── Write each segment ─────────────────────────────────────────
    print(f"\n  {CYAN('2. WRITE ' + str(len(segs)) + ' segments')}")
    t0 = time.time()
    total_written = 0
    for i, (start, end) in enumerate(segs):
        seg_data = ih.tobinarray(start=start, end=end - 1).tobytes()
        core = "Net" if is_net(start) else "App"
        sys.stdout.write(f"    [{i+1:02d}/{len(segs)}] {core} 0x{start:08x}..0x{end:08x} "
                         f"({end-start:>7d} B) ... ")
        sys.stdout.flush()
        if not write_segment(p_ref, start, seg_data):
            return False
        p = p_ref[0]  # may have been swapped by USB reset inside write_segment
        total_written += end - start
        sys.stdout.write(f"{GREEN('ok')}\n")
    dt = time.time() - t0
    print(f"    Total: {total_written:,} bytes in {dt:.1f}s "
          f"({total_written/dt/1024:.1f} KB/s)")

    # ── Read-back + hash compare per segment ───────────────────────
    print(f"\n  {CYAN('3. READ-BACK + hash compare per segment')}")
    t0 = time.time()
    all_pass = True
    silent_writes = []   # segments where write claimed success but readback differs
    for i, (start, end) in enumerate(segs):
        expected = ih.tobinarray(start=start, end=end - 1).tobytes()
        core = "Net" if is_net(start) else "App"
        sys.stdout.write(f"    [{i+1:02d}/{len(segs)}] {core} 0x{start:08x}..0x{end:08x}  ")
        sys.stdout.flush()
        try:
            got = read_segment(p, start, len(expected))
        except RuntimeError as e:
            print(f"{RED('READ FAIL')}: {e}")
            all_pass = False
            continue
        exp_h = hashlib.sha256(expected).hexdigest()[:16]
        got_h = hashlib.sha256(got).hexdigest()[:16]
        if exp_h == got_h:
            sys.stdout.write(f"{GREEN('ok')}  sha256[:16]={exp_h}\n")
        else:
            sys.stdout.write(f"{RED('MISMATCH')}\n")
            sys.stdout.write(f"        expected sha256[:16]={exp_h}\n")
            sys.stdout.write(f"        got      sha256[:16]={got_h}\n")
            # Find first mismatch byte for diagnostics
            for j, (a, b) in enumerate(zip(expected, got)):
                if a != b:
                    sys.stdout.write(f"        first diff at offset {j}: "
                                     f"expected 0x{a:02x}, got 0x{b:02x}\n")
                    # Show 16-byte context
                    ctx_start = max(0, j - 4)
                    ctx_end = min(len(expected), j + 12)
                    exp_ctx = ' '.join(f"{b:02x}" for b in expected[ctx_start:ctx_end])
                    got_ctx = ' '.join(f"{b:02x}" for b in got[ctx_start:ctx_end])
                    sys.stdout.write(f"        expected: {exp_ctx}\n")
                    sys.stdout.write(f"        got:      {got_ctx}\n")
                    break
            silent_writes.append((start, end))
            all_pass = False
    dt = time.time() - t0
    print(f"    Read-back: {total_written:,} bytes in {dt:.1f}s "
          f"({total_written/dt/1024:.1f} KB/s)")

    # ── Whole-image hash compare ───────────────────────────────────
    print(f"\n  {CYAN('4. WHOLE-IMAGE hash compare')}")
    full_expected = b""
    full_got = b""
    for start, end in segs:
        exp = ih.tobinarray(start=start, end=end - 1).tobytes()
        full_expected += exp
        try:
            full_got += read_segment(p, start, len(exp))
        except RuntimeError as e:
            print(f"    {RED('FAIL')} read @ 0x{start:08x}: {e}")
            return False
    exp_h = hashlib.sha256(full_expected).hexdigest()
    got_h = hashlib.sha256(full_got).hexdigest()
    print(f"    expected sha256 = {exp_h}")
    print(f"    got      sha256 = {got_h}")
    if exp_h == got_h:
        print(f"    {GREEN('WHOLE-IMAGE HASH MATCH')}")
    else:
        print(f"    {RED('WHOLE-IMAGE HASH MISMATCH')}")
        if silent_writes:
            print(f"    Segments with silent write failures:")
            for s, e in silent_writes:
                print(f"      0x{s:08x}..0x{e:08x}  ({e-s} bytes)")

    return all_pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("hex_files", nargs="+", help="merged hex file(s) to program")
    args = ap.parse_args()

    p = Probe()
    try:
        dap_connect_and_reset(p)
        # Quick identify so we know what we're talking to
        resp = p.transfer([CMD_TARGET_INFO], timeout_ms=3000)
        dpidr, ap_idr, cpuid = struct.unpack("<III", resp[2:14])
        if dpidr != 0x6BA02477:
            print(f"{RED('Wrong target')}: DPIDR=0x{dpidr:08x} (expected 0x6BA02477 nRF5340)")
            sys.exit(1)
        print(f"Target: nRF5340 (DPIDR=0x{dpidr:08x} CPUID=0x{cpuid:08x})")

        p_ref = [p]
        results = []
        for hf in args.hex_files:
            # Full USB reset between files — keeps the probe USB stack
            # clean and matches what a fresh connection looks like.
            print()
            print(f"--- USB reset before next file ---")
            try:
                p_ref[0] = _usb_reset_and_reconnect(p_ref[0])
            except Exception as e:
                print(f"USB reset failed: {e} — continuing anyway")
                dap_connect_and_reset(p_ref[0])
            ok = program_and_verify(p_ref, hf)
            results.append((hf, ok))
        # rebind p for the close() in finally:
        p = p_ref[0]

        # Summary
        print()
        print(CYAN("═" * 70))
        print(CYAN("  SUMMARY"))
        print(CYAN("═" * 70))
        any_fail = False
        for hf, ok in results:
            tag = GREEN("PASS") if ok else RED("FAIL")
            print(f"  {tag}  {hf}")
            if not ok:
                any_fail = True
        if any_fail:
            sys.exit(2)
    finally:
        p.close()


if __name__ == "__main__":
    main()
