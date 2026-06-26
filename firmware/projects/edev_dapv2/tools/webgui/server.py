"""
server.py — FastAPI app for the edev_dapv2 web GUI.

Run:
    python server.py        # uvicorn at http://127.0.0.1:8766

Endpoints are documented in README.md.
"""

from __future__ import annotations

import asyncio
import hashlib
import io
import json
import os
import re
import struct
import time
import uuid
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Optional

from fastapi import Body, FastAPI, HTTPException, UploadFile, File
from fastapi.responses import FileResponse, HTMLResponse, JSONResponse, Response
from fastapi.staticfiles import StaticFiles
from sse_starlette.sse import EventSourceResponse

import identify as identify_mod
import probers
import pyocd_diag

# ── App + session state ───────────────────────────────────────────────

app = FastAPI(title="edev_dapv2 web GUI")

HERE = Path(__file__).parent
INDEX_HTML = HERE / "index.html"


@dataclass
class Session:
    chip: str = "nRF52840_xxAA"
    speed_khz: int = 8000
    serial: Optional[str] = None
    vid_pid: str = "2e8a:000c"   # the edev_dapv2 USB IDs
    core_index: int = 0           # multi-core chips: 0 = primary, 1 = secondary


session = Session()


# Active dump jobs: id → {"queue": asyncio.Queue, "data": bytearray, ...}
DUMP_JOBS: dict[str, dict] = {}

# Active flash jobs: id → {"queue": asyncio.Queue, "result": dict|None, ...}
FLASH_JOBS: dict[str, dict] = {}


async def _resolve_serial() -> Optional[str]:
    """Return session.serial, or auto-pick the first probe matching VID:PID."""
    if session.serial:
        return session.serial
    try:
        vid_hex, pid_hex = session.vid_pid.split(":")
        vid, pid = int(vid_hex, 16), int(pid_hex, 16)
    except Exception:
        return None
    for p in await probers.list_probes():
        if p.vid == vid and p.pid == pid:
            return p.serial
    return None


async def _post_op_reset(serial: Optional[str], chip: str) -> dict:
    """Post-erase / post-flash reset.

    Nordic chips get a CTRL-AP RESET pulse — that's a hardware-pin-equivalent
    reset which works even when the CPU is halted or APPROTECT is engaged,
    and reliably brings the freshly-flashed image into running state.
    SYSRESETREQ via AHB-AP (pyocd's `target.reset()`) is unreliable here
    because we open the session with `init_board=False`, which leaves
    pyocd's core abstraction unrealized.

    Non-Nordic chips fall back to SYSRESETREQ — no CTRL-AP equivalent exists.
    """
    kind = "ctrl_ap" if chip.lower().startswith("nrf") else "system"
    return await pyocd_diag.do_reset(
        serial=serial, frequency_hz=session.speed_khz * 1000, kind=kind,
    )


# ── Index page ────────────────────────────────────────────────────────

@app.get("/", response_class=HTMLResponse)
async def index():
    return INDEX_HTML.read_text()


# ── Probe enumeration ─────────────────────────────────────────────────

@app.get("/api/probes")
async def api_probes():
    probes = await probers.list_probes()
    return {"probes": [asdict(p) for p in probes]}


# ── Session: connect / disconnect ─────────────────────────────────────

@app.post("/api/connect")
async def api_connect(body: dict = Body(...)):
    session.chip = body.get("chip", session.chip)
    session.speed_khz = int(body.get("speed_khz", session.speed_khz))
    session.serial = body.get("serial") or None
    session.vid_pid = body.get("vid_pid") or session.vid_pid
    if "core_index" in body:
        session.core_index = int(body["core_index"])
    return {"ok": True, "session": asdict(session)}


@app.get("/api/session")
async def api_session():
    return asdict(session)


# ── Info ──────────────────────────────────────────────────────────────

# JEP106 designer codes (subset, same as identify module). Inlined here to
# keep this module self-contained without importing identify just for a dict.
_JEP106_INFO = {
    0x23B: "ARM Ltd",
    0x144: "Nordic Semiconductor",
    0x020: "STMicroelectronics",
    0x017: "Texas Instruments",
    0x01F: "Microchip / Atmel",
    0x015: "NXP",
    0x00E: "Freescale / NXP",
    0x493: "Raspberry Pi Ltd",
    0x0AE: "Espressif Systems",
    0x21C: "Silicon Labs",
}


@app.get("/api/info")
async def api_info():
    """Fast DP/AP snapshot via pyocd (~60 ms) — replaces the original
    probe-rs-backed implementation that took 10+ s on multi-core nRF chips
    because probe-rs walks every AP including the unresponsive Net-core MEM-AP.
    Same fields the UI panel expects; raw tree synthesized."""
    diag = await pyocd_diag.run_diag(
        serial=await _resolve_serial(),
        target="cortex_m",
        frequency_hz=session.speed_khz * 1000,
    )
    if not diag.success or diag.dpidr is None:
        return {
            "raw": diag.error or "info: pyocd returned no DPIDR",
            "debug_port": None,
            "designer": None,
            "part": None,
            "revision": None,
            "aps_found": [],
            "warnings": diag.notes,
        }

    dpidr        = diag.dpidr
    dp_version   = (dpidr >> 12) & 0xF
    dp_designer  = (dpidr >> 1)  & 0x7FF
    dp_partno    = (dpidr >> 20) & 0xFF
    dp_revision  = (dpidr >> 28) & 0xF
    designer_str = _JEP106_INFO.get(dp_designer, f"0x{dp_designer:03X}")

    aps_found = [
        f"AP#{ap.index}: {ap.kind} (IDR=0x{ap.idr:08X})"
        for ap in diag.aps if ap.idr
    ]

    raw_lines = [
        "Probing target (pyocd, fast path)",
        "---------------------------------",
        "",
        f"Debug Port: DPv{dp_version}, Designer: {designer_str}, "
        f"Part: 0x{dp_partno:02X}, Revision: 0x{dp_revision:X}",
        f"  DPIDR    = 0x{dpidr:08X}",
    ]
    if diag.dlpidr is not None:
        raw_lines.append(f"  DLPIDR   = 0x{diag.dlpidr:08X}")
    if diag.targetid is not None:
        tpartno   = (diag.targetid >> 12) & 0xFFFF
        tdesigner = (diag.targetid >> 1)  & 0x7FF
        raw_lines.append(
            f"  TARGETID = 0x{diag.targetid:08X}  "
            f"(designer 0x{tdesigner:03X}, partno 0x{tpartno:04X})"
        )
    if diag.ctrl_stat is not None:
        raw_lines.append(f"  CTRL/STAT= 0x{diag.ctrl_stat:08X}")
    raw_lines += ["", "Access Ports:"]
    for ap in diag.aps:
        if ap.idr:
            raw_lines.append(f"  AP#{ap.index}: IDR=0x{ap.idr:08X}  {ap.kind}")

    return {
        "raw": "\n".join(raw_lines),
        "debug_port": f"DPv{dp_version}",
        "designer":   designer_str,
        "part":       f"0x{dp_partno:02X}",
        "revision":   f"0x{dp_revision:X}",
        "aps_found":  aps_found,
        "warnings":   diag.notes,
    }


# ── Memory read ───────────────────────────────────────────────────────

@app.post("/api/read")
async def api_read(body: dict = Body(...)):
    addr = int(body["address"], 0) if isinstance(body["address"], str) else int(body["address"])
    count = int(body["word_count"])
    code, data, err = await probers.read_words(
        chip=session.chip,
        speed_khz=session.speed_khz,
        address=addr,
        word_count=count,
        serial=session.serial,
        vid_pid=session.vid_pid,
        core_index=session.core_index,
    )
    if code != 0 or not data:
        raise HTTPException(status_code=502, detail=err or "read failed")
    # Return both raw hex words (for display) and base64 of bytes (for download)
    import base64
    words = struct.unpack(f"<{count}I", data)
    return {
        "address": addr,
        "word_count": count,
        "words_hex": [f"{w:08x}" for w in words],
        "bytes_b64": base64.b64encode(data).decode("ascii"),
        "md5": hashlib.md5(data).hexdigest(),
    }


# ── Flash dump (SSE progress) ─────────────────────────────────────────

@app.post("/api/dump")
async def api_dump_start(body: dict = Body(...)):
    addr = int(body.get("address", 0))
    size = int(body["byte_count"])
    chunk = int(body.get("chunk_bytes", 262144))   # 256 KB — keeps probe-rs invocations cheap
    job_id = uuid.uuid4().hex[:12]
    job = {
        "queue": asyncio.Queue(),
        "data": bytearray(),
        "size": size,
        "address": addr,
        "started": time.monotonic(),
        "done": False,
        "error": None,
    }
    DUMP_JOBS[job_id] = job

    async def runner():
        try:
            async for kind, cursor, payload in probers.stream_dump(
                chip=session.chip,
                speed_khz=session.speed_khz,
                address=addr,
                byte_count=size,
                chunk_bytes=chunk,
                serial=session.serial,
                vid_pid=session.vid_pid,
                core_index=session.core_index,
            ):
                if kind == "chunk":
                    job["data"].extend(payload)
                    await job["queue"].put({
                        "type": "progress",
                        "bytes": cursor,
                        "total": size,
                    })
                elif kind == "error":
                    job["error"] = payload.decode("utf-8", "replace")
                    await job["queue"].put({"type": "error", "message": job["error"]})
                    return
                elif kind == "done":
                    job["done"] = True
                    md5 = hashlib.md5(bytes(job["data"])).hexdigest()
                    elapsed = time.monotonic() - job["started"]
                    await job["queue"].put({
                        "type": "done",
                        "bytes": cursor,
                        "md5": md5,
                        "elapsed_s": round(elapsed, 2),
                    })
        finally:
            await job["queue"].put(None)   # sentinel

    asyncio.create_task(runner())
    return {"job_id": job_id, "size": size, "chunk_bytes": chunk}


@app.get("/api/dump/{job_id}/progress")
async def api_dump_progress(job_id: str):
    job = DUMP_JOBS.get(job_id)
    if not job:
        raise HTTPException(status_code=404, detail="unknown job")

    async def gen():
        while True:
            item = await job["queue"].get()
            if item is None:
                break
            yield {"event": item["type"], "data": json.dumps(item)}

    return EventSourceResponse(gen())


@app.get("/api/dump/{job_id}/data")
async def api_dump_data(job_id: str):
    job = DUMP_JOBS.get(job_id)
    if not job:
        raise HTTPException(status_code=404, detail="unknown job")
    if not job["done"]:
        raise HTTPException(status_code=425, detail="dump still in progress")
    fname = f"dump_0x{job['address']:08x}_{len(job['data'])}.bin"
    return Response(
        content=bytes(job["data"]),
        media_type="application/octet-stream",
        headers={"content-disposition": f'attachment; filename="{fname}"'},
    )


# ── Compare against reference ─────────────────────────────────────────

def _parse_intel_hex(text: str) -> bytes:
    addr_high = 0
    data = bytearray()
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith(":"):
            continue
        try:
            count = int(line[1:3], 16)
            offset = int(line[3:7], 16)
            rtype = int(line[7:9], 16)
        except ValueError:
            continue
        abs_addr = (addr_high << 16) | offset
        if rtype == 0:
            if len(data) < abs_addr + count:
                data.extend(b"\xff" * (abs_addr + count - len(data)))
            for i in range(count):
                data[abs_addr + i] = int(line[9 + i * 2:11 + i * 2], 16)
        elif rtype == 4:
            addr_high = int(line[9:13], 16)
    return bytes(data)


@app.post("/api/compare")
async def api_compare(
    job_id: Optional[str] = None,
    reference: UploadFile = File(...),
):
    job = DUMP_JOBS.get(job_id) if job_id else None
    if not job or not job["done"]:
        raise HTTPException(status_code=404, detail="need a completed dump job_id")
    ref_bytes = await reference.read()
    if reference.filename and reference.filename.lower().endswith((".hex", ".ihex")):
        ref = _parse_intel_hex(ref_bytes.decode("ascii", "replace"))
    else:
        ref = ref_bytes

    ours = bytes(job["data"])
    common = min(len(ours), len(ref))
    diffs = []
    diff_count = 0
    for i in range(common):
        if ours[i] != ref[i]:
            diff_count += 1
            if len(diffs) < 16:
                diffs.append({
                    "offset": i,
                    "ours": ours[i],
                    "ref": ref[i],
                })

    return {
        "ours_bytes": len(ours),
        "ours_md5": hashlib.md5(ours).hexdigest(),
        "ref_bytes": len(ref),
        "ref_md5": hashlib.md5(ref).hexdigest(),
        "common_bytes": common,
        "diff_count": diff_count,
        "first_diffs": diffs,
        "match": diff_count == 0 and len(ours) == len(ref),
    }


# ── CDC log live tail (SSE) ───────────────────────────────────────────

def _find_cdc_port() -> Optional[str]:
    """Find /dev/cu.usbmodem* — the edev_dapv2 CDC log channel."""
    import glob
    candidates = sorted(glob.glob("/dev/cu.usbmodem*"))
    return candidates[0] if candidates else None


@app.get("/api/cdc/log")
async def api_cdc_log():
    port = _find_cdc_port()
    if not port:
        raise HTTPException(status_code=404, detail="no /dev/cu.usbmodem* found")

    import serial

    async def gen():
        try:
            ser = serial.Serial(port, 115200, timeout=0.5)
        except Exception as e:
            yield {"event": "error", "data": str(e)}
            return
        try:
            buf = bytearray()
            while True:
                await asyncio.sleep(0.05)
                chunk = ser.read(256)
                if not chunk:
                    continue
                buf.extend(chunk)
                while b"\n" in buf:
                    idx = buf.index(b"\n")
                    line = bytes(buf[:idx]).decode("utf-8", "replace").rstrip("\r")
                    del buf[:idx + 1]
                    yield {"event": "line", "data": line}
        finally:
            try:
                ser.close()
            except Exception:
                pass

    return EventSourceResponse(gen())


# ── pyocd diagnostics ─────────────────────────────────────────────────

@app.post("/api/diag")
async def api_diag(body: dict = Body(default={})):
    target = body.get("target", "cortex_m")
    serial = await _resolve_serial()
    result = await pyocd_diag.run_diag(
        serial=serial,
        target=target,
        frequency_hz=session.speed_khz * 1000,
    )
    # Convert dataclass + nested dataclasses to plain dicts
    d = asdict(result)
    # Format ints as hex strings for clarity
    for k in ("dpidr", "dlpidr", "targetid", "ctrl_stat",
              "ahb_ap_csw_initial", "ahb_ap_csw_after_write"):
        if d.get(k) is not None:
            d[k] = f"0x{d[k]:08x}"
    for ap in d["aps"]:
        if ap.get("idr") is not None:
            ap["idr"] = f"0x{ap['idr']:08x}"
    return d


# ── Chip identification ───────────────────────────────────────────────

@app.post("/api/identify")
async def api_identify():
    serial = await _resolve_serial()
    result = await identify_mod.identify(
        serial=serial,
        vid_pid=session.vid_pid,
        speed_khz=session.speed_khz,
    )
    d = asdict(result)
    if d.get("cpuid") is not None:
        d["cpuid"] = f"0x{d['cpuid']:08x}"
    if d.get("rom_designer_jep106") is not None:
        d["rom_designer_jep106"] = f"0x{d['rom_designer_jep106']:03x}"
    for c in d["rom_components"]:
        c["designer"] = f"0x{c['designer']:03x}"
        c["part"] = f"0x{c['part']:03x}"
        c["component_class"] = f"0x{c['component_class']:x}"
    for core in d.get("cores", []):
        core["flash_base_hex"] = f"0x{core['flash_base']:08X}"
    return d


# ── Erase / flash ─────────────────────────────────────────────────────

@app.post("/api/erase")
async def api_erase(body: dict = Body(default={})):
    """Erase all non-volatile memory.

    method:
      - "auto"     (default): try probe-rs first (fast path for unlocked
                              chips). If it fails AND the chip is Nordic,
                              fall back to CTRL-AP ERASEALL — that's the
                              only way to unlock an APPROTECT-locked chip.
      - "ctrl_ap": force CTRL-AP path (Nordic-only). Use as "Recover" — it
                   wipes flash AND clears APPROTECT in one shot.
      - "probe_rs": force probe-rs erase (single chip, no --core flag).
    """
    import time
    method = body.get("method", "auto")
    chip = body.get("chip", session.chip)
    is_nordic = chip.lower().startswith("nrf")
    fallback_used = False

    async def _post_erase_reset_msg() -> str:
        """Post-erase reset so the chip leaves halt state and runs whatever
        comes next. Returns a one-line summary for the response message."""
        s = await _resolve_serial()
        r = await _post_op_reset(s, chip)
        return f"post-erase reset: {r['message']}"

    t0 = time.monotonic()

    # Auto: probe-rs first for speed, CTRL-AP if that fails on Nordic.
    if method == "auto":
        core_index = int(body.get("core_index", session.core_index))
        code, out, err = await probers.erase(
            chip=chip, speed_khz=session.speed_khz,
            serial=session.serial, vid_pid=session.vid_pid,
            core_index=core_index,
        )
        if code == 0:
            reset_msg = await _post_erase_reset_msg()
            return {"ok": True, "method": "probe_rs",
                    "message": (out.strip() or "erase complete") + "\n" + reset_msg,
                    "elapsed_s": round(time.monotonic() - t0, 2),
                    "core_index": core_index}
        if not is_nordic:
            return {"ok": False, "method": "probe_rs",
                    "message": err.strip() or out.strip() or f"exit {code}",
                    "elapsed_s": round(time.monotonic() - t0, 2),
                    "core_index": core_index}
        # Probe-rs failed on a Nordic chip → fall through to CTRL-AP.
        method = "ctrl_ap"
        fallback_used = True

    serial = await _resolve_serial()

    if method == "ctrl_ap":
        ok, per_ap = await pyocd_diag.erase_ctrl_ap_all(
            serial=serial,
            frequency_hz=session.speed_khz * 1000,
        )
        lines = [f"CTRL-AP#{ap}: {'✓' if ok2 else '✗'} {msg}"
                 for (ap, ok2, msg) in per_ap]

        # Post-erase sequence (matches the flash path):
        #   1) Read-verify each core's flash[0] = 0xFFFFFFFF while the chip
        #      is post-erase but pre-reset. AHB-AP is open because erased
        #      flash can't have re-engaged APPROTECT.
        #   2) Issue one CTRL-AP reset to leave the chip in a clean run state.
        verify_lines = []
        if ok:
            verify_cores = []
            chip_lc = chip.lower()
            if "nrf5340" in chip_lc:
                verify_cores = [(0, 0x00000000, "App"), (1, 0x01000000, "Net")]
            else:
                verify_cores = [(0, 0x00000000, "")]

            for core_idx, addr, label in verify_cores:
                code, data, err = await probers.read_words(
                    chip=chip, speed_khz=session.speed_khz,
                    address=addr, word_count=1,
                    serial=session.serial, vid_pid=session.vid_pid,
                    core_index=core_idx,
                )
                if code != 0 or not data:
                    verify_lines.append(
                        f"verify {label or 'core'} @ 0x{addr:08X}: ✗ read failed"
                    )
                    ok = False
                else:
                    import struct
                    val = struct.unpack("<I", data)[0]
                    erased = (val == 0xFFFFFFFF)
                    verify_lines.append(
                        f"verify {label or 'core'} @ 0x{addr:08X}: "
                        f"{'✓' if erased else '✗'} 0x{val:08X}"
                        f"{'' if erased else ' (NOT erased)'}"
                    )
                    if not erased:
                        ok = False

            reset_res = await _post_op_reset(serial, chip)
            verify_lines.append(f"post-erase reset: {reset_res['message']}")

        elapsed = round(time.monotonic() - t0, 2)
        return {
            "ok": ok,
            "method": "ctrl_ap" + (" (auto-fallback)" if fallback_used else ""),
            "message": "\n".join(lines + verify_lines) if (lines or verify_lines) else "done",
            "elapsed_s": elapsed,
            "ctrl_aps": [{"ap": ap, "ok": ok2, "message": msg} for (ap, ok2, msg) in per_ap],
        }

    # method == "probe_rs"
    core_index = int(body.get("core_index", session.core_index))
    code, out, err = await probers.erase(
        chip=chip,
        speed_khz=session.speed_khz,
        serial=session.serial,
        vid_pid=session.vid_pid,
        core_index=core_index,
    )
    elapsed = round(time.monotonic() - t0, 2)
    if code != 0:
        return {"ok": False, "method": "probe_rs",
                "message": err.strip() or out.strip() or f"exit {code}",
                "elapsed_s": elapsed, "core_index": core_index}
    reset_msg = await _post_erase_reset_msg()
    return {"ok": True, "method": "probe_rs",
            "message": (out.strip() or "erase complete") + "\n" + reset_msg,
            "elapsed_s": elapsed, "core_index": core_index}


@app.post("/api/flash")
async def api_flash(
    file: UploadFile = File(...),
    core_index: int = 0,
    verify: int = 1,
    base_address: Optional[str] = None,
    chip: Optional[str] = None,
):
    """Start a flash job. Returns {job_id} immediately; client polls
    /api/flash/{job_id}/progress (SSE) for live Erasing/Programming progress,
    and reads the final verify result from the same stream.

    The file is saved to a temp path and handed to `probe-rs download` under
    a pty so its progress bars actually emit. Format is inferred from
    extension (.hex → hex, .bin → bin, .elf → elf)."""
    import tempfile, time

    fname = file.filename or "image.hex"
    ext = (fname.rsplit(".", 1)[-1] or "").lower()
    fmt_map = {"hex": "hex", "ihex": "hex", "ihx": "hex",
               "bin": "bin", "elf": "elf", "out": "elf"}
    binfmt = fmt_map.get(ext, "hex")
    base = int(base_address, 0) if base_address else None
    use_chip = chip or session.chip

    suffix = f".{ext or 'hex'}"
    with tempfile.NamedTemporaryFile(delete=False, suffix=suffix) as tf:
        contents = await file.read()
        tf.write(contents)
        tmp_path = tf.name

    job_id = uuid.uuid4().hex[:12]
    job = {
        "queue": asyncio.Queue(),
        "result": None,
        "started": time.monotonic(),
        "filename": fname,
        "bytes_received": len(contents),
        "binary_format": binfmt,
        "core_index": core_index,
    }
    FLASH_JOBS[job_id] = job

    async def runner():
        try:
            async for kind, payload in probers.flash_file_streaming(
                chip=use_chip, speed_khz=session.speed_khz,
                file_path=tmp_path, binary_format=binfmt,
                serial=session.serial, vid_pid=session.vid_pid,
                core_index=core_index, base_address=base,
                verify=bool(int(verify)),
            ):
                if kind == "progress":
                    await job["queue"].put({"type": "progress", **payload})
                elif kind == "done":
                    elapsed = round(time.monotonic() - job["started"], 2)
                    if not payload["ok"]:
                        result = {
                            "ok": False,
                            "message": payload["message"],
                            "elapsed_s": elapsed,
                            "core_index": core_index,
                            "binary_format": binfmt,
                            "filename": fname,
                            "bytes_received": len(contents),
                        }
                        job["result"] = result
                        await job["queue"].put({"type": "done", **result})
                        return

                    # Post-flash sequence:
                    #   1) Read SP+Reset vector while chip is still halted
                    #      from the flash op (AHB-AP is open, APPROTECT
                    #      can't have re-engaged yet because firmware
                    #      hasn't run).
                    #   2) CTRL-AP reset (Nordic) / SYSRESETREQ (others)
                    #      so the chip actually boots into the new image.
                    # If we read AFTER reset, an APPROTECT-on-boot
                    # firmware would lock us out before the readback.
                    base_addr = (0x01000000 if (core_index == 1 and "nrf5340" in use_chip.lower())
                                 else 0)
                    rd_code, rd_data, _ = await probers.read_words(
                        chip=use_chip, speed_khz=session.speed_khz,
                        address=base_addr, word_count=2,
                        serial=session.serial, vid_pid=session.vid_pid,
                        core_index=core_index,
                    )
                    verify_lines = []
                    if rd_code != 0 or not rd_data:
                        verify_lines.append(f"verify @ 0x{base_addr:08X}: ✗ read failed")
                        flash_ok = False
                    else:
                        sp, reset_vec = struct.unpack("<II", rd_data)
                        is_real = (sp != 0xFFFFFFFF) and (reset_vec != 0xFFFFFFFF)
                        verify_lines.append(
                            f"verify @ 0x{base_addr:08X}: {'✓' if is_real else '✗'} "
                            f"SP=0x{sp:08X}, Reset=0x{reset_vec:08X}"
                        )
                        flash_ok = is_real

                    serial = await _resolve_serial()
                    reset_res = await _post_op_reset(serial, use_chip)
                    verify_lines.append(f"post-flash reset: {reset_res['message']}")

                    result = {
                        "ok": flash_ok,
                        "message": (payload["message"] or
                                    f"flashed {fname} ({len(contents):,} B)") +
                                   "\n" + "\n".join(verify_lines),
                        "elapsed_s": elapsed,
                        "core_index": core_index,
                        "binary_format": binfmt,
                        "filename": fname,
                        "bytes_received": len(contents),
                    }
                    job["result"] = result
                    await job["queue"].put({"type": "done", **result})
                    return
        finally:
            try:
                os.unlink(tmp_path)
            except Exception:
                pass
            await job["queue"].put(None)   # sentinel

    asyncio.create_task(runner())
    return {"job_id": job_id, "filename": fname, "bytes": len(contents),
            "binary_format": binfmt, "core_index": core_index}


@app.get("/api/flash/{job_id}/progress")
async def api_flash_progress(job_id: str):
    job = FLASH_JOBS.get(job_id)
    if not job:
        raise HTTPException(status_code=404, detail="unknown job")

    async def gen():
        while True:
            item = await job["queue"].get()
            if item is None:
                break
            yield {"event": item["type"], "data": json.dumps(item)}

    return EventSourceResponse(gen())


@app.get("/api/flash/{job_id}/result")
async def api_flash_result(job_id: str):
    job = FLASH_JOBS.get(job_id)
    if not job:
        raise HTTPException(status_code=404, detail="unknown job")
    if job["result"] is None:
        raise HTTPException(status_code=425, detail="flash still in progress")
    return job["result"]


# ── Target reset ──────────────────────────────────────────────────────

@app.post("/api/reset")
async def api_reset(body: dict = Body(...)):
    kind = body.get("kind", "system")
    if kind not in ("system", "halt", "ctrl_ap"):
        raise HTTPException(status_code=400, detail=f"unknown reset kind: {kind}")
    serial = await _resolve_serial()
    result = await pyocd_diag.do_reset(
        serial=serial,
        frequency_hz=session.speed_khz * 1000,
        kind=kind,
    )
    return result


# ── Entry point ───────────────────────────────────────────────────────

if __name__ == "__main__":
    import uvicorn

    print("\n  edev_dapv2 web GUI → http://127.0.0.1:8766\n")
    uvicorn.run(
        "server:app",
        host="127.0.0.1",
        port=8766,
        reload=False,
        log_level="info",
    )
