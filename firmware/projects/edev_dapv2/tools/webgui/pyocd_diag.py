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


# ─── nRF5340 APPROTECT-disable Thumb stub ─────────────────────────────
# Source: edevkit/firmware/projects/edev_dapv2/tools/nrf53_disable_approtect.s
# Build:  arm-none-eabi-as -mcpu=cortex-m33 -mthumb nrf53_disable_approtect.s
#
# Layout: starts with a full Cortex-M33 vector table (initial MSP at +0x00,
# Reset at +0x04, fault handlers at +0x08..+0x3C) followed by the code.
# The fault handlers all point to a spin loop with BKPT #1 — without these,
# any fault during stub execution causes lockup (CPU dispatches to
# 0xFFFFFFFE → double fault → lockup).
#
# nrfjprog's 3748-byte recover image installs the same fault-handler
# vectors; verified by reading flash post-recover. The actual UICR work
# is inline right after the vectors (nrfjprog puts it at offset 0x448
# after data-copy bootstrap; we skip that since our stub is PIC).
#
# Writes the stub directly to App flash @ 0x00000000 via App NVMC.
# SYSRESETREQ boots the CPU into our vector table — VTOR resets to 0,
# so flash[0] is the only place that survives.
_NRF53_DISABLE_STUB = (
    b"\x00\x00\x01\x20\x51\x00\x00\x00\x41\x00\x00\x00\x41\x00\x00\x00"
    b"\x41\x00\x00\x00\x41\x00\x00\x00\x41\x00\x00\x00\x41\x00\x00\x00"
    b"\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x41\x00\x00\x00"
    b"\x41\x00\x00\x00\x00\x00\x00\x00\x41\x00\x00\x00\x41\x00\x00\x00"
    b"\x33\x48\x71\x46\x01\x60\x4f\xf0\x00\x50\x32\x49\x01\x60\xfe\xe7"
    b"\x4f\xf0\x00\x50\x4f\xf0\x11\x31\x01\x60\x2f\x48\x4f\xf0\xff\x31"
    b"\x01\x60\x01\x61\x01\x62\x01\x63\x01\x64\x01\x65\x01\x66\x01\x67"
    b"\x4f\xf0\x00\x50\x4f\xf0\x22\x31\x01\x60\x28\x48\x4f\xf0\xff\x31"
    b"\x01\x60\x27\x4c\x27\x4d\x28\x4e\x22\x68\x01\x2a\xfc\xd1\x01\x22"
    b"\x2a\x60\x22\x68\x01\x2a\xfc\xd1\x24\x48\x06\x60\x22\x68\x01\x2a"
    b"\xfc\xd1\x23\x48\x06\x60\x22\x68\x01\x2a\xfc\xd1\x00\x22\x2a\x60"
    b"\x22\x68\x01\x2a\xfc\xd1\x4f\xf0\x00\x50\x4f\xf0\x33\x31\x01\x60"
    b"\x1c\x48\x00\x22\x02\x60\x00\x22\x4f\xf4\x00\x43\x01\x32\x9a\x42"
    b"\xfc\xd3\x4f\xf0\x00\x50\x4f\xf0\x44\x31\x01\x60\x16\x4c\x17\x4d"
    b"\x22\x68\x01\x2a\xfc\xd1\x01\x22\x2a\x60\x22\x68\x01\x2a\xfc\xd1"
    b"\x13\x48\x06\x60\x22\x68\x01\x2a\xfc\xd1\x00\x22\x2a\x60\x22\x68"
    b"\x01\x2a\xfc\xd1\x4f\xf0\x00\x50\x0e\x49\x01\x60\xfe\xe7\x00\x00"
    b"\x04\x00\x00\x20\xd5\x00\xdf\xba\x04\x16\x08\x50\x00\x54\x00\x50"
    b"\x00\x94\x03\x50\x04\x95\x03\x50\xfa\x50\xfa\x50\x00\x80\xff\x00"
    b"\x1c\x80\xff\x00\x14\x56\x00\x50\x00\x04\x08\x41\x04\x05\x08\x41"
    b"\x00\x80\xff\x01\xde\xc0\xad\xde"
)
# Stage magic values written to SRAM 0x20000000 by the stub:
#   0x11111111  reset_handler entered
#   0x22222222  VMC RAM power-on done
#   0x33333333  App UICR programmed
#   0x44444444  NETWORK.FORCEOFF=0 + delay done
#   0xDEADC0DE  Net UICR programmed — stub complete ✓
#   0xBADF00D5  fault_handler hit (LR recorded at 0x20000004)
_STUB_TIMEOUT_S    = 5.0          # typical run is < 100 ms once running


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
            # Try the on-target stub first (programs BOTH App + Net UICR
            # via on-chip NVMC after VMC RAM power-on). If the stub fails
            # for any reason — chip in unusual state, NVMC sticky, etc. —
            # fall back to host-side App UICR write (which is bulletproof)
            # and leave Net as session-only.
            _recover_dp(dp)
            ok_stub, msg_stub = _run_disable_stub(
                dp, session=sess, serial=serial, frequency_hz=frequency_hz,
                app_ahb=0,
            )
            if ok_stub:
                results.append(("UICR-disable", True, f"stub: ✓ {msg_stub}"))
            else:
                _recover_dp(dp)
                ok_app, msg_app = _disable_approtect_via_nvmc(
                    ahb=0, csw=0x23000002, nvmc_base=0x50039000,
                    uicr_writes=[
                        (0x00FF8000, 0x50FA50FA),
                        (0x00FF801C, 0x50FA50FA),
                    ],
                )
                fallback = (f"stub failed ({msg_stub}); "
                            f"fallback App-only: {'✓' if ok_app else '✗'} {msg_app}; "
                            f"Net: SESSION-only (flash firmware that writes APPROTECT.DISABLE, "
                            f"or use J-Link + nrfjprog --recover --coprocessor CP_NETWORK)")
                results.append(("UICR-disable", ok_app, fallback))
                if not ok_app:
                    overall_ok = False
                    _recover_dp(dp)

        _recover_dp(dp)
    except Exception as e:
        results.append((None, False, f"erase: {e}"))
        overall_ok = False
    finally:
        try: sess.close()
        except Exception: pass

    return overall_ok, results


def _run_disable_stub(dp, *, session=None, serial=None, frequency_hz=None,
                      app_ahb: int = 0) -> tuple[bool, str]:
    """Write the nRF5340 APPROTECT-disable stub to App flash and run it.

    Sequence:
      1. Write 2-word vector table + stub bytes to App flash @ 0x00000000
         via App NVMC. SYSRESETREQ resets VTOR, so the stub HAS to live
         at flash[0] for the post-reset vector fetch to land on it.
      2. Halt-on-reset (DEMCR.VC_CORERESET=1) + SYSRESETREQ.
      3. CPU comes out of reset, loads SP+PC from our vectors in flash,
         halts at the first stub instruction.
      4. Clear VC_CORERESET, release halt — stub runs.
      5. Stub does VMC RAM power-on (so stack/exception entry works),
         then App+Net UICR programming via NVMC, then BKPT.
      6. Poll DHCSR.S_HALT until set (BKPT fired) or timeout.
      7. Verify UICR readbacks.

    Returns (ok, message). Stub is in _NRF53_DISABLE_STUB (184 B).
    """
    import time
    ahb = app_ahb
    CSW_SINGLE     = 0x23000002   # 32-bit, no autoincrement
    CSW_AUTOINC    = 0x23000012   # 32-bit, single-word autoincrement
    DHCSR          = 0xE000EDF0   # Debug Halt Control/Status
    DCRSR          = 0xE000EDF4   # Debug Core Register Selector
    DCRDR          = 0xE000EDF8   # Debug Core Register Data
    DEMCR          = 0xE000EDFC
    DBGKEY         = 0xA05F0000
    C_DEBUGEN      = 1 << 0
    C_HALT         = 1 << 1
    S_REGRDY       = 1 << 16
    S_HALT         = 1 << 17

    def write_ap_select():
        dp.write_dp(0x8, (ahb << 24) | 0x00)

    def ap_write(addr, value, csw=CSW_SINGLE):
        write_ap_select()
        dp.write_ap((ahb << 24) | 0x00, csw)
        dp.write_ap((ahb << 24) | 0x04, addr)
        dp.write_ap((ahb << 24) | 0x0C, value)
        dp.read_dp(0x0C)

    def ap_read(addr, csw=CSW_SINGLE):
        write_ap_select()
        dp.write_ap((ahb << 24) | 0x00, csw)
        dp.write_ap((ahb << 24) | 0x04, addr)
        _ = dp.read_ap((ahb << 24) | 0x0C)
        return dp.read_dp(0x0C)

    def wait_regrdy(timeout=0.5):
        t0 = time.monotonic()
        while time.monotonic() - t0 < timeout:
            if ap_read(DHCSR) & S_REGRDY:
                return True
            time.sleep(0.001)
        return False

    def write_cpu_reg(reg_num, value):
        """Write CPU register `reg_num` (13=MSP, 15=PC, 16=xPSR)."""
        ap_write(DCRDR, value)
        ap_write(DCRSR, (1 << 16) | (reg_num & 0x7F))   # REGWnR=1
        if not wait_regrdy():
            return False
        return True

    # On Cortex-M33 with TrustZone, SYSRESETREQ resets VTOR (Secure: to 0).
    # So pre-setting VTOR in RAM doesn't survive — the CPU always fetches
    # SP+PC from flash[0x00000000..7] after reset. With erased flash that
    # gives 0xFFFFFFFF → instant lockup, and S_LOCKUP can't be cleared
    # except by reset, creating an unbreakable cycle.
    #
    # Solution (mirrors nrfjprog's approach but with a 144-byte payload
    # instead of 3748 bytes): write the stub INTO App flash starting at
    # 0x00000000, so the post-reset vector fetch lands on valid values
    # and the CPU runs our code instead of going to lockup. The stub
    # programs UICR and halts at BKPT, where we read the result.

    APP_NVMC_BASE = 0x50039000

    def nvmc_wait_ready(timeout=2.0):
        t0 = time.monotonic()
        while time.monotonic() - t0 < timeout:
            try:
                if ap_read(APP_NVMC_BASE + 0x400) & 1:
                    return True
            except Exception:
                pass
            time.sleep(0.005)
        return False

    outer_phase = "init"
    try:
        # 0. Halt the App CPU before any flash writes. After CTRL-AP
        #    ERASEALL the CPU is in lockup executing erased-flash
        #    0xFFFFFFFF; an unhalted lockup CPU can stomp on NVMC state
        #    or simply slow down/fault the bus. Halt sets S_HALT even in
        #    lockup state (verified earlier).
        outer_phase = "halt App"
        ap_write(DHCSR, DBGKEY | C_DEBUGEN | C_HALT)
        t0 = time.monotonic()
        while time.monotonic() - t0 < 0.5:
            try:
                if ap_read(DHCSR) & S_HALT:
                    break
            except Exception:
                pass
            time.sleep(0.005)

        # 1. Stub binary starts with its own vector table (initial MSP +
        #    Reset + fault handlers); write verbatim to flash[0]. CPU
        #    boots from these vectors after SYSRESETREQ.
        blob = _NRF53_DISABLE_STUB
        if len(blob) % 4:
            blob = blob + b"\x00" * (4 - (len(blob) % 4))
        flash_writes = []
        for off in range(0, len(blob), 4):
            word = (blob[off] | (blob[off+1] << 8)
                    | (blob[off+2] << 16) | (blob[off+3] << 24))
            flash_writes.append((off, word))

        # 2. NVMC write-enable, write all words, NVMC write-disable.
        phase = "pre-NVMC ready"
        last_ok_addr = -1
        try:
            if not nvmc_wait_ready():
                return False, "App NVMC not ready before stub flash"
            phase = "NVMC=Wen"
            ap_write(APP_NVMC_BASE + 0x504, 0x00000001)
            phase = "post-Wen ready"
            if not nvmc_wait_ready():
                return False, "App NVMC didn't acknowledge Wen"
            for addr, value in flash_writes:
                phase = f"flash write 0x{addr:08X}"
                ap_write(addr, value)
                phase = f"flash wait 0x{addr:08X}"
                if not nvmc_wait_ready(timeout=5.0):
                    ap_write(APP_NVMC_BASE + 0x504, 0x00000000)
                    return False, f"App NVMC stalled at 0x{addr:08X} (last_ok=0x{last_ok_addr:08X})"
                last_ok_addr = addr
            phase = "NVMC=Ren"
            ap_write(APP_NVMC_BASE + 0x504, 0x00000000)
            phase = "post-Ren ready"
            if not nvmc_wait_ready():
                return False, "App NVMC didn't acknowledge Ren"
        except Exception as e:
            return False, f"fault at phase '{phase}' (last_ok=0x{max(last_ok_addr,0):08X}): {type(e).__name__}: {e}"

        # 3. Reset via CTRL-AP#2 RESET pulse. Going through the chip's
        #    CTRL-AP reset (vs SYSRESETREQ through AHB-AP) keeps the
        #    SWD bus clean — SYSRESETREQ writes to AIRCR via AHB-AP
        #    which faults mid-transaction when the chip resets, and
        #    the queued-fault recovery is brittle in pyocd. CTRL-AP
        #    RESET is a dedicated reset request path. We also pre-set
        #    DEMCR.VC_CORERESET so the CPU halts at the first
        #    instruction of our stub when reset releases.
        outer_phase = "VC_CORERESET set"
        ap_write(DEMCR, 0x00000001)
        outer_phase = "CTRL-AP#2 RESET pulse"
        # CTRL-AP#2 = App core control AP on nRF5340. Reset pulse causes
        # a FAULT ACK on the in-flight SWD transaction (chip resets mid-
        # transfer); swallow it. Stub will run independently.
        app_ctrl_ap = 2
        try:
            dp.write_dp(0x8, (app_ctrl_ap << 24) | 0x00)
            dp.write_ap((app_ctrl_ap << 24) | 0x00, 0x00000001)
            time.sleep(0.05)
            dp.write_ap((app_ctrl_ap << 24) | 0x00, 0x00000000)
        except Exception:
            pass
        time.sleep(0.5)   # generous: stub completes in < 50 ms

        # Close this session so we can open a fresh one — pyocd's DP
        # state is unrecoverable after a reset-induced FAULT ACK.
        if session is not None:
            try: session.close()
            except Exception: pass
        time.sleep(0.1)
        return _verify_stub_run(serial=serial, frequency_hz=frequency_hz)
    except Exception as e:
        return False, f"fault in phase '{outer_phase}': {type(e).__name__}: {e}"


def _verify_stub_run(*, serial, frequency_hz) -> tuple[bool, str]:
    """Verify the stub ran by reading the stub's stage-magic + UICR cells
    via probe-rs subprocess. We use probe-rs (not pyocd) because the
    parent pyocd session just closed but USB takes a moment to release,
    and probe-rs auto-retries USB-claim more gracefully.

    Stub writes a magic value to SRAM 0x20000000 at each stage:
      0x11111111 reset entered, 0x22222222 VMC done, 0x33333333 App UICR done,
      0x44444444 Net release done, 0xDEADC0DE all done, 0xBADF00D5 faulted.
    """
    import subprocess
    import shutil
    import re
    probe_rs = shutil.which("probe-rs")
    if not probe_rs:
        return False, "probe-rs not on PATH for verify"

    def _probe_rs_read(addr, count, core=0):
        cmd = [probe_rs, "read", "--probe", f"2e8a:000c:{serial}",
               "--protocol", "swd", "--chip", "nRF5340_xxAA",
               "--speed", str(int(frequency_hz / 1000)),
               "--core", str(core), "b32", f"0x{addr:08x}", str(count)]
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=15)
        except subprocess.TimeoutExpired:
            return None, "timeout"
        words = []
        for line in r.stdout.splitlines():
            if "WARN" in line: continue
            for tok in line.strip().split():
                if re.fullmatch(r"[0-9a-fA-F]{8}", tok):
                    words.append(int(tok, 16))
        if r.returncode != 0 and not words:
            return None, r.stderr.strip().splitlines()[-1] if r.stderr else "exit nonzero"
        return words, None

    # Read SRAM stage magic (App-core view).
    words, err = _probe_rs_read(0x20000000, 2, core=0)
    if words is None:
        return False, f"verify read failed: {err}"
    stage = words[0]
    fault_lr = words[1] if len(words) > 1 else 0

    STAGE_NAMES = {
        0x11111111: "reset entered",
        0x22222222: "VMC done",
        0x33333333: "App UICR done",
        0x44444444: "Net release done",
        0xDEADC0DE: "✓ FULLY DONE",
        0xBADF00D5: f"✗ FAULTED (LR=0x{fault_lr:08X})",
    }
    stage_desc = STAGE_NAMES.get(stage, f"unknown 0x{stage:08X}")

    # Read UICR cells.
    uicr, _ = _probe_rs_read(0x00FF8000, 8, core=0)
    app_approt = uicr[0] if uicr else 0
    app_secapprot = uicr[7] if uicr and len(uicr) > 7 else 0
    net_uicr, _ = _probe_rs_read(0x01FF8000, 1, core=0)
    net_approt = net_uicr[0] if net_uicr else 0

    ok = (stage == 0xDEADC0DE
          and app_approt == 0x50FA50FA
          and app_secapprot == 0x50FA50FA
          and net_approt == 0x50FA50FA)
    return ok, (f"stage: {stage_desc}; "
                f"App.AP=0x{app_approt:08X}, App.SECAP=0x{app_secapprot:08X}, "
                f"Net.AP=0x{net_approt:08X}")


async def erase_ctrl_ap_all(*, serial, frequency_hz):
    """Async wrapper around _erase_ctrl_ap_sync."""
    loop = asyncio.get_running_loop()
    return await loop.run_in_executor(
        None, _erase_ctrl_ap_sync, serial, frequency_hz
    )
