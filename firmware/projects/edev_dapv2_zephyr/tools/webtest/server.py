"""
edev_dapv2_zephyr webtest — FastAPI server that exposes the probe's
vendor commands through a browser UI on http://127.0.0.1:8766.

Wraps the existing Python test primitives in test_nrf53_vendor_cmds.py
and _program_and_verify.py. Single connected probe at a time; serialized
access via a global lock since USB is exclusive.

Run:
  cd firmware/projects/edev_dapv2_zephyr/tools/webtest
  pip install fastapi uvicorn python-multipart intelhex
  python3 server.py

Then open http://127.0.0.1:8766/
"""
from __future__ import annotations

import asyncio
import hashlib
import io
import struct
import sys
import threading
import time
from pathlib import Path
from typing import AsyncGenerator, Optional

from fastapi import FastAPI, File, HTTPException, Request, UploadFile
from fastapi.responses import HTMLResponse, JSONResponse, Response, StreamingResponse
from intelhex import IntelHex
import uvicorn
import usb.core
import usb.util

# Reuse the bench-validated probe primitives.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from test_nrf53_vendor_cmds import (  # noqa: E402
    Probe, dap_connect_and_reset, fmt_status,
    CMD_RECOVER, CMD_ERASE, CMD_FLASH_WRITE_NET, CMD_FLASH_WRITE_APP,
    CMD_TARGET_INFO, CMD_READ_MEM, CMD_WRITE_MEM,
)

NVMC_APP = 0x50039000
APP_CSW  = 0x23000002
NET_CSW  = 0x03800042
MAX_WRITE_APP_WORDS = 13
MAX_WRITE_NET_WORDS = 14
MAX_READ_WORDS      = 15

app = FastAPI(title="edev_dapv2_zephyr webtest")

# ── Probe management ────────────────────────────────────────────────
_probe_lock = threading.Lock()
_probe: Optional[Probe] = None


def _get_probe() -> Probe:
    """Lazy-init probe handle. Caller MUST hold _probe_lock."""
    global _probe
    if _probe is None:
        _probe = Probe()
        dap_connect_and_reset(_probe)
    return _probe


def _release_probe() -> None:
    """Drop probe handle (forces reconnect on next call)."""
    global _probe
    if _probe is not None:
        try:
            _probe.close()
        except Exception:
            pass
        _probe = None


def _reset_usb_and_reconnect() -> Probe:
    """Force USB-level device reset + re-claim. Used to recover after
    a wedged stop mid-USB-transfer."""
    global _probe
    _release_probe()
    dev = usb.core.find(idVendor=0x2E8A)
    if dev is not None:
        try:
            dev.reset()
        except Exception:
            pass
    time.sleep(1.0)
    _probe = Probe()
    dap_connect_and_reset(_probe)
    return _probe


# ── Helpers ──────────────────────────────────────────────────────────
def _is_net(addr: int) -> bool:
    return 0x01000000 <= addr < 0x02000000


def _read_words(p: Probe, csw: int, addr: int, n: int, timeout_ms: int = 3000,
                retries: int = 4) -> bytes:
    ap = 0 if csw == APP_CSW else 1
    req = (bytes([CMD_READ_MEM, 0x00, ap])
           + struct.pack("<II", csw, addr)
           + struct.pack("<H", n))
    last = -1
    for _ in range(retries):
        resp = p.transfer(req, timeout_ms=timeout_ms)
        if resp[0] != 0x88:
            raise RuntimeError(f"READ_MEM @ 0x{addr:08x}: echo 0x{resp[0]:02x}")
        last = resp[1]
        if last == 0:
            return bytes(resp[2:2 + 4 * n])
        if last in (1, 2, 3, 4):  # WAIT/FAULT/NO_ACK/PROTO
            try: dap_connect_and_reset(p)
            except Exception: pass
            continue
        break
    raise RuntimeError(f"READ_MEM @ 0x{addr:08x}: {fmt_status(last)}")


def _write_app_batch(p: Probe, addr: int, words: list[int], retries: int = 8) -> bool:
    req = (bytes([CMD_FLASH_WRITE_APP])
           + struct.pack("<II", NVMC_APP, addr)
           + struct.pack("<H", len(words))
           + b"".join(struct.pack("<I", w) for w in words))
    for _ in range(retries):
        resp = p.transfer(req, timeout_ms=3000)
        if resp[0] == 0x87 and resp[1] == 0:
            written = struct.unpack("<H", resp[2:4])[0]
            return written == len(words)
        if resp[1] in (1, 2, 3, 4):
            try: dap_connect_and_reset(p)
            except Exception: pass
            continue
        return False
    return False


def _write_net_batch(p: Probe, addr: int, words: list[int], retries: int = 8) -> bool:
    req = (bytes([CMD_FLASH_WRITE_NET])
           + struct.pack("<I", addr)
           + struct.pack("<H", len(words))
           + b"".join(struct.pack("<I", w) for w in words))
    for _ in range(retries):
        # Net flash writes can take longer when the auto-erase fires.
        resp = p.transfer(req, timeout_ms=5000)
        if resp[0] == 0x86 and resp[1] == 0:
            written = struct.unpack("<H", resp[2:4])[0]
            return written == len(words)
        if resp[1] in (1, 2, 3, 4):
            try: dap_connect_and_reset(p)
            except Exception: pass
            continue
        return False
    return False


def _pad_to_words(data: bytes) -> bytes:
    if len(data) % 4 == 0:
        return data
    return data + b"\xff" * (4 - (len(data) % 4))


# ── API endpoints — quick ops (synchronous) ─────────────────────────
@app.get("/api/info")
def api_info() -> JSONResponse:
    """Return DPIDR + AHB-AP IDR + CPUID via vendor cmd 0x89."""
    with _probe_lock:
        try:
            p = _get_probe()
            resp = p.transfer([CMD_TARGET_INFO], timeout_ms=3000)
            if len(resp) < 14 or resp[1] != 0:
                raise HTTPException(500, f"TARGET_INFO failed: {fmt_status(resp[1])}")
            dpidr, ap_idr, cpuid = struct.unpack("<III", resp[2:14])
            version = (dpidr >> 12) & 0xF
            designer = (dpidr >> 1) & 0x7FF
            cpuid_part = (cpuid >> 4) & 0xFFF
            family = (
                "nRF5340" if (dpidr == 0x6BA02477 and cpuid_part == 0xD21)
                else "nRF52 (M4)" if cpuid_part == 0xC24
                else "unknown"
            )
            return JSONResponse({
                "dpidr": f"0x{dpidr:08x}",
                "ap0_idr": f"0x{ap_idr:08x}",
                "cpuid": f"0x{cpuid:08x}",
                "version": f"DPv{version}",
                "designer": f"0x{designer:03x}",
                "core": "Cortex-M33" if cpuid_part == 0xD21 else
                        ("Cortex-M4" if cpuid_part == 0xC24 else "Cortex-M?"),
                "family": family,
            })
        except usb.core.USBError as e:
            _release_probe()
            raise HTTPException(500, f"USB error: {e}")
        except RuntimeError as e:
            raise HTTPException(500, str(e))


@app.post("/api/erase")
def api_erase() -> JSONResponse:
    """CTRL-AP ERASEALL on every CTRL-AP found (1 on nRF52, 2 on nRF5340)."""
    with _probe_lock:
        try:
            p = _get_probe()
            t0 = time.time()
            resp = p.transfer([CMD_ERASE], timeout_ms=20000)
            dt = time.time() - t0
            if resp[1] != 0:
                raise HTTPException(500, f"ERASE failed: {fmt_status(resp[1])}")
            return JSONResponse({"ap_count": resp[2], "elapsed_s": round(dt, 2)})
        except usb.core.USBError as e:
            _release_probe()
            raise HTTPException(500, f"USB error: {e}")


@app.post("/api/recover")
def api_recover() -> JSONResponse:
    """Full RECOVER chain (nRF5340 only — ERASEALL + App UICR + Net UICR via stub)."""
    with _probe_lock:
        try:
            p = _get_probe()
            t0 = time.time()
            resp = p.transfer([CMD_RECOVER], timeout_ms=30000)
            dt = time.time() - t0
            if resp[1] != 0:
                raise HTTPException(500, f"RECOVER failed: {fmt_status(resp[1])}")
            ap_count = resp[2]
            app_app, app_sec, net_marker, net_app = struct.unpack("<IIII", resp[3:19])
            return JSONResponse({
                "ap_count": ap_count,
                "app_approtect": f"0x{app_app:08x}",
                "app_secureapprotect": f"0x{app_sec:08x}",
                "net_sram_marker": f"0x{net_marker:08x}",
                "net_approtect": f"0x{net_app:08x}",
                "elapsed_s": round(dt, 2),
            })
        except usb.core.USBError as e:
            _release_probe()
            raise HTTPException(500, f"USB error: {e}")


@app.get("/api/read")
def api_read(addr: str, length: str, fmt: str = "hex") -> Response:
    """Read `length` bytes from `addr`. Both decimal or 0x-hex accepted.
    fmt: 'hex' (default) returns JSON hexdump; 'bin' returns raw bytes."""
    try:
        addr = int(addr, 0)
        length = int(length, 0)
    except ValueError:
        raise HTTPException(400, "addr/length must be integer (decimal or 0x..)")
    if length <= 0 or length > 0x100000:
        raise HTTPException(400, "length must be 1..1048576")
    if addr & 3:
        raise HTTPException(400, "addr must be 4-byte aligned")
    if length & 3:
        length = (length + 3) & ~3   # round up to word
    csw = NET_CSW if _is_net(addr) else APP_CSW
    out = bytearray()
    with _probe_lock:
        try:
            p = _get_probe()
            cur = addr
            remaining = length // 4
            while remaining > 0:
                n = min(MAX_READ_WORDS, remaining)
                out.extend(_read_words(p, csw, cur, n))
                cur += n * 4
                remaining -= n
        except usb.core.USBError as e:
            _release_probe()
            raise HTTPException(500, f"USB error: {e}")
        except RuntimeError as e:
            raise HTTPException(500, str(e))
    data = bytes(out[:length])
    if fmt == "bin":
        return Response(data, media_type="application/octet-stream",
                        headers={"Content-Disposition":
                                 f"attachment; filename=read_{addr:08x}_{length}.bin"})
    # hex JSON
    lines = []
    for off in range(0, len(data), 16):
        line = data[off:off + 16]
        hexstr = " ".join(f"{b:02x}" for b in line)
        asc = "".join(chr(b) if 32 <= b < 127 else "." for b in line)
        lines.append(f"0x{addr + off:08x}: {hexstr:<48s} |{asc}|")
    return JSONResponse({
        "addr": f"0x{addr:08x}",
        "length": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "hexdump": lines,
    })


@app.post("/api/usb_reset")
def api_usb_reset() -> JSONResponse:
    """Hard-reset the probe USB and re-claim. Use when the probe seems wedged."""
    with _probe_lock:
        try:
            _reset_usb_and_reconnect()
            return JSONResponse({"status": "ok"})
        except Exception as e:
            raise HTTPException(500, f"USB reset failed: {e}")


# ── API: flash + verify (streamed via SSE) ──────────────────────────
async def _flash_verify_stream(hex_bytes: bytes,
                               filename: str) -> AsyncGenerator[str, None]:
    """Async generator yielding SSE 'data:' lines while flashing + verifying."""
    def sse(event: str, payload: str) -> str:
        # Each SSE event = "event: NAME\ndata: PAYLOAD\n\n"
        return f"event: {event}\ndata: {payload}\n\n"

    # Parse hex
    try:
        ih = IntelHex(io.StringIO(hex_bytes.decode("utf-8")))
    except Exception as e:
        yield sse("log", f"❌ hex parse failed: {e}")
        yield sse("done", "FAIL")
        return

    segs = list(ih.segments())
    total = sum(end - start for start, end in segs)
    yield sse("log", f"📄 {filename}")
    yield sse("log", f"📊 {len(segs)} segments, {total:,} bytes")

    # Acquire probe in a thread (USB calls are blocking)
    loop = asyncio.get_event_loop()

    def _stage(fn, *args):
        with _probe_lock:
            return fn(*args)

    # Stage 1 — ERASE
    yield sse("log", "🧹 ERASE…")
    t0 = time.time()
    try:
        resp = await loop.run_in_executor(None, _stage,
                                          lambda: _get_probe().transfer([CMD_ERASE], timeout_ms=20000))
        if resp[1] != 0:
            yield sse("log", f"❌ ERASE: {fmt_status(resp[1])}")
            yield sse("done", "FAIL")
            return
        yield sse("log", f"   ✓ {resp[2]} CTRL-APs in {time.time() - t0:.2f}s")
    except Exception as e:
        yield sse("log", f"❌ ERASE exception: {e}")
        yield sse("done", "FAIL")
        return

    # Stage 2 — RECOVER (nRF5340 only — auto-detect; if nRF52 returns FAULT skip)
    yield sse("log", "🔓 RECOVER…")
    t0 = time.time()
    try:
        resp = await loop.run_in_executor(None, _stage,
                                          lambda: _get_probe().transfer([CMD_RECOVER], timeout_ms=30000))
        if resp[1] != 0:
            yield sse("log", f"⚠ RECOVER returned {fmt_status(resp[1])} — continuing")
        else:
            yield sse("log", f"   ✓ UICRs unlocked in {time.time() - t0:.2f}s")
    except Exception as e:
        yield sse("log", f"⚠ RECOVER exception: {e} — continuing")

    # Stage 3 — write each segment
    yield sse("log", "✍️  WRITE segments…")
    t_write = time.time()
    total_written = 0
    for i, (start, end) in enumerate(segs):
        seg_data = _pad_to_words(
            ih.tobinarray(start=start, end=end - 1).tobytes())
        core = "Net" if _is_net(start) else "App"
        chunk_words = MAX_WRITE_NET_WORDS if _is_net(start) else MAX_WRITE_APP_WORDS
        writer = _write_net_batch if _is_net(start) else _write_app_batch
        n_words = len(seg_data) // 4
        cur = start
        ok = True
        while cur < start + n_words * 4:
            remaining = (start + n_words * 4 - cur) // 4
            batch_n = min(chunk_words, remaining)
            offset = cur - start
            batch = [struct.unpack_from("<I", seg_data, offset + j * 4)[0]
                     for j in range(batch_n)]
            try:
                ok = await loop.run_in_executor(
                    None, _stage,
                    lambda c=cur, b=batch, w=writer: w(_get_probe(), c, b))
            except Exception as e:
                yield sse("log", f"   ❌ write @ 0x{cur:08x}: {e}")
                ok = False
                break
            if not ok:
                yield sse("log", f"   ❌ batch failed @ 0x{cur:08x}")
                break
            cur += batch_n * 4
        if not ok:
            yield sse("done", "FAIL")
            return
        total_written += end - start
        yield sse("log",
                  f"   [{i + 1:02d}/{len(segs)}] {core} 0x{start:08x}..0x{end:08x} "
                  f"({end - start:,} B) ✓")

    write_secs = time.time() - t_write
    yield sse("log",
              f"   total {total_written:,} B in {write_secs:.1f}s "
              f"({total_written / write_secs / 1024:.1f} KB/s)")

    # Stage 4 — read back + per-segment hash compare
    yield sse("log", "🔍 VERIFY (read-back + hash compare)…")
    t_read = time.time()
    all_match = True
    for i, (start, end) in enumerate(segs):
        expected = ih.tobinarray(start=start, end=end - 1).tobytes()
        n_words_full = (len(expected) + 3) // 4
        csw = NET_CSW if _is_net(start) else APP_CSW
        got = bytearray()
        cur = start
        remaining = n_words_full
        try:
            while remaining > 0:
                n = min(MAX_READ_WORDS, remaining)
                got.extend(await loop.run_in_executor(
                    None, _stage,
                    lambda c=cur, n=n, csw=csw: _read_words(_get_probe(), csw, c, n)))
                cur += n * 4
                remaining -= n
        except Exception as e:
            yield sse("log", f"   ❌ read @ 0x{start:08x}: {e}")
            all_match = False
            continue
        got_bytes = bytes(got[:len(expected)])
        if got_bytes == expected:
            yield sse("log",
                      f"   [{i + 1:02d}/{len(segs)}] 0x{start:08x}..0x{end:08x}  ✓ "
                      f"sha256[:16]={hashlib.sha256(expected).hexdigest()[:16]}")
        else:
            yield sse("log",
                      f"   [{i + 1:02d}/{len(segs)}] 0x{start:08x}..0x{end:08x}  ❌ "
                      f"expected {hashlib.sha256(expected).hexdigest()[:16]} got "
                      f"{hashlib.sha256(got_bytes).hexdigest()[:16]}")
            all_match = False

    read_secs = time.time() - t_read
    yield sse("log",
              f"   read-back {total_written:,} B in {read_secs:.1f}s "
              f"({total_written / read_secs / 1024:.1f} KB/s)")

    # Stage 5 — whole-image sha256
    yield sse("log", "🔐 WHOLE-IMAGE hash compare…")
    full_expected = b""
    full_got = b""
    try:
        for start, end in segs:
            exp = ih.tobinarray(start=start, end=end - 1).tobytes()
            full_expected += exp
            csw = NET_CSW if _is_net(start) else APP_CSW
            n_w = (len(exp) + 3) // 4
            cur = start
            remaining = n_w
            seg_got = bytearray()
            while remaining > 0:
                n = min(MAX_READ_WORDS, remaining)
                seg_got.extend(await loop.run_in_executor(
                    None, _stage,
                    lambda c=cur, n=n, csw=csw: _read_words(_get_probe(), csw, c, n)))
                cur += n * 4
                remaining -= n
            full_got += bytes(seg_got[:len(exp)])
    except Exception as e:
        yield sse("log", f"❌ whole-image read failed: {e}")
        yield sse("done", "FAIL")
        return

    exp_h = hashlib.sha256(full_expected).hexdigest()
    got_h = hashlib.sha256(full_got).hexdigest()
    yield sse("log", f"   expected sha256 = {exp_h}")
    yield sse("log", f"   got      sha256 = {got_h}")

    if exp_h == got_h and all_match:
        yield sse("log", "✅ WHOLE-IMAGE HASH MATCH")
        yield sse("done", "PASS")
    else:
        yield sse("log", "❌ HASH MISMATCH")
        yield sse("done", "FAIL")


@app.post("/api/flash")
async def api_flash(file: UploadFile = File(...)) -> StreamingResponse:
    """Flash + verify an Intel-hex image. Returns Server-Sent Events
    streaming progress lines as `event: log` and a final
    `event: done` carrying PASS / FAIL."""
    data = await file.read()
    return StreamingResponse(
        _flash_verify_stream(data, file.filename or "image.hex"),
        media_type="text/event-stream",
        headers={"Cache-Control": "no-cache",
                 "X-Accel-Buffering": "no"},
    )


# ── Static page ─────────────────────────────────────────────────────
INDEX_HTML = (Path(__file__).resolve().parent / "index.html").read_text()


@app.get("/", response_class=HTMLResponse)
def root() -> HTMLResponse:
    return HTMLResponse(INDEX_HTML)


if __name__ == "__main__":
    uvicorn.run("server:app", host="127.0.0.1", port=8766, reload=False)
