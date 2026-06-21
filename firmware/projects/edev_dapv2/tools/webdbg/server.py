#!/usr/bin/env python3
# edev_dapv2 webdbg — single-page bench for chip inspection / dump / recover / reset.
# SPDX-License-Identifier: Apache-2.0
#
# Run:
#   cd edev_dapv2/tools/edevocd && .venv/bin/python ../webdbg/server.py
#   open http://127.0.0.1:8766/
#
# Endpoints:
#   GET  /                    the single-page UI
#   GET  /api/probes          list connected CMSIS-DAP probes (uses pyocd's enum)
#   GET  /api/default-probe   pick the edev_dapv2 probe (if present) for the UI default
#   POST /api/chip-info       run `edevocd info --json --reset-halt` and return parsed data
#   POST /api/read-flash      run `edevocd dump-flash --unlock` and return hex dump
#   POST /api/recover         run `edevocd nrf-recover --yes` (destructive)
#   POST /api/reset           run `edevocd nrf-reset`
#
# All compute happens through `edevocd` subprocess invocations. Listens on 127.0.0.1.

from __future__ import annotations

import asyncio
import json
import os
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Optional

from fastapi import FastAPI
from fastapi.responses import HTMLResponse, JSONResponse, Response, StreamingResponse
from pydantic import BaseModel

HERE       = Path(__file__).resolve().parent
UHOCD_ROOT = HERE.parent / "edevocd"
sys.path.insert(0, str(UHOCD_ROOT))
from pyocd.probe.pydapaccess import DAPAccess  # noqa: E402

UHOCD = str(UHOCD_ROOT / ".venv/bin/pyocd")

# edev_dapv2's USB ID.
EDEV_VID = 0x2E8A
EDEV_PID = 0x000C

# In-memory cache of the most recent dump's raw bytes per core slot.
# Single-user dev tool — no concurrency. The full-hex + download
# endpoints read from here so the SSE done payload doesn't have to ship
# 5 MB of hex over the wire.
_LAST_DUMP: dict[str, dict[str, Any]] = {
    "app": {"data": b"", "base_addr": 0, "family": ""},
    "net": {"data": b"", "base_addr": 0, "family": ""},
}


# ---------------------------------------------------------------------- #
#                              Helpers                                   #
# ---------------------------------------------------------------------- #

def _enumerate_probes() -> list[dict[str, Any]]:
    daps = DAPAccess.get_connected_devices()
    out = []
    for d in daps:
        try:
            uid = d.get_unique_id()
            vendor = d.vendor_name or ""
            product = getattr(d, "product_name", "") or ""
            is_edev_dapv2 = ("edev_dapv2" in product) or ("Edevkit" in vendor)
            out.append({
                "uid":     uid,
                "vendor":  vendor,
                "product": product,
                "name":    f"{vendor} {product}".strip(),
                "is_edev_dapv2": is_edev_dapv2,
            })
        except Exception as e:
            out.append({"uid": str(d), "vendor": "?", "product": "?",
                        "name": f"error: {e}", "is_edev_dapv2": False})
    return out


def _run_edevocd(argv: list[str], timeout: float = 60.0) -> dict[str, Any]:
    """Run edevocd, capture stdout/stderr, return a dict shaped for the UI."""
    pretty = " ".join(shlex.quote(a) for a in argv)
    t0 = time.monotonic()
    try:
        proc = subprocess.run(
            argv, capture_output=True, text=True,
            timeout=timeout, check=False,
            env={**os.environ, "PYTHONUNBUFFERED": "1"},
        )
        return {
            "command":   pretty,
            "stdout":    proc.stdout,
            "stderr":    proc.stderr,
            "exit_code": proc.returncode,
            "duration_ms": int((time.monotonic() - t0) * 1000),
            "timed_out": False,
        }
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode("utf-8", "replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        err = (e.stderr or b"").decode("utf-8", "replace") if isinstance(e.stderr, bytes) else (e.stderr or "")
        return {
            "command":   pretty,
            "stdout":    out,
            "stderr":    err + f"\n--- timed out after {timeout}s ---",
            "exit_code": 124,
            "duration_ms": int((time.monotonic() - t0) * 1000),
            "timed_out": True,
        }


# ---------------------------------------------------------------------- #
#                              HTTP API                                  #
# ---------------------------------------------------------------------- #

app = FastAPI(title="edev_dapv2 webdbg")


class ProbeReq(BaseModel):
    uid: Optional[str] = None


@app.get("/", response_class=HTMLResponse)
async def index() -> HTMLResponse:
    return HTMLResponse(
        content=(HERE / "index.html").read_text(),
        headers={"Cache-Control": "no-store, no-cache, must-revalidate"},
    )


@app.get("/api/probes")
async def list_probes() -> dict[str, Any]:
    return {"probes": _enumerate_probes()}


@app.get("/api/default-probe")
async def default_probe() -> dict[str, Any]:
    """Return the edev_dapv2 probe info if connected, otherwise the first probe."""
    probes = _enumerate_probes()
    if not probes:
        return {"probe": None}
    uh = next((p for p in probes if p["is_edev_dapv2"]), None)
    return {"probe": uh or probes[0]}


@app.post("/api/chip-info")
async def chip_info(req: ProbeReq) -> JSONResponse:
    """Run edevocd info --json and parse the structured chip data.

    Returns:
        {
          run: { command, stdout, stderr, exit_code, duration_ms },
          chip: { dp, aps, core, debug_state, features, rom_table, … } or null,
        }
    """
    if not req.uid:
        return JSONResponse(status_code=400, content={"error": "uid required"})
    argv = [UHOCD, "info", "-u", req.uid, "--reset-halt", "--json"]
    run = _run_edevocd(argv, timeout=20.0)

    # The `info` subcommand emits two things on stdout: a JSON block then
    # a human-readable text section. Extract the JSON.
    chip = None
    try:
        s = run["stdout"]
        first_brace = s.find("{")
        if first_brace >= 0:
            depth = 0
            end = -1
            for i, ch in enumerate(s[first_brace:], start=first_brace):
                if ch == "{":
                    depth += 1
                elif ch == "}":
                    depth -= 1
                    if depth == 0:
                        end = i + 1
                        break
            if end > first_brace:
                parsed = json.loads(s[first_brace:end])
                # edev_info_cmd's JSON shape is {"data": {dp, aps, core, ...}, "text": "..."}
                # Surface the "data" payload directly so the UI can lookup
                # chip.dp / chip.aps etc.
                chip = parsed.get("data", parsed)
    except Exception as e:
        run["stderr"] += f"\n[parse error: {e}]"

    return JSONResponse({"run": run, "chip": chip})


class FlashReq(BaseModel):
    uid:    Optional[str]   = None
    family: Optional[str]   = None     # e.g. "nRF5340-App"
    start:  int             = 0
    length: Optional[int]   = None     # bytes; None = full region
    unlock: bool            = True


@app.get("/api/read-flash-stream")
async def read_flash_stream(
    uid: str,
    family: Optional[str] = None,
    start: int = 0,
    length: Optional[int] = None,
    time_budget: float = 30.0,
):
    """Server-Sent Events stream: live progress + final result.

    Events emitted (each prefixed `data: ` per SSE spec):
      {type: "start", command: "..."}
      {type: "progress", chunk, total_chunks, addr, real_bytes, total_bytes,
                         elapsed_ms, budget_ms}
      {type: "budget_exhausted", completed_chunks, total_chunks}
      {type: "log", level: "info|warning|error", text}
      {type: "done", captured_bytes, real_bytes, ff_bytes, hex_dump,
                     chunks, run: {exit_code, duration_ms}}

    The browser opens an EventSource and updates the UI live.
    """
    # The flow may iterate multiple cores. For nRF5340 we dump App THEN
    # Net (auto-detected). User-explicit family overrides this and only
    # reads that one core.
    #
    # IMPORTANT: --start is an OFFSET INTO the family's flash region, not
    # an absolute address. The base address is implicit in --family
    # (nRF5340-Net's region is at 0x01000000 by definition). Passing
    # --start 0 means "read from the start of the region".
    #
    # base_addr is tracked separately for the UI/hex-dump display so the
    # panel can label addresses correctly without us having to read
    # FLASH_MAP from the host side.
    FAMILY_BASE = {
        "nRF52840":    0x00000000,
        "nRF52833":    0x00000000,
        "nRF52832":    0x00000000,
        "nRF5340-App": 0x00000000,
        "nRF5340-Net": 0x01000000,
        "nRF91xx":     0x00000000,
        "nRF54L15":    0x00000000,
    }
    cores_to_read: list[dict[str, Any]] = []
    if family is None or family == "nRF5340-App":
        cores_to_read.append({
            "slot": "app", "family": "nRF5340-App",
            "start": start, "length": length,
            "display_base": FAMILY_BASE["nRF5340-App"] + (start or 0),
        })
        if family is None:
            cores_to_read.append({
                "slot": "net", "family": "nRF5340-Net",
                "start": 0, "length": None,
                "display_base": FAMILY_BASE["nRF5340-Net"],
            })
    elif family == "nRF5340-Net":
        cores_to_read.append({
            "slot": "net", "family": "nRF5340-Net",
            "start": start or 0, "length": length,
            "display_base": FAMILY_BASE["nRF5340-Net"] + (start or 0),
        })
    else:
        cores_to_read.append({
            "slot": "app", "family": family,
            "start": start, "length": length,
            "display_base": FAMILY_BASE.get(family, 0) + (start or 0),
        })

    async def event_stream():
        # Tell the client which cores will be read so it can lay out
        # placeholder sections up front.
        yield f"data: {json.dumps({'type': 'start', 'cores': [c['slot'] for c in cores_to_read]})}\n\n"

        sections: dict[str, dict[str, Any]] = {}
        t_total = time.monotonic()

        for core in cores_to_read:
            slot = core["slot"]
            tmp_path = Path(f"/tmp/webdbg-flash-{slot}-{os.getpid()}-{int(time.time())}.bin")
            argv = [
                UHOCD, "dump-flash",
                "-u", uid,
                "--no-progress",
                "--unlock",
                "--json-progress",
                "--time-budget", str(time_budget),
                "-o", str(tmp_path),
                "--family", core["family"],
            ]
            if core.get("start") is not None:
                argv += ["--start", str(core["start"])]
            if core.get("length") is not None:
                argv += ["--length", str(core["length"])]
            pretty = " ".join(shlex.quote(a) for a in argv)

            yield f"data: {json.dumps({'type': 'section_start', 'slot': slot, 'family': core['family'], 'command': pretty})}\n\n"

            t0 = time.monotonic()
            proc = await asyncio.create_subprocess_exec(
                *argv,
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
                env={**os.environ, "PYTHONUNBUFFERED": "1"},
            )
            stderr_lines: list[str] = []
            while True:
                line = await proc.stderr.readline()
                if not line:
                    break
                txt = line.decode("utf-8", "replace").rstrip()
                stderr_lines.append(txt)
                if txt.startswith("__PROG__ "):
                    try:
                        payload = json.loads(txt[len("__PROG__ "):])
                        payload["slot"] = slot
                        yield f"data: {json.dumps(payload)}\n\n"
                    except Exception:
                        pass
                else:
                    m = re.match(r"^\d+\s+([IWE])\s+(.*)$", txt)
                    if m:
                        lvl = {"I": "info", "W": "warning", "E": "error"}[m.group(1)]
                        yield f"data: {json.dumps({'type': 'log', 'level': lvl, 'text': m.group(2), 'slot': slot})}\n\n"
            await proc.wait()

            duration_ms = int((time.monotonic() - t0) * 1000)
            captured_bytes = 0; real_bytes = 0; hex_dump = ""; chunks = []
            line_count_compact = 0; line_count_full = 0
            base_addr = core.get("display_base") or 0
            if tmp_path.exists():
                data = tmp_path.read_bytes()
                captured_bytes = len(data)
                real_bytes = sum(1 for b in data if b != 0xFF)
                hex_dump = _format_hex_dump(data, base_addr=base_addr, collapse=True)
                line_count_compact = hex_dump.count("\n") + 1
                line_count_full    = (captured_bytes + 15) // 16 + 1
                chunks   = _segment_real_chunks(data, base_addr=base_addr)
                _LAST_DUMP[slot] = {"data": data, "base_addr": base_addr, "family": core["family"]}
                try: tmp_path.unlink()
                except Exception: pass

            section_evt = {
                "type": "section_done",
                "slot": slot,
                "family": core["family"],
                "captured_bytes": captured_bytes,
                "real_bytes":     real_bytes,
                "ff_bytes":       captured_bytes - real_bytes,
                "hex_dump":       hex_dump,
                "hex_line_count_compact": line_count_compact,
                "hex_line_count_full":    line_count_full,
                "chunks":         chunks,
                "base_addr":      base_addr,
                "run": {
                    "command":     pretty,
                    "exit_code":   proc.returncode,
                    "duration_ms": duration_ms,
                    "stderr_tail": "\n".join(stderr_lines[-15:]),
                },
            }
            sections[slot] = section_evt
            yield f"data: {json.dumps(section_evt)}\n\n"

        yield f"data: {json.dumps({'type': 'done', 'total_duration_ms': int((time.monotonic() - t_total) * 1000), 'sections': list(sections.keys())})}\n\n"

    return StreamingResponse(event_stream(), media_type="text/event-stream",
                              headers={"Cache-Control": "no-cache",
                                       "X-Accel-Buffering": "no"})


@app.post("/api/read-flash")
async def read_flash(req: FlashReq) -> JSONResponse:
    """Run edevocd dump-flash. Returns hex dump + capture stats."""
    if not req.uid:
        return JSONResponse(status_code=400, content={"error": "uid required"})

    # Write the binary to a tempfile, then read + hex-format here.
    tmp_path = Path(f"/tmp/webdbg-flash-{os.getpid()}-{int(time.time())}.bin")
    argv = [
        UHOCD, "dump-flash",
        "-u", req.uid,
        "--no-progress",
        "-o", str(tmp_path),
    ]
    if req.unlock:
        argv.append("--unlock")
    if req.family:
        argv += ["--family", req.family]
    if req.start is not None:
        argv += ["--start", str(req.start)]
    if req.length is not None:
        argv += ["--length", str(req.length)]

    run = _run_edevocd(argv, timeout=300.0)

    captured_bytes = 0
    real_bytes = 0
    hex_dump = ""
    chunks = []
    if tmp_path.exists():
        data = tmp_path.read_bytes()
        captured_bytes = len(data)
        real_bytes = sum(1 for b in data if b != 0xFF)
        hex_dump = _format_hex_dump(data, base_addr=req.start)
        chunks   = _segment_real_chunks(data, base_addr=req.start)
        try:
            tmp_path.unlink()
        except Exception:
            pass

    return JSONResponse({
        "run": run,
        "captured_bytes": captured_bytes,
        "real_bytes": real_bytes,
        "ff_bytes": captured_bytes - real_bytes,
        "hex_dump": hex_dump,
        "chunks":   chunks,
    })


def _format_hex_dump(data: bytes, base_addr: int = 0, width: int = 16,
                     collapse: bool = True) -> str:
    """Canonical hex dump (`hexdump -C` style).

    When `collapse=True` (default), consecutive identical lines are
    replaced with `*` (matches `hexdump -C`, `objdump`, etc). When False,
    every line is emitted — large dumps will be huge (1 MB ≈ 65,536 lines).
    """
    lines = []
    prev_line_hex = None
    star_active = False
    for off in range(0, len(data), width):
        chunk = data[off : off + width]
        hex_part = " ".join(f"{b:02x}" for b in chunk)
        ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        line_key = hex_part
        addr = base_addr + off
        if collapse and line_key == prev_line_hex:
            if not star_active:
                lines.append("*")
                star_active = True
            continue
        star_active = False
        lines.append(f"{addr:08x}  {hex_part:<48}  |{ascii_part}|")
        prev_line_hex = line_key
    lines.append(f"{base_addr + len(data):08x}")
    return "\n".join(lines)


def _segment_real_chunks(data: bytes, base_addr: int = 0) -> list[dict[str, Any]]:
    """Return [{start, length, kind: 'data'|'ff'}] runs across the buffer."""
    if not data:
        return []
    runs = []
    cur_kind = "ff" if data[0] == 0xFF else "data"
    cur_start = 0
    for i in range(1, len(data)):
        kind = "ff" if data[i] == 0xFF else "data"
        if kind != cur_kind:
            runs.append({"start": base_addr + cur_start, "length": i - cur_start, "kind": cur_kind})
            cur_kind = kind
            cur_start = i
    runs.append({"start": base_addr + cur_start, "length": len(data) - cur_start, "kind": cur_kind})
    return runs


@app.get("/api/last-hex-full")
async def last_hex_full(slot: str = "app"):
    """Return the full uncollapsed hex dump of a cached core dump."""
    blob = _LAST_DUMP.get(slot) or {}
    data = blob.get("data") or b""
    base = blob.get("base_addr") or 0
    if not data:
        return JSONResponse({"hex_dump": "(no dump in cache)", "lines": 0})
    hex_dump = _format_hex_dump(data, base_addr=base, collapse=False)
    return JSONResponse({"hex_dump": hex_dump, "lines": hex_dump.count("\n") + 1})


@app.get("/api/download")
async def download(slot: str = "app"):
    """Stream the cached binary dump as a downloadable file."""
    blob = _LAST_DUMP.get(slot) or {}
    data = blob.get("data") or b""
    family = blob.get("family") or "unknown"
    if not data:
        return JSONResponse(status_code=404, content={"error": f"no '{slot}' dump in cache"})
    safe_family = family.replace(" ", "_").replace("/", "_")
    fname = f"flash-{safe_family}-{slot}.bin"
    return Response(
        content=data,
        media_type="application/octet-stream",
        headers={"Content-Disposition": f'attachment; filename="{fname}"',
                 "Content-Length": str(len(data))},
    )


@app.post("/api/recover")
async def recover(req: ProbeReq) -> JSONResponse:
    if not req.uid:
        return JSONResponse(status_code=400, content={"error": "uid required"})
    argv = [UHOCD, "nrf-recover", "-u", req.uid, "--both-cores", "--yes"]
    run = _run_edevocd(argv, timeout=120.0)
    return JSONResponse({"run": run})


@app.post("/api/reset")
async def reset(req: ProbeReq) -> JSONResponse:
    if not req.uid:
        return JSONResponse(status_code=400, content={"error": "uid required"})
    argv = [UHOCD, "nrf-reset", "-u", req.uid]
    run = _run_edevocd(argv, timeout=15.0)
    return JSONResponse({"run": run})


# ---------------------------------------------------------------------- #
#                                MAIN                                    #
# ---------------------------------------------------------------------- #

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(
        "server:app",
        host="127.0.0.1",
        port=8766,
        log_level="info",
        reload=True,
        reload_dirs=[str(HERE)],
    )
