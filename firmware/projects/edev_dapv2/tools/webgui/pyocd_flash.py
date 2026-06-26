"""pyocd-based flash for nRF5340 via edev_dapv2.

Why this exists (Bug 2):
  probe-rs's CMSIS-DAP backend fails to actually program flash on nRF5340
  over edev_dapv2 — the loader parses the HEX and exits without sending a
  single flash transaction. The "TransferBlock USB timeout" error is
  misleading.

  pyocd does work, with two caveats:
    (a) Nordic's CMSIS pack declares IROM2 (Net flash at 0x01000000) with
        sector_size=0 → ZeroDivisionError during pyocd's sector layout.
        Fixed by pyocd_nrf5340_fix.fixup_session().
    (b) Net core flash algorithm requires Net core to be runnable. After
        CTRL-AP ERASEALL, Net is in lockup and pyocd's algo can't run.
        For now we flash App-side only; Net is a separate problem.

This module spawns pyocd_flash_worker.py as a subprocess so a known macOS
26.4.1 + libhid finalizer crash (rdar://hid_exit-IOHIDManagerClose) can't
take down the webgui process.
"""
import asyncio
import json
import logging
import os
import sys
import tempfile
from typing import AsyncIterator, Optional, Tuple

log = logging.getLogger(__name__)


def _filter_hex_app_only(src_path: str, dst_path: str) -> int:
    """Strip Net-side segments (>= 0x01000000) from a HEX file.

    Returns total bytes in the App-only output.
    """
    from intelhex import IntelHex
    src = IntelHex(src_path)
    out = IntelHex()
    total = 0
    for start, end in src.segments():
        if start < 0x01000000:
            for a in range(start, end):
                out[a] = src[a]
            total += end - start
    out.write_hex_file(dst_path)
    return total


async def flash_file_streaming_pyocd(*, chip: str, frequency_hz: int,
                                     hex_path: str,
                                     serial: Optional[str]
                                     ) -> AsyncIterator[Tuple[str, dict]]:
    """Flash a HEX file via pyocd + edev_dapv2 (subprocess).

    For nRF5340: hex is filtered to App-only (< 0x01000000) automatically;
    Net-core flashing is a separate problem.

    Yields ("progress", {...}) and ("done", {...}) events compatible with
    server.py's SSE consumer.
    """
    is_nrf5340 = "nrf5340" in chip.lower()

    if is_nrf5340:
        with tempfile.NamedTemporaryFile(suffix=".hex", delete=False) as tf:
            filtered_path = tf.name
        app_bytes = _filter_hex_app_only(hex_path, filtered_path)
        flash_path = filtered_path
        yield "progress", {
            "phase": "Erasing", "percent": 0,
            "kib": 0.0, "rate_kib_s": None, "eta_s": None,
            "note": f"nRF5340: App-only ({app_bytes//1024} KB) — Net flash skipped",
        }
    else:
        flash_path = hex_path

    worker_path = os.path.join(os.path.dirname(__file__), 'pyocd_flash_worker.py')
    venv_python = os.path.join(os.path.dirname(__file__), '.venv', 'bin', 'python')
    py = venv_python if os.path.exists(venv_python) else sys.executable

    proc = await asyncio.create_subprocess_exec(
        py, worker_path,
        serial or '-', chip, str(frequency_hz), flash_path,
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
                    "phase": phase, "percent": pct,
                    "kib": None, "rate_kib_s": None, "eta_s": None,
                }
            elif kind == 'done':
                final_ok = bool(evt.get('ok'))
                final_msg = evt.get('message', '(no message)')
    finally:
        rc = await proc.wait()
        # Drain stderr for diagnostics if the worker died unexpectedly.
        if not final_msg and proc.stderr is not None:
            err = await proc.stderr.read()
            final_msg = err.decode('utf-8', 'replace').strip() or f"worker exit {rc}"

    yield "done", {
        "ok": final_ok and rc == 0,
        "exit_code": rc,
        "message": final_msg or "flash worker exited without status",
    }
