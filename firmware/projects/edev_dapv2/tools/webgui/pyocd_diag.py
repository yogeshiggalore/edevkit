"""
pyocd_diag.py — deep DAP-transaction diagnostics via pyocd.

Where probers.py (probe-rs subprocess) is for normal operations,
this module is for "look at the wire" inspection: per-AP IDR scan,
DP CTRL/STAT snapshot, CSW write/readback verification.

pyocd's Python API is synchronous, so we run it in a thread pool.
"""

from __future__ import annotations

import asyncio
from dataclasses import dataclass, field
from typing import Optional

# pyocd's import is heavy — keep it local so the rest of the server
# starts fast even if pyocd has install issues.


@dataclass
class ApEntry:
    index: int
    idr: int                  # 0 means absent
    kind: str                 # "AHB-AP", "CTRL-AP", "JTAG-AP", or "unknown"


@dataclass
class DiagResult:
    success: bool
    error: str = ""
    dpidr: Optional[int] = None
    dlpidr: Optional[int] = None
    targetid: Optional[int] = None
    ctrl_stat: Optional[int] = None
    aps: list[ApEntry] = field(default_factory=list)
    ahb_ap_csw_initial: Optional[int] = None     # CSW before our write
    ahb_ap_csw_after_write: Optional[int] = None  # CSW after writing default
    notes: list[str] = field(default_factory=list)


def _identify_ap(idr: int) -> str:
    if idr == 0:
        return "absent"
    # Nordic CTRL-AP — designer (idr>>17 & 0x7FF) == 0x144, class 0.
    # nRF52/91 family: 0x02880000.  nRF53/54L family: 0x12880000.
    if idr in (0x02880000, 0x12880000):
        return "Nordic CTRL-AP"
    # IDR class is bits 13:16. Class 8 = MEM-AP, Class 0 = JTAG-AP.
    cls = (idr >> 13) & 0xF
    var = (idr >> 4) & 0xF
    type_ = idr & 0xF
    if cls == 8:
        if type_ == 1:
            return f"AHB-AP var{var}"
        if type_ == 2:
            return f"APB-AP var{var}"
        if type_ == 4:
            return f"AXI-AP var{var}"
        return f"MEM-AP type{type_} var{var}"
    return f"unknown (cls={cls})"


def _run_diag_sync(serial: Optional[str], target: str,
                   frequency_hz: int) -> DiagResult:
    """Synchronous body — must run in a worker thread."""
    from pyocd.core.helpers import ConnectHelper
    from pyocd.probe.pydapaccess import DAPAccess

    res = DiagResult(success=False)
    try:
        session = ConnectHelper.session_with_chosen_probe(
            unique_id=serial,
            target_override=target,
            connect_mode="attach",
            options={
                "frequency": frequency_hz,
                "dap_protocol": "swd",
                "auto_unlock": False,
            },
        )
    except Exception as e:
        res.error = f"open session: {e}"
        return res

    if not session:
        res.error = "no probe matched"
        return res

    try:
        session.open(init_board=False)
        dp = session.board.target.dp
        try:
            dp.connect()
        except Exception as e:
            res.notes.append(f"dp.connect: {e}")
        try:
            dp.power_up_debug()
        except Exception as e:
            res.notes.append(f"dp.power_up_debug: {e}")

        # DPIDR — first to learn DP version
        try:
            res.dpidr = dp.read_dp(0x0)
        except Exception as e:
            res.notes.append(f"dpidr: {e}")

        # CTRL/STAT
        try:
            res.ctrl_stat = dp.read_dp(0x4)
        except Exception as e:
            res.notes.append(f"ctrl_stat: {e}")

        # DPv1 has no DLPIDR/TARGETID — only DPv2+.
        dp_version = ((res.dpidr or 0) >> 12) & 0xF
        if dp_version >= 2:
            try:
                dp.write_dp(0x8, 0x00000003)
                res.dlpidr = dp.read_dp(0x4)
                dp.write_dp(0x8, 0x00000000)
            except Exception as e:
                res.notes.append(f"dlpidr: {e}")
            try:
                dp.write_dp(0x8, 0x00000002)
                res.targetid = dp.read_dp(0x4)
                dp.write_dp(0x8, 0x00000000)
            except Exception as e:
                res.notes.append(f"targetid: {e}")
        else:
            res.notes.append(f"DPv{dp_version}: DLPIDR/TARGETID unsupported")

        # AP scan
        for i in range(8):
            try:
                # APSEL = i, APBANKSEL = 0xF, DPBANKSEL = 0
                dp.write_dp(0x8, (i << 24) | 0xF0)
                # Read AP register at A[3:2]=11 = bank 0xF offset 0xC = IDR
                idr = dp.read_ap((i << 24) | 0xFC)
                kind = _identify_ap(idr)
                res.aps.append(ApEntry(index=i, idr=idr or 0, kind=kind))
            except Exception as e:
                res.aps.append(ApEntry(index=i, idr=0, kind=f"err: {e}"))

        # AHB-AP CSW probe: find first MEM-AP, write CSW, read back
        for ap in res.aps:
            if "AHB-AP" in ap.kind:
                try:
                    dp.write_dp(0x8, (ap.index << 24) | 0x00)
                    res.ahb_ap_csw_initial = dp.read_ap((ap.index << 24) | 0x00)
                    dp.write_ap((ap.index << 24) | 0x00, 0x23000042)
                    res.ahb_ap_csw_after_write = dp.read_ap((ap.index << 24) | 0x00)
                except Exception as e:
                    res.notes.append(f"csw probe ap#{ap.index}: {e}")
                break

        res.success = True
    except Exception as e:
        res.error = f"diag: {e}"
    finally:
        try:
            session.close()
        except Exception:
            pass

    return res


async def run_diag(*, serial: Optional[str], target: str,
                   frequency_hz: int) -> DiagResult:
    """Run the diagnostic in a thread pool — pyocd is synchronous."""
    loop = asyncio.get_running_loop()
    return await loop.run_in_executor(
        None, _run_diag_sync, serial, target, frequency_hz
    )
