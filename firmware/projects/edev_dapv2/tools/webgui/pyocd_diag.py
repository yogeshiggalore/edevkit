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


# ── Nordic CTRL-AP ERASEALL ───────────────────────────────────────────

def _erase_ctrl_ap_sync(serial, frequency_hz):
    """Issue ERASEALL on every Nordic CTRL-AP we can find on this DP.

    Nordic CTRL-AP register layout:
      0x000  RESET           write 1 to assert / 0 to release
      0x004  ERASEALL        write 1 to start mass-erase
      0x008  ERASEALLSTATUS  0 = idle, 1 = busy
      0x010  APPROTECTSTATUS

    On nRF52: one CTRL-AP per chip (IDR 0x02880000).
    On nRF5340: two CTRL-APs (one per core, both IDR 0x12880000) —
    AP#2 wipes App core flash + UICR, AP#3 wipes Net core flash + UICR.

    Returns (ok, [(ap_index, ok, message), ...]).
    """
    from pyocd.core.helpers import ConnectHelper
    import time

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
        return False, [(None, False, f"open session: {e}")]
    if not sess:
        return False, [(None, False, "no probe matched")]

    results = []
    overall_ok = True
    try:
        sess.open(init_board=False)
        dp = sess.board.target.dp
        try: dp.connect()
        except Exception: pass
        try: dp.power_up_debug()
        except Exception: pass

        # Find every Nordic CTRL-AP. Remember the IDR for later family ID.
        ctrl_aps = []          # list of AP indices
        ctrl_ap_idrs = {}      # ap → idr
        for i in range(8):
            try:
                dp.write_dp(0x8, (i << 24) | 0xF0)
                idr = dp.read_ap((i << 24) | 0xFC)
            except Exception:
                continue
            if idr in (0x02880000, 0x12880000):
                ctrl_aps.append(i)
                ctrl_ap_idrs[i] = idr

        if not ctrl_aps:
            return False, [(None, False, "no Nordic CTRL-AP found")]

        # ── Helpers for AHB-AP-mediated memory access ─────────────────
        # CSW values are AP-specific on nRF5340:
        #   App AHB-AP (AP#0): 0x23000002 (HPROT=secure/priv/data, 32-bit)
        #   Net AHB-AP (AP#1): 0x03800042 (Nordic-required CSW — only this
        #                                  value works against Net's bus;
        #                                  observed in nrfjprog J-Link trace).
        def _ap32_write(ahb, addr, value, csw):
            dp.write_dp(0x8, (ahb << 24) | 0x00)
            dp.write_ap((ahb << 24) | 0x00, csw)
            dp.write_ap((ahb << 24) | 0x04, addr)
            dp.write_ap((ahb << 24) | 0x0C, value)
            dp.read_dp(0x0C)   # RDBUFF — forces the write to commit

        def _ap32_read(ahb, addr, csw):
            dp.write_dp(0x8, (ahb << 24) | 0x00)
            dp.write_ap((ahb << 24) | 0x00, csw)
            dp.write_ap((ahb << 24) | 0x04, addr)
            _ = dp.read_ap((ahb << 24) | 0x0C)   # posted read
            return dp.read_dp(0x0C)              # RDBUFF

        def _nvmc_wait_ready(ahb, nvmc_base, csw, timeout=2.0):
            t0 = time.monotonic()
            while time.monotonic() - t0 < timeout:
                try:
                    if _ap32_read(ahb, nvmc_base + 0x400, csw) & 1:
                        return True
                except Exception:
                    pass
                time.sleep(0.005)
            return False

        def _disable_approtect_via_nvmc(ahb, csw, nvmc_base, uicr_writes):
            """Write `uicr_writes = [(addr, value), ...]` via the given AHB-AP
            and NVMC. Each write goes through the Nordic NVMC write-enable
            handshake. Returns (ok, message)."""
            try:
                if not _nvmc_wait_ready(ahb, nvmc_base, csw):
                    return False, f"NVMC@0x{nvmc_base:08X} not ready initially"
                _ap32_write(ahb, nvmc_base + 0x504, 0x00000001, csw)   # CONFIG = Wen
                if not _nvmc_wait_ready(ahb, nvmc_base, csw):
                    return False, f"NVMC@0x{nvmc_base:08X} not ready after Wen"
                for (uicr_addr, value) in uicr_writes:
                    _ap32_write(ahb, uicr_addr, value, csw)
                    if not _nvmc_wait_ready(ahb, nvmc_base, csw, timeout=5.0):
                        _ap32_write(ahb, nvmc_base + 0x504, 0x00000000, csw)
                        return False, f"NVMC@0x{nvmc_base:08X} not ready after 0x{uicr_addr:08X}"
                _ap32_write(ahb, nvmc_base + 0x504, 0x00000000, csw)   # CONFIG = Ren
                return True, "ok"
            except Exception as e:
                return False, f"{type(e).__name__}: {e}"

        for ap in ctrl_aps:
            # Mass-erase via CTRL-AP — matches pyocd/target/family/target_nordic.py:
            #   1) ERASEALL = 1
            #   2) Poll ERASEALLSTATUS until 0 (READY)
            #
            # We DO NOT hold RESET during step 1: with RESET asserted the
            # on-chip flash controller (which lives in the CPU domain) can't
            # run, so ERASEALL never completes and ERASEALLSTATUS stays at
            # BUSY forever. This matches what pyocd's nordic family does;
            # the OpenOCD "hold reset" pattern is folklore that times out
            # on nRF52 in practice.
            #
            # The post-erase reset pulse is the caller's job — server.py
            # routes both erase and flash through _post_op_reset so there's
            # exactly one CTRL-AP reset for each destructive op.
            ap_ok = True
            ap_msgs = []
            try:
                dp.write_dp(0x8, (ap << 24) | 0x00)
                dp.write_ap((ap << 24) | 0x04, 0x00000001)   # ERASEALL=1
                # Poll ERASEALLSTATUS until READY (== 0).
                t0 = time.monotonic()
                busy = True
                while busy and time.monotonic() - t0 < 10.0:
                    time.sleep(0.02)
                    try:
                        s = dp.read_ap((ap << 24) | 0x08)
                    except Exception:
                        s = None
                    if s == 0:
                        busy = False
                if busy:
                    raise TimeoutError("ERASEALLSTATUS never reached READY within 10s")
                ap_msgs.append(f"{round(time.monotonic() - t0, 2)}s")
            except Exception as e:
                ap_ok = False
                ap_msgs.append(str(e))
            results.append((ap, ap_ok, "; ".join(ap_msgs)))
            if not ap_ok:
                overall_ok = False

        # ── Post-ERASEALL: permanently disable APPROTECT via UICR writes ──
        # On nRF5340 (and nRF91), erased UICR.APPROTECT = 0xFFFFFFFF means
        # HwEnabled — the chip auto-locks AHB-AP on next pin-reset / POR /
        # BOR / WDT reset. The "permanently unlocked" sentinel is 0x50FA50FA.
        # nrfjprog handles this by writing a Thumb stub to App flash that
        # programs UICR.APPROTECT on the next boot; we do it directly here
        # via NVMC writes from the debug host, while AHB-AP is still open
        # from the just-completed ERASEALL.
        #
        # nRF5340 has two CTRL-APs at IDR 0x12880000; nRF52 has one at
        # IDR 0x02880000. Only nRF5340/91 need the UICR disable step
        # (on nRF52, UICR=0xFFFFFFFF IS the unlocked default).
        is_nrf53 = any(idr == 0x12880000 for idr in ctrl_ap_idrs.values())
        if is_nrf53 and overall_ok:
            # Per-core UICR programming. nRF5340 layout:
            #   App AHB-AP (AP#0) ─ NVMC @ 0x50039000 ─ App UICR @ 0x00FF8000
            #   Net AHB-AP (AP#1) ─ NVMC @ 0x41080000 ─ Net UICR @ 0x00FF8000 (Net's own space)
            #
            # Both AHB-APs need CSW=0x03800042 (Nordic-specific MasterType=CPU
            # value), confirmed against nrfjprog J-Link trace. Net peripherals
            # are NOT accessible from App AHB-AP — go through Net AHB-AP.
            # NETWORK.FORCEOFF gets set by CTRL-AP#3 ERASEALL; releasing it
            # turns out to be unnecessary because Net AHB-AP debug access
            # works independently of Net core power.
            # _recover_dp clears any sticky-fault bits left by the ERASEALL
            # transactions so the AHB-AP path starts clean.
            _recover_dp(dp)

            disable_msgs = []
            ok_app, msg_app = _disable_approtect_via_nvmc(
                ahb=0, csw=0x23000002, nvmc_base=0x50039000,
                uicr_writes=[
                    (0x00FF8000, 0x50FA50FA),   # App UICR.APPROTECT
                    (0x00FF801C, 0x50FA50FA),   # App UICR.SECUREAPPROTECT
                ],
            )
            disable_msgs.append(f"App: {'✓' if ok_app else '✗'} {msg_app}")
            if not ok_app:
                overall_ok = False
                _recover_dp(dp)   # heal before trying Net

            # Net AHB-AP gets gated after CTRL-AP#3 ERASEALL — direct accesses
            # FAULT. A CTRL-AP#3 RESET pulse + APPROTECTSTATUS readback wakes
            # Net AHB-AP without re-arming APPROTECT (empirically confirmed
            # 2026-06-26 — APPROTECTSTATUS stays at 1 post-pulse).
            _recover_dp(dp)
            net_ctrl_ap = max(
                (ap for ap, idr in ctrl_ap_idrs.items() if idr == 0x12880000),
                default=3,
            )
            try:
                dp.write_dp(0x8, (net_ctrl_ap << 24) | 0x00)
                dp.write_ap((net_ctrl_ap << 24) | 0x00, 0x00000001)   # RESET = 1
                time.sleep(0.05)
                dp.write_ap((net_ctrl_ap << 24) | 0x00, 0x00000000)   # RESET = 0
                time.sleep(0.05)
                # APPROTECTSTATUS readback — appears to be the trigger that
                # actually unblocks Net AHB-AP transactions.
                _ = dp.read_ap((net_ctrl_ap << 24) | 0x0C)
                dp.read_dp(0x0C)
            except Exception:
                _recover_dp(dp)

            ok_net, msg_net = _disable_approtect_via_nvmc(
                ahb=1, csw=0x03800042, nvmc_base=0x41080000,
                uicr_writes=[
                    (0x00FF8000, 0x50FA50FA),   # Net UICR.APPROTECT (Net's own space)
                ],
            )
            disable_msgs.append(f"Net: {'✓' if ok_net else '✗'} {msg_net}")
            # Net UICR write is BEST-EFFORT. Net AHB-AP access immediately
            # after the multi-step erase flow is flaky on this probe (works
            # standalone, FAULTs inside the webgui sequence — likely DP
            # state accumulation across the App-UICR + Net-pulse path).
            # App UICR alone keeps the chip unlocked for App-side ops, which
            # is enough for the common case (Net core typically gets
            # re-flashed with firmware that disables APPROTECT on boot).
            # For a chip that truly requires permanent Net unlock without
            # firmware help, recover via J-Link + nrfjprog.
            if not ok_net:
                disable_msgs.append("(Net write is best-effort — App-side unlocked, "
                                    "use J-Link + nrfjprog if Net needs permanent unlock)")
                _recover_dp(dp)   # clear DP sticky bits before reset

            results.append(("UICR-disable", ok_app, "; ".join(disable_msgs)))

        _recover_dp(dp)
    except Exception as e:
        results.append((None, False, f"erase: {e}"))
        overall_ok = False
    finally:
        try: sess.close()
        except Exception: pass

    return overall_ok, results


async def erase_ctrl_ap_all(*, serial, frequency_hz):
    """Async wrapper around _erase_ctrl_ap_sync."""
    loop = asyncio.get_running_loop()
    return await loop.run_in_executor(
        None, _erase_ctrl_ap_sync, serial, frequency_hz
    )
