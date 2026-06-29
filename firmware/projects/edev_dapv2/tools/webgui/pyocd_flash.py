"""pyocd-based flash for nRF5340 via edev_dapv2.

Two-stage flash for nRF5340:

  App side (< 0x01000000):
    pyocd FileProgrammer in a subprocess (pyocd_flash_worker.py).
    Why subprocess: pyocd's hid library finalizer crashes at Python exit
    on macOS 26.4.1 (rdar://hid_exit-IOHIDManagerClose). Running pyocd in
    its own process means that crash can't take down the webgui.

  Net side (>= 0x01000000):
    pyocd_diag.program_net_flash — direct AP#1 + Net NVMC bridge writes
    from the host, bypassing both pyocd's Net flash algo (which requires
    Net CPU runnable — chicken-and-egg with empty Net flash) and probe-rs
    entirely. Works on locked chips because CTRL-AP#3 ERASEALL unlocks
    Net AHB-AP for the session.

probe-rs is not used for nRF5340 flash at all — its CMSIS-DAP backend
silently exits the loader without sending any flash transaction.
"""
import asyncio
import json
import logging
import os
import sys
import tempfile
from typing import AsyncIterator, Optional, Tuple

log = logging.getLogger(__name__)


def _split_hex_segments(src_path: str) -> tuple[list, list]:
    """Return (app_segments, net_segments). Each is a list of
    (start_addr, bytes). App = addr < 0x01000000, Net = addr >=.
    """
    from intelhex import IntelHex
    src = IntelHex(src_path)
    app, net = [], []
    for start, end in src.segments():
        data = bytes(src.tobinarray(start=start, end=end-1))
        (app if start < 0x01000000 else net).append((start, data))
    return app, net


def _write_app_only_hex(app_segments, dst_path: str) -> int:
    """Materialise App-side segments as a HEX file for the pyocd subprocess.
    Returns total bytes.
    """
    from intelhex import IntelHex
    out = IntelHex()
    total = 0
    for start, data in app_segments:
        for i, b in enumerate(data):
            out[start + i] = b
        total += len(data)
    out.write_hex_file(dst_path)
    return total


async def _run_pyocd_subprocess(*, chip: str, frequency_hz: int,
                                hex_path: str, serial: Optional[str],
                                phase_label_offset: int = 0
                                ) -> AsyncIterator[Tuple[str, dict]]:
    """Yield ("progress",{}) and ("done",{}) events from pyocd_flash_worker."""
    worker_path = os.path.join(os.path.dirname(__file__), 'pyocd_flash_worker.py')
    venv_python = os.path.join(os.path.dirname(__file__), '.venv', 'bin', 'python')
    py = venv_python if os.path.exists(venv_python) else sys.executable

    proc = await asyncio.create_subprocess_exec(
        py, worker_path,
        serial or '-', chip, str(frequency_hz), hex_path,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
        stdin=asyncio.subprocess.DEVNULL,
        env={**os.environ, 'PYTHONUNBUFFERED': '1'},
    )

    final_msg = None
    final_ok = False
    last_pct: dict = {}
    try:
        assert proc.stdout is not None
        while True:
            line = await proc.stdout.readline()
            if not line:
                break
            try:
                evt = json.loads(line.decode('utf-8').strip())
            except (ValueError, UnicodeDecodeError):
                continue
            kind = evt.get('event')
            if kind == 'progress':
                pct = int(evt.get('percent', 0))
                phase = evt.get('phase', 'Programming')
                if last_pct.get(phase) == pct:
                    continue
                last_pct[phase] = pct
                yield "progress", {
                    "phase": "App " + phase, "percent": pct,
                    "kib": None, "rate_kib_s": None, "eta_s": None,
                }
            elif kind == 'done':
                final_ok = bool(evt.get('ok'))
                final_msg = evt.get('message', '(no message)')
    finally:
        rc = await proc.wait()
        if not final_msg and proc.stderr is not None:
            err = await proc.stderr.read()
            final_msg = err.decode('utf-8', 'replace').strip() or f"worker exit {rc}"

    yield "done", {"ok": final_ok and rc == 0, "exit_code": rc,
                   "message": final_msg or "App flash worker exited without status"}


async def flash_file_streaming_pyocd(*, chip: str, frequency_hz: int,
                                     hex_path: str,
                                     serial: Optional[str]
                                     ) -> AsyncIterator[Tuple[str, dict]]:
    """Flash a HEX file end-to-end on nRF5340 via edev_dapv2.

    Splits the HEX into App and Net segments, programs App via pyocd
    subprocess, programs Net via direct AP#1 + NVMC writes.

    Yields ("progress",{...}) and a single final ("done",{...}) event.
    """
    is_nrf5340 = "nrf5340" in chip.lower()

    if not is_nrf5340:
        # Non-Nordic chip — single pyocd subprocess run with original hex.
        async for kind, payload in _run_pyocd_subprocess(
            chip=chip, frequency_hz=frequency_hz,
            hex_path=hex_path, serial=serial,
        ):
            yield kind, payload
        return

    # --- nRF5340: two-stage flash ----------------------------------------
    app_segs, net_segs = _split_hex_segments(hex_path)
    app_bytes = sum(len(d) for _, d in app_segs)
    net_bytes = sum(len(d) for _, d in net_segs)
    log.info("nRF5340 HEX split: App=%dB across %d segs, Net=%dB across %d segs",
             app_bytes, len(app_segs), net_bytes, len(net_segs))

    # Stage 1: App via pyocd subprocess (only if there's App content)
    app_ok = True
    app_msg = "no App content in HEX"
    if app_segs:
        with tempfile.NamedTemporaryFile(suffix=".hex", delete=False) as tf:
            app_hex_path = tf.name
        _write_app_only_hex(app_segs, app_hex_path)

        yield "progress", {
            "phase": "App Erasing", "percent": 0,
            "kib": 0.0, "rate_kib_s": None, "eta_s": None,
            "note": f"App: {app_bytes//1024} KB across {len(app_segs)} segments",
        }

        app_ok = False
        async for kind, payload in _run_pyocd_subprocess(
            chip="nrf5340_xxaa", frequency_hz=frequency_hz,
            hex_path=app_hex_path, serial=serial,
        ):
            if kind == "progress":
                yield kind, payload
            elif kind == "done":
                app_ok = payload["ok"]
                app_msg = payload["message"]

        try: os.unlink(app_hex_path)
        except Exception: pass

        if not app_ok:
            yield "done", {"ok": False, "exit_code": 1,
                           "message": f"App flash failed: {app_msg}"}
            return

    # Stage 2: Net via direct AP#1 + NVMC (only if there's Net content)
    net_ok = True
    net_msg = "no Net content in HEX"
    if net_segs:
        yield "progress", {
            "phase": "Net Erasing", "percent": 0,
            "kib": 0.0, "rate_kib_s": None, "eta_s": None,
            "note": f"Net: {net_bytes//1024} KB across {len(net_segs)} segments",
        }

        # Bridge pyocd_diag's progress callback (thread-side) into our
        # async generator via an asyncio.Queue.
        progress_q: asyncio.Queue = asyncio.Queue()
        loop = asyncio.get_running_loop()

        def _net_progress_cb(pct: int):
            # Called from pyocd_diag's worker thread. Hand the value off
            # to the asyncio loop.
            try:
                loop.call_soon_threadsafe(progress_q.put_nowait, pct)
            except RuntimeError:
                pass

        # Kick off the Net programmer as a future; drain the progress
        # queue alongside it.
        import pyocd_diag
        net_task = asyncio.create_task(pyocd_diag.program_net_flash(
            serial=serial, frequency_hz=frequency_hz,
            segments=net_segs, progress_cb=_net_progress_cb,
        ))

        last_pct = -1
        while not net_task.done():
            try:
                pct = await asyncio.wait_for(progress_q.get(), timeout=0.5)
                if pct != last_pct:
                    last_pct = pct
                    yield "progress", {
                        "phase": "Net Programming", "percent": pct,
                        "kib": None, "rate_kib_s": None, "eta_s": None,
                    }
            except asyncio.TimeoutError:
                continue

        # Drain any remaining progress events.
        while not progress_q.empty():
            try:
                pct = progress_q.get_nowait()
                if pct != last_pct:
                    last_pct = pct
                    yield "progress", {
                        "phase": "Net Programming", "percent": pct,
                        "kib": None, "rate_kib_s": None, "eta_s": None,
                    }
            except asyncio.QueueEmpty:
                break

        net_ok, net_bytes_written, net_msg = await net_task
        if not net_ok:
            yield "done", {"ok": False, "exit_code": 1,
                           "message": f"Net flash failed after {net_bytes_written} bytes: {net_msg}"}
            return
        # Keep the rich net_msg returned by pyocd_diag.program_net_flash —
        # it includes the inline verify result and UICR.APPROTECT confirmation.
        net_msg = f"{net_bytes_written} bytes written; {net_msg}"

    yield "done", {
        "ok": True, "exit_code": 0,
        "message": (f"App: {app_msg}\nNet: {net_msg}" if (app_segs and net_segs)
                    else f"App: {app_msg}" if app_segs
                    else f"Net: {net_msg}"),
    }
