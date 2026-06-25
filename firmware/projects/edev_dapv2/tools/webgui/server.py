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
