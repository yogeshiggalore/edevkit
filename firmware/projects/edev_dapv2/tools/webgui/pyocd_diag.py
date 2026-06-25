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


# ── Focused DP.TARGETID read for identify-panel fallback ──────────────

def _read_dp_targetid_sync(serial, frequency_hz):
    """Open the DP, read DPIDR; if DPv2+ then also read TARGETID.
    Returns (dpidr, targetid) or (None, None) on any failure."""
    from pyocd.core.helpers import ConnectHelper

    try:
        sess = ConnectHelper.session_with_chosen_probe(
            unique_id=serial,
            target_override="cortex_m",
            connect_mode="attach",
            options={
                "frequency": frequency_hz,
                "dap_protocol": "swd",
                "auto_unlock": False,
            },
        )
    except Exception:
        return None, None
    if not sess:
        return None, None

    dpidr = targetid = None
    try:
        sess.open(init_board=False)
        dp = sess.board.target.dp
        try:
            dp.connect()
        except Exception:
            pass
        try:
            dpidr = dp.read_dp(0x0)
        except Exception:
            return None, None
        # DPIDR bits 15:12 are DP version — TARGETID exists from DPv2.
        if ((dpidr >> 12) & 0xF) >= 2:
            try:
                dp.write_dp(0x8, 0x00000002)   # DPBANKSEL = 2
                targetid = dp.read_dp(0x4)
                dp.write_dp(0x8, 0x00000000)   # restore bank 0
            except Exception:
                targetid = None
    finally:
        try:
            sess.close()
        except Exception:
            pass
    return dpidr, targetid


async def read_dp_targetid(*, serial, frequency_hz):
    """Async wrapper returning (dpidr, targetid). Either may be None."""
    loop = asyncio.get_running_loop()
    return await loop.run_in_executor(
        None, _read_dp_targetid_sync, serial, frequency_hz
    )


# ── Reset operations (pyocd-backed) ───────────────────────────────────

def _recover_dp(dp):
    """Clean up DP state after a reset so the next probe-rs invocation can
    connect cleanly. Raw-AP operations (halt-on-reset, CTRL-AP) and
    target reboot can leave: (a) sticky-error bits in CTRL/STAT,
    (b) the DP in a state probe-rs's fresh-connect logic doesn't recover.

    Sequence: clear ABORT.*CLR bits, then re-run dp.connect() so the
    DP-line-reset / DPIDR-read handshake runs again."""
    try:
        dp.write_dp(0x0, 0x0000001E)   # STKCMPCLR|STKERRCLR|WDERRCLR|ORUNERRCLR
    except Exception:
        pass
    try:
        dp.connect()
    except Exception:
        pass
    try:
        dp.power_up_debug()
    except Exception:
        pass


def _reset_sync(serial, frequency_hz, kind):
    """kind ∈ {"system", "halt", "ctrl_ap"}. Returns (ok, message)."""
    from pyocd.core.helpers import ConnectHelper

    try:
        sess = ConnectHelper.session_with_chosen_probe(
            unique_id=serial,
            target_override="cortex_m",
            connect_mode="attach",
            options={
                "frequency": frequency_hz,
                "dap_protocol": "swd",
                "auto_unlock": False,
            },
        )
    except Exception as e:
        return False, f"open session: {e}"
    if not sess:
        return False, "no probe matched"

    try:
        sess.open(init_board=False)
        target = sess.board.target
        try:
            target.dp.connect()
        except Exception:
            pass
        try:
            target.dp.power_up_debug()
        except Exception:
            pass

        if kind == "system":
            # SYSRESETREQ via AIRCR. pyocd's target.reset() handles it.
            target.reset()
            _recover_dp(target.dp)
            return True, "system reset issued (SYSRESETREQ)"

        if kind == "halt":
            # Set DEMCR.VC_CORERESET=1, then SYSRESETREQ — core stops at
            # the reset vector. We can't call target.reset_and_halt()
            # because init_board=False leaves no selected core. Do the
            # AHB-AP memory transactions directly via DP register access.
            import time
            dp = target.dp
            # Locate the first AHB-AP (class 8 MEM-AP + type 1 AHB).
            ahb_ap = None
            for i in range(8):
                try:
                    dp.write_dp(0x8, (i << 24) | 0xF0)
                    idr = dp.read_ap((i << 24) | 0xFC)
                except Exception:
                    continue
                if idr and ((idr >> 13) & 0xF) == 0x8 and (idr & 0xF) == 0x1:
                    ahb_ap = i
                    break
            if ahb_ap is None:
                return False, "no AHB-AP found for halt-on-reset"

            # AHB-AP register-level memory access:
            #   CSW @ 0x00, TAR @ 0x04, DRW @ 0x0C.
            #   CSW = 0x23000002: HPROT=0x23, AddrInc=off, Size=32-bit.
            def ap_select_bank0():
                dp.write_dp(0x8, (ahb_ap << 24) | 0x00)
            def ap_write32(addr, value):
                ap_select_bank0()
                dp.write_ap((ahb_ap << 24) | 0x00, 0x23000002)  # CSW
                dp.write_ap((ahb_ap << 24) | 0x04, addr)        # TAR
                dp.write_ap((ahb_ap << 24) | 0x0C, value)       # DRW
                dp.read_dp(0x0C)                                # RDBUFF flush
            def ap_read32(addr):
                ap_select_bank0()
                dp.write_ap((ahb_ap << 24) | 0x00, 0x23000002)
                dp.write_ap((ahb_ap << 24) | 0x04, addr)
                _ = dp.read_ap((ahb_ap << 24) | 0x0C)           # post the read
                return dp.read_dp(0x0C)                          # RDBUFF

            try:
                demcr = ap_read32(0xE000EDFC)
                ap_write32(0xE000EDFC, demcr | 0x1)              # VC_CORERESET
                ap_write32(0xE000ED0C, 0x05FA0004)               # AIRCR.SYSRESETREQ
                time.sleep(0.05)
            except Exception as e:
                return False, f"halt-reset write: {e}"
            _recover_dp(dp)
            return True, (
                f"reset issued with halt-on-reset (DEMCR.VC_CORERESET=1 + "
                f"AIRCR.SYSRESETREQ via AHB-AP#{ahb_ap})"
            )

        if kind == "ctrl_ap":
            # Nordic CTRL-AP RESET register at offset 0x000. Find the AP
            # with the right IDR and write 1, then 0, to assert+release.
            import time
            dp = target.dp
            ctrl_ap = None
            for i in range(8):
                try:
                    dp.write_dp(0x8, (i << 24) | 0xF0)
                    idr = dp.read_ap((i << 24) | 0xFC)
                except Exception:
                    continue
                if idr in (0x02880000, 0x12880000):
                    ctrl_ap = i
                    break
            if ctrl_ap is None:
                return False, "no Nordic CTRL-AP found (0x02880000 / 0x12880000)"
            try:
                dp.write_dp(0x8, (ctrl_ap << 24) | 0x00)
                dp.write_ap((ctrl_ap << 24) | 0x00, 0x00000001)  # assert
                time.sleep(0.05)
                dp.write_ap((ctrl_ap << 24) | 0x00, 0x00000000)  # release
            except Exception as e:
                return False, f"CTRL-AP write: {e}"
            _recover_dp(dp)
            return True, f"Nordic CTRL-AP reset issued (AP#{ctrl_ap})"

        return False, f"unknown reset kind: {kind}"

    except Exception as e:
        return False, f"reset: {e}"
    finally:
        try:
            sess.close()
        except Exception:
            pass


async def do_reset(*, serial, frequency_hz, kind):
    """kind ∈ {"system", "halt", "ctrl_ap"}. Returns {ok, message}."""
    loop = asyncio.get_running_loop()
    ok, msg = await loop.run_in_executor(
        None, _reset_sync, serial, frequency_hz, kind
    )
    return {"ok": ok, "message": msg}
