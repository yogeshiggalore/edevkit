"""
probers.py — wrappers around the `probe-rs` CLI.

Everything here shells out to `probe-rs`. We don't reimplement DAP
logic in Python; we just parse stdout. This matches our pure-transport
firmware design: the host tool owns chip knowledge.

All functions are async because flash dumps take seconds and we want
to stream progress over SSE without blocking the FastAPI event loop.
"""

from __future__ import annotations

import asyncio
import re
import shutil
import struct
from dataclasses import dataclass, field
from typing import AsyncIterator, Optional


@dataclass
class Probe:
    identifier: str           # human label, e.g. "edev_dapv2 CMSIS-DAP"
    vid: int                  # 0x2E8A
    pid: int                  # 0x000C
    serial: str               # "EDV-2C66-..."
    kind: str                 # "CMSIS-DAP", "J-Link", etc.


@dataclass
class InfoResult:
    raw: str                  # raw stdout
    debug_port: Optional[str] = None
    designer: Optional[str] = None
    part: Optional[str] = None
    revision: Optional[str] = None
    aps_found: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)


def _probe_rs_path() -> str:
    p = shutil.which("probe-rs")
    if not p:
        raise RuntimeError(
            "probe-rs not found on PATH. Install via "
            "`brew install probe-rs/probe-rs/probe-rs` or follow probe.rs/install."
        )
    return p


async def _run(cmd: list[str], timeout: float = 60.0) -> tuple[int, str, str]:
    proc = await asyncio.create_subprocess_exec(
        *cmd,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    try:
        out, err = await asyncio.wait_for(proc.communicate(), timeout=timeout)
    except asyncio.TimeoutError:
        proc.kill()
        await proc.wait()
        return 124, "", "timeout"
    return proc.returncode or 0, out.decode("utf-8", "replace"), err.decode("utf-8", "replace")


async def list_probes() -> list[Probe]:
    """`probe-rs list` → structured records.

    probe-rs output looks like:
      [0]: edev_dapv2 CMSIS-DAP -- 2e8a:000c-0:EDV-2C66-... (CMSIS-DAP)
      [1]: J-Link Pro -- 1366:1020:000176002130 (J-Link)
    """
    code, out, err = await _run([_probe_rs_path(), "list"])
    if code != 0:
        return []
    probes: list[Probe] = []
    line_re = re.compile(
        r"^\s*\[\d+\]:\s+(?P<id>.+?)\s+--\s+"
        r"(?P<vid>[0-9a-fA-F]{4}):(?P<pid>[0-9a-fA-F]{4})"
        r"(?:-\d+)?:?(?P<serial>[^\s]*)\s+"
        r"\((?P<kind>[^)]+)\)\s*$"
    )
    for line in out.splitlines():
        m = line_re.match(line)
        if not m:
            continue
        probes.append(
            Probe(
                identifier=m["id"].strip(),
                vid=int(m["vid"], 16),
                pid=int(m["pid"], 16),
                serial=m["serial"],
                kind=m["kind"],
            )
        )
    return probes


def _probe_arg(serial: Optional[str], vid_pid: Optional[str]) -> list[str]:
    """Build the `--probe vid:pid[:serial]` argument."""
    if vid_pid:
        if serial:
            return ["--probe", f"{vid_pid}:{serial}"]
        return ["--probe", vid_pid]
    if serial:
        return ["--probe", serial]
    return []


async def info(*, chip: str, speed_khz: int,
               serial: Optional[str] = None,
               vid_pid: Optional[str] = None) -> InfoResult:
    """`probe-rs info`. Best-effort parse of the structured output."""
    cmd = [_probe_rs_path(), "info"] + _probe_arg(serial, vid_pid) + [
        "--protocol", "swd",
        "--chip", chip,
        "--speed", str(speed_khz),
    ]
    code, out, err = await _run(cmd, timeout=20.0)
    combined = (out + "\n" + err).strip()
    result = InfoResult(raw=combined)

    for line in combined.splitlines():
        s = line.strip()
        if "WARN" in s:
            result.warnings.append(s)
            continue
        m = re.search(r"Debug Port:\s*(\S+),", s)
        if m:
            result.debug_port = m.group(1)
        m = re.search(r"Designer:\s*(.+?),", s)
        if m:
            result.designer = m.group(1).strip()
        m = re.search(r"Part:\s*(\S+)", s)
        if m:
            result.part = m.group(1).strip(",")
        m = re.search(r"Revision:\s*(\S+)", s)
        if m:
            result.revision = m.group(1).strip(",")
        m = re.search(r"Found access port:\s*(.+)$", s)
        if m:
            result.aps_found.append(m.group(1).strip())

    return result


async def read_words(*, chip: str, speed_khz: int,
                     address: int, word_count: int,
                     serial: Optional[str] = None,
                     vid_pid: Optional[str] = None,
                     core_index: int = 0,
                     timeout: float = 600.0) -> tuple[int, bytes, str]:
    """`probe-rs read b32 ADDR N` → bytes (little-endian).

    Returns (exit_code, data_bytes, stderr). data_bytes is empty on
    failure. `core_index` is passed as `--core N` when non-zero (for
    multi-core chips like nRF5340: 0=App, 1=Net).
    """
    cmd = [_probe_rs_path(), "read"] + _probe_arg(serial, vid_pid) + [
        "--protocol", "swd",
        "--chip", chip,
        "--speed", str(speed_khz),
    ]
    if core_index:
        cmd += ["--core", str(core_index)]
    cmd += ["b32", f"0x{address:08x}", str(word_count)]
    code, out, err = await _run(cmd, timeout=timeout)
    if code != 0:
        return code, b"", err.strip()

    # probe-rs prints WARN lines + hex words separated by spaces.
    words: list[int] = []
    for line in out.splitlines():
        if "WARN" in line:
            continue
        for tok in line.strip().split():
            if re.fullmatch(r"[0-9a-fA-F]{8}", tok):
                words.append(int(tok, 16))
    data = b"".join(struct.pack("<I", w) for w in words)
    return code, data, err.strip()


async def erase(*, chip: str, speed_khz: int,
                serial: Optional[str] = None,
                vid_pid: Optional[str] = None,
                core_index: int = 0,    # accepted for API parity; probe-rs erase doesn't take --core
                timeout: float = 120.0) -> tuple[int, str, str]:
    """`probe-rs erase` for the given chip. Returns (exit_code, stdout, stderr).

    Note: `probe-rs erase` does NOT accept `--core` (verified against probe-rs
    0.31). The chip definition encodes both cores on dual-core parts like
    nRF5340 — a full chip erase covers all flash regions in one go. core_index
    is accepted but ignored.
    """
    cmd = [_probe_rs_path(), "erase"] + _probe_arg(serial, vid_pid) + [
        "--protocol", "swd",
        "--chip", chip,
        "--speed", str(speed_khz),
        # Required on Nordic chips where probe-rs needs to escalate to chip-
        # wide ERASEALL to unlock a protected core (nRF5340 Net core often
        # boots locked). Without this flag probe-rs refuses with "lacked
        # the permission". For the webgui Erase button the user has already
        # confirmed the destructive intent.
        "--allow-erase-all",
    ]
    code, out, err = await _run(cmd, timeout=timeout)
    return code, out, err


async def flash_file(*, chip: str, speed_khz: int,
                     file_path: str, binary_format: str,
                     serial: Optional[str] = None,
                     vid_pid: Optional[str] = None,
                     core_index: int = 0,
                     base_address: Optional[int] = None,
                     verify: bool = True,
                     timeout: float = 600.0) -> tuple[int, str, str]:
    """`probe-rs download` a file (hex / bin / elf) to the target.

    `binary_format` ∈ {"hex", "bin", "elf"}. For "bin", pass base_address.
    For "hex"/"elf", base_address is ignored (encoded in the file).
    """
    # NOTE: `probe-rs download` does NOT accept `--core` (verified). On dual-
    # core nRF5340 the chip definition covers both flash regions, so a merged
    # hex routes 0x00xxxxxx → App and 0x01xxxxxx → Net automatically. The
    # core_index param is accepted for API parity but not passed downstream.
    cmd = [_probe_rs_path(), "download"] + _probe_arg(serial, vid_pid) + [
        "--protocol", "swd",
        "--chip", chip,
        "--speed", str(speed_khz),
        "--binary-format", binary_format,
    ]
    if binary_format == "bin" and base_address is not None:
        cmd += ["--base-address", f"0x{base_address:x}"]
    if verify:
        cmd += ["--verify"]
    # Required for nRF5340 when probe-rs needs to ERASEALL to unlock a
    # protected core before programming.
    cmd += ["--allow-erase-all"]
    cmd += [file_path]
    code, out, err = await _run(cmd, timeout=timeout)
    return code, out, err


async def flash_file_streaming(*, chip: str, speed_khz: int,
                               file_path: str, binary_format: str,
                               serial: Optional[str] = None,
                               vid_pid: Optional[str] = None,
                               core_index: int = 0,
                               base_address: Optional[int] = None,
                               verify: bool = True,
                               timeout: float = 600.0):
    """Run `probe-rs download` under a pty so its progress bars emit, then
    yield parsed {phase, percent, kib, total_kib, rate_kib_s, eta_s} events.

    probe-rs only prints progress when stderr is a TTY. We give it one by
    spawning under a pty (master/slave fd pair) and reading the master.

    Yields:
      - ("progress", {phase: "Erasing"|"Programming", percent: 0..100,
                      kib: float|None, total_kib: float|None,
                      rate_kib_s: float|None, eta_s: int|None})
      - ("done", {ok: bool, exit_code: int, message: str})
    """
    import asyncio
    import os
    import pty

    # See flash_file() note above — `probe-rs download` has no `--core` flag.
    cmd = [_probe_rs_path(), "download"] + _probe_arg(serial, vid_pid) + [
        "--protocol", "swd",
        "--chip", chip,
        "--speed", str(speed_khz),
        "--binary-format", binary_format,
    ]
    if binary_format == "bin" and base_address is not None:
        cmd += ["--base-address", f"0x{base_address:x}"]
    if verify:
        cmd += ["--verify"]
    # Required for nRF5340 when probe-rs needs to ERASEALL to unlock a
    # protected core before programming.
    cmd += ["--allow-erase-all"]
    cmd += [file_path]

    master_fd, slave_fd = pty.openpty()
    proc = await asyncio.create_subprocess_exec(
        *cmd, stdout=slave_fd, stderr=slave_fd, stdin=asyncio.subprocess.DEVNULL,
        close_fds=True,
    )
    os.close(slave_fd)

    loop = asyncio.get_running_loop()

    # Regex tolerant of ANSI: capture phase + percent + (kib + unit + rate-kib + (eta_s | took_s))
    # Lines look like:
    #   Erasing\x1b[0m ⠉  47% [#########-----------] 192.00 KiB @  44.06 KiB/s (ETA 5s)
    #   Programming\x1b[0m ✔ 100% [####################] 408.00 KiB @  83.31 KiB/s (took 5s)
    line_re = re.compile(
        r"(Erasing|Programming)\x1b\[0m\s+\S+\s+(\d+)%"
        r"(?:.*?(\d+\.\d+)\s+(KiB|B)\s+@\s+(\d+\.\d+)\s+KiB/s\s+"
        r"\((?:ETA|took)\s+(\d+)s\))?"
    )

    buf = b""
    last_per_phase: dict[str, int] = {}   # phase → last emitted percent
    final_msg = ""
    ansi_re = re.compile(r"\x1b\[[\d;]*[A-Za-z]")
    try:
        while True:
            try:
                chunk = await loop.run_in_executor(
                    None, lambda: os.read(master_fd, 4096)
                )
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            # Each progress frame is delimited by either \r or \n (probe-rs
            # uses \r to redraw the same line in place).
            while True:
                # Find next CR or LF
                cr = buf.find(b"\r")
                lf = buf.find(b"\n")
                if cr == -1 and lf == -1:
                    break
                if cr == -1: i = lf
                elif lf == -1: i = cr
                else: i = min(cr, lf)
                frame = buf[:i].decode("utf-8", "replace")
                buf = buf[i+1:]

                m = line_re.search(frame)
                if m:
                    phase = m.group(1)
                    pct = int(m.group(2))
                    if last_per_phase.get(phase) == pct:
                        continue
                    last_per_phase[phase] = pct
                    kib_val = float(m.group(3)) if m.group(3) else None
                    unit = m.group(4)
                    if unit == "B" and kib_val is not None:
                        kib_val = kib_val / 1024.0
                    rate = float(m.group(5)) if m.group(5) else None
                    eta = int(m.group(6)) if m.group(6) else None
                    yield "progress", {
                        "phase": phase,
                        "percent": pct,
                        "kib": kib_val,
                        "rate_kib_s": rate,
                        "eta_s": eta,
                    }
                elif "Finished" in frame:
                    final_msg = ansi_re.sub("", frame).strip()
    finally:
        try:
            os.close(master_fd)
        except Exception:
            pass

    rc = await proc.wait()
    yield "done", {
        "ok": rc == 0,
        "exit_code": rc,
        "message": final_msg or (f"exit {rc}" if rc != 0 else "flash complete"),
    }


async def stream_dump(*, chip: str, speed_khz: int,
                      address: int, byte_count: int,
                      chunk_bytes: int = 4096,
                      serial: Optional[str] = None,
                      vid_pid: Optional[str] = None,
                      core_index: int = 0,
                      ) -> AsyncIterator[tuple[str, int, bytes]]:
    """Dump `byte_count` bytes starting at `address`, yielding progress.

    Yields tuples (kind, bytes_so_far, payload):
      - ("chunk", offset_after, chunk_bytes)  on success
      - ("error", offset_so_far, error_message_bytes)  on failure
      - ("done",  byte_count, b"")           when complete

    Caller assembles the chunks into the full dump.
    """
    word_chunk = chunk_bytes // 4
    cursor = 0
    while cursor < byte_count:
        words_left = (byte_count - cursor) // 4
        words = min(word_chunk, words_left)
        code, data, err = await read_words(
            chip=chip, speed_khz=speed_khz,
            address=address + cursor, word_count=words,
            serial=serial, vid_pid=vid_pid,
            core_index=core_index,
        )
        if code != 0 or len(data) != words * 4:
            yield "error", cursor, err.encode("utf-8", "replace")
            return
        cursor += len(data)
        yield "chunk", cursor, data
    yield "done", byte_count, b""
