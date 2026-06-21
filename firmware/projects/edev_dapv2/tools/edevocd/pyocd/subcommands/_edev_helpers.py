# Shared helpers for the edev_dapv2 vendor-cmd subcommands.
# SPDX-License-Identifier: Apache-2.0

import logging
from contextlib import contextmanager

from ..probe.pydapaccess import DAPAccess
from ..probe.pydapaccess.dap_access_api import DAPAccessIntf

LOG = logging.getLogger(__name__)

# Vendor command IDs — must mirror edev_dapv2's src/dap/dap_config.h.
VCMD_NRF_SYS_RESET = 3   # 0x83
VCMD_MEM_READ      = 6   # 0x86
VCMD_MEM_WRITE     = 7   # 0x87
VCMD_AP_READ       = 8   # 0x88
VCMD_AP_WRITE      = 9   # 0x89
VCMD_CORTEX_M_DUMP = 10  # 0x8A — single-call combo (DPIDR + reset+halt + AP scan + bulk SCB)

# Status bytes mirror src/dap/dap_vendor.c.
STATUS_NAMES = {
    0x00: "OK",
    0x01: "PORT_NOT_SWD",
    0x02: "DP_FAIL",
    0x03: "AP_FAIL",
    0x04: "RESET_TIMEOUT",
    0x05: "HALT_TIMEOUT",
}


def pick_probe(unique_id: str | None):
    """Return a single DAPAccess instance matching `unique_id` (or
    the only one available if `unique_id` is None). Raises RuntimeError."""
    all_daps = DAPAccess.get_connected_devices()
    if not all_daps:
        raise RuntimeError("no CMSIS-DAP probes found")
    if unique_id:
        matches = [d for d in all_daps if unique_id in d.get_unique_id()]
    else:
        matches = all_daps
    if not matches:
        raise RuntimeError(f"no probe matched {unique_id!r}")
    if len(matches) > 1 and not unique_id:
        ids = [d.get_unique_id() for d in matches]
        raise RuntimeError(f"multiple probes; pick one with -u <uid>: {ids}")
    return matches[0]


@contextmanager
def open_probe_swd(unique_id: str | None, frequency: int = 1_000_000):
    """Open DAPAccess, connect SWD, do SWJ wakeup. Yield the dap object.

    Bypasses pyocd's Session / CoreSight target plugin — pure raw DAP.
    Cleans up on exit.
    """
    dap = pick_probe(unique_id)
    dap.open()
    try:
        dap.set_clock(frequency)
        dap.connect(DAPAccessIntf.PORT.SWD)
        # Standard non-dormant SWJ wakeup. Works for DPv1 + DPv2 targets.
        dap.swj_sequence(51, 0xffffffffffffff)
        dap.swj_sequence(16, 0xe79e)
        dap.swj_sequence(51, 0xffffffffffffff)
        dap.swj_sequence(8,  0x00)
        yield dap
    finally:
        try:
            dap.disconnect()
        except Exception:
            pass
        dap.close()


def status_name(b: int) -> str:
    return STATUS_NAMES.get(b, f"UNKNOWN(0x{b:02X})")


# Higher-level wrappers around the raw vendor() calls.

def vcmd_mem_read(dap, addr: int, count: int) -> tuple[int, list[int]]:
    """Returns (status, [words]). status==0 means OK."""
    req = bytes([
        addr & 0xFF, (addr >> 8) & 0xFF, (addr >> 16) & 0xFF, (addr >> 24) & 0xFF,
        count,
    ])
    resp = dap.vendor(VCMD_MEM_READ, list(req))
    if len(resp) < 2:
        raise RuntimeError(f"mem-read response too short: {len(resp)} bytes")
    status = resp[0]
    actual = resp[1]
    words = []
    for i in range(actual):
        w = (resp[2 + i*4]) | (resp[3 + i*4] << 8) | (resp[4 + i*4] << 16) | (resp[5 + i*4] << 24)
        words.append(w)
    return status, words


def vcmd_mem_write(dap, addr: int, words: list[int]) -> tuple[int, int]:
    """Returns (status, count_actual)."""
    count = len(words)
    req = [addr & 0xFF, (addr >> 8) & 0xFF, (addr >> 16) & 0xFF, (addr >> 24) & 0xFF, count]
    for w in words:
        req += [w & 0xFF, (w >> 8) & 0xFF, (w >> 16) & 0xFF, (w >> 24) & 0xFF]
    resp = dap.vendor(VCMD_MEM_WRITE, req)
    if len(resp) < 2:
        raise RuntimeError(f"mem-write response too short: {len(resp)} bytes")
    return resp[0], resp[1]


# ──────────────────────────────────────────────────────────────────
#  Cortex-M debug register access — CPU-register read/write via
#  DCRSR (0xE000EDF4) / DCRDR (0xE000EDF8) / DHCSR (0xE000EDF0).
#  Used by the flash-algorithm runner to set up R0..R2, MSP, PC, XPSR
#  before launching the algo and to inspect S_HALT after.
# ──────────────────────────────────────────────────────────────────

ADDR_DHCSR = 0xE000EDF0
ADDR_DCRSR = 0xE000EDF4
ADDR_DCRDR = 0xE000EDF8

DHCSR_DBGKEY    = (0xA05F << 16)
DHCSR_C_DEBUGEN = (1 <<  0)
DHCSR_C_HALT    = (1 <<  1)
DHCSR_C_STEP    = (1 <<  2)
DHCSR_S_REGRDY  = (1 << 16)
DHCSR_S_HALT    = (1 << 17)

# Core register selectors used by DCRSR.REGSEL.
REG_R0 = 0; REG_R1 = 1; REG_R2 = 2; REG_R3 = 3
REG_MSP = 13; REG_LR = 14; REG_PC = 15; REG_XPSR = 16


def write_cpu_reg(dap, reg: int, value: int, timeout_iter: int = 100) -> tuple[bool, str]:
    """Write a Cortex-M core register via DCRSR/DCRDR.

    Returns (ok, reason). On failure, reason names the failing step so
    callers can log it.
    """
    # 1) data → DCRDR
    st, _ = vcmd_mem_write(dap, ADDR_DCRDR, [value])
    if st != 0:
        return False, f"DCRDR write st={status_name(st)}"
    # 2) selector → DCRSR (REGWnR=bit16=1, REGSEL=bits[6:0])
    sel = (1 << 16) | (reg & 0x7F)
    st, _ = vcmd_mem_write(dap, ADDR_DCRSR, [sel])
    if st != 0:
        return False, f"DCRSR write st={status_name(st)}"
    # 3) poll DHCSR.S_REGRDY
    last_dhcsr = 0
    for _ in range(timeout_iter):
        st_r, words = vcmd_mem_read(dap, ADDR_DHCSR, 1)
        if st_r == 0 and words:
            last_dhcsr = words[0]
            if words[0] & DHCSR_S_REGRDY:
                return True, ""
    return False, f"S_REGRDY never asserted (last DHCSR=0x{last_dhcsr:08X})"


def read_cpu_reg(dap, reg: int, timeout_iter: int = 100) -> tuple[bool, int]:
    """Read a Cortex-M core register via DCRSR/DCRDR. Returns (ok, value)."""
    sel = (0 << 16) | (reg & 0x7F)
    st, _ = vcmd_mem_write(dap, ADDR_DCRSR, [sel])
    if st != 0:
        return False, 0
    for _ in range(timeout_iter):
        st_r, words = vcmd_mem_read(dap, ADDR_DHCSR, 1)
        if st_r == 0 and words and (words[0] & DHCSR_S_REGRDY):
            st_d, dwords = vcmd_mem_read(dap, ADDR_DCRDR, 1)
            if st_d == 0 and dwords:
                return True, dwords[0]
            return False, 0
    return False, 0


def cpu_halt(dap) -> bool:
    """Halt the CPU. Returns True if S_HALT becomes 1."""
    st, _ = vcmd_mem_write(dap, ADDR_DHCSR, [DHCSR_DBGKEY | DHCSR_C_HALT | DHCSR_C_DEBUGEN])
    if st != 0:
        return False
    for _ in range(50):
        st_r, words = vcmd_mem_read(dap, ADDR_DHCSR, 1)
        if st_r == 0 and words and (words[0] & DHCSR_S_HALT):
            return True
    return False


def cpu_run(dap) -> bool:
    """Release the CPU from halt (sets C_DEBUGEN, clears C_HALT)."""
    st, _ = vcmd_mem_write(dap, ADDR_DHCSR, [DHCSR_DBGKEY | DHCSR_C_DEBUGEN])
    return st == 0


def cpu_wait_halt(dap, timeout_ms: int = 200) -> bool:
    """Spin until DHCSR.S_HALT becomes 1 or timeout. Used after running an
    algorithm that ends in BKPT — the CPU halts and S_HALT goes high."""
    import time as _t
    deadline = _t.monotonic() + (timeout_ms / 1000.0)
    while _t.monotonic() < deadline:
        st_r, words = vcmd_mem_read(dap, ADDR_DHCSR, 1)
        if st_r == 0 and words and (words[0] & DHCSR_S_HALT):
            return True
    return False


def vcmd_ap_read(dap, apsel: int, apreg: int) -> tuple[int, int]:
    """Returns (status, value)."""
    resp = dap.vendor(VCMD_AP_READ, [apsel, apreg])
    if len(resp) < 5:
        raise RuntimeError(f"ap-read response too short: {len(resp)} bytes")
    status = resp[0]
    value = resp[1] | (resp[2] << 8) | (resp[3] << 16) | (resp[4] << 24)
    return status, value


# Wrappers that retry on DP_FAIL (status 0x02), waking the wire via a
# fresh DAP_SWJ_Sequence between attempts. Useful on sleepy targets
# (nRF5340) where the debug power-state degrades between operations.

DP_FAIL = 0x02


def _resend_swj(dap) -> None:
    try:
        dap.swj_sequence(51, 0xffffffffffffff)
        dap.swj_sequence(16, 0xe79e)
        dap.swj_sequence(51, 0xffffffffffffff)
        dap.swj_sequence(8,  0x00)
    except Exception:
        pass


def vcmd_ap_read_retry(dap, apsel: int, apreg: int, retries: int = 2) -> tuple[int, int]:
    last = (DP_FAIL, 0)
    for i in range(retries + 1):
        try:
            st, v = vcmd_ap_read(dap, apsel, apreg)
        except Exception:
            if i == retries:
                raise
            _resend_swj(dap)
            continue
        if st != DP_FAIL:
            return st, v
        last = (st, v)
        _resend_swj(dap)
    return last


def vcmd_mem_read_retry(dap, addr: int, count: int, retries: int = 2) -> tuple[int, list[int]]:
    last = (DP_FAIL, [])
    for i in range(retries + 1):
        try:
            st, words = vcmd_mem_read(dap, addr, count)
        except Exception:
            if i == retries:
                raise
            _resend_swj(dap)
            continue
        if st != DP_FAIL:
            return st, words
        last = (st, words)
        _resend_swj(dap)
    return last


def vcmd_ap_write(dap, apsel: int, apreg: int, value: int) -> int:
    """Returns status."""
    req = [apsel, apreg,
           value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF, (value >> 24) & 0xFF]
    resp = dap.vendor(VCMD_AP_WRITE, req)
    if len(resp) < 1:
        raise RuntimeError("ap-write response empty")
    return resp[0]


def parse_int_base_0(s: str) -> int:
    """argparse type for hex (0x…) / decimal / binary (0b…) ints."""
    return int(s, 0)


def parse_size(s: str) -> int:
    """Parse a size string with optional K/M/G suffix (binary).
    Examples: '1024', '1K', '512K', '1M', '0x1000'."""
    s = s.strip()
    mult = 1
    if s and s[-1] in ("K", "k"): mult, s = 1024, s[:-1]
    elif s and s[-1] in ("M", "m"): mult, s = 1024 * 1024, s[:-1]
    elif s and s[-1] in ("G", "g"): mult, s = 1024 * 1024 * 1024, s[:-1]
    return int(s, 0) * mult


# ──────────────────────────────────────────────────────────────────
#                    Chip-family detection
# ──────────────────────────────────────────────────────────────────

# Per-family memory map. Each region is (name, base, size_bytes).
# Sourced from Nordic Product Specifications + ST RMs + pyocd target db
# cross-checks.
FLASH_MAP = {
    "nRF52840": [
        ("flash", 0x00000000, 1024 * 1024),
        ("uicr",  0x10001000, 4096),
        ("ficr",  0x10000000, 4096),
    ],
    "nRF52833": [
        ("flash", 0x00000000, 512 * 1024),
        ("uicr",  0x10001000, 4096),
        ("ficr",  0x10000000, 4096),
    ],
    "nRF52832": [
        ("flash", 0x00000000, 512 * 1024),
        ("uicr",  0x10001000, 4096),
        ("ficr",  0x10000000, 4096),
    ],
    "nRF5340-App": [
        ("flash", 0x00000000, 1024 * 1024),
        ("uicr",  0x00FF8000, 4096),
        ("ficr",  0x00FF0000, 4096),
    ],
    "nRF5340-Net": [
        ("flash", 0x01000000, 256 * 1024),
        ("uicr",  0x01FF8000, 4096),
        ("ficr",  0x01FF0000, 4096),
    ],
    "nRF91xx": [
        ("flash", 0x00000000, 1024 * 1024),
        ("uicr",  0x00FF8000, 4096),
        ("ficr",  0x00FF0000, 4096),
    ],
    "nRF54L15": [
        # nRF54L uses RRAM, but address-mapped the same way.
        ("rram",  0x00000000, 1500 * 1024),     # 1.5 MB
        ("uicr",  0x00FFD000, 4096),
        ("ficr",  0x00FFC000, 4096),
    ],
    "STM32-generic": [
        ("flash", 0x08000000, 0),                # size unknown without IDCODE read
    ],
}


def detect_chip_family(dap) -> dict:
    """Identify the target chip family via DPIDR + AP IDR fingerprint.

    Returns a dict with:
      family:    str — best-effort family name (e.g. "nRF5340-App")
      dpidr:     int
      ahb_ap:    int or None — apsel of the chosen AHB-AP
      flash_regions: list of (name, base, size_bytes)
      detected:  bool — True if family was matched
      notes:     str — what we observed
    """
    # Deferred import — DAPAccessIntf only needed inside the function body.
    from ..probe.pydapaccess.dap_access_api import DAPAccessIntf

    result = {
        "family": "unknown",
        "dpidr": None,
        "ahb_ap": None,
        "flash_regions": [],
        "detected": False,
        "notes": "",
        "ap_summary": [],
    }

    # DPIDR
    try:
        result["dpidr"] = dap.read_reg(DAPAccessIntf.REG.DP_0x0)
    except Exception as e:
        result["notes"] = f"DPIDR read failed: {e}"
        return result

    # Scan APs 0..7. Track which slots faulted vs. were empty vs. populated
    # — the difference matters for disambiguating partial-scan results
    # (locked nRF5340 looks like a healthy nRF54L if you only see AP[2]).
    ap_fingerprint = []
    ap_status = {}   # apsel -> "ok" | "empty" | "fault"
    first_ahb_ap = None
    for apsel in range(8):
        try:
            st, idr = vcmd_ap_read(dap, apsel, 0xFC)
        except Exception:
            ap_status[apsel] = "fault"
            continue
        if st != 0:
            ap_status[apsel] = "fault"
            continue
        if idr == 0:
            ap_status[apsel] = "empty"
            continue
        ap_status[apsel] = "ok"
        cls = (idr >> 13) & 0xF
        jep_cont = (idr >> 24) & 0xF
        jep_id = (idr >> 17) & 0x7F
        designer = (jep_cont << 7) | jep_id
        ap_fingerprint.append({
            "apsel": apsel, "idr": idr, "class": cls, "designer": designer,
        })
        if cls == 0x8 and first_ahb_ap is None:
            first_ahb_ap = apsel
    result["ahb_ap"] = first_ahb_ap
    result["ap_summary"] = ap_fingerprint
    result["ap_status"] = ap_status

    return result


def detect_chip_via_combo(dap) -> dict:
    """Run DAP_VENDOR 0x8A (combo dump) and derive the same family-info
    dict that detect_chip_family() returns. Single firmware call —
    avoids the inter-call gap that closes the DP on chips with
    APPROTECT-enabled UICR (production Ring locked nRF5340).

    Note: this variant ALSO performs reset+halt (the combo does that
    internally when its first byte is 1). It will destroy runtime state.
    """
    result = {
        "family": "unknown",
        "dpidr": None,
        "ahb_ap": None,
        "flash_regions": [],
        "detected": False,
        "notes": "",
        "ap_summary": [],
        "ap_status": {},
    }
    try:
        dump = vcmd_cortex_m_dump(dap, reset_halt=True, ap_count=8)
    except Exception as e:
        result["notes"] = f"combo cmd 0x8A failed: {e}"
        return result

    result["dpidr"] = dump.get("dpidr")
    if result["dpidr"] is None:
        result["notes"] = "combo returned no DPIDR — chip unreachable"
        return result

    # Reshape combo's aps[] into the (apsel, idr, class, designer)
    # fingerprint structure that _classify_family expects.
    ap_fingerprint = []
    ap_status = {}
    first_ahb_ap = None
    for ap in dump["aps"]:
        apsel = ap["apsel"]
        st = ap["status"]
        idr = ap["idr"]
        if st != 0:
            ap_status[apsel] = "fault"
            continue
        if idr is None or idr == 0:
            ap_status[apsel] = "empty"
            continue
        ap_status[apsel] = "ok"
        cls = (idr >> 13) & 0xF
        jep_cont = (idr >> 24) & 0xF
        jep_id = (idr >> 17) & 0x7F
        designer = (jep_cont << 7) | jep_id
        ap_fingerprint.append({
            "apsel": apsel, "idr": idr, "class": cls, "designer": designer,
        })
        if cls == 0x8 and first_ahb_ap is None:
            first_ahb_ap = apsel
    result["ahb_ap"] = first_ahb_ap
    result["ap_summary"] = ap_fingerprint
    result["ap_status"] = ap_status

    _classify_family(result, ap_fingerprint, ap_status, first_ahb_ap)
    return result


def _classify_family(result: dict, ap_fingerprint: list, ap_status: dict,
                     first_ahb_ap):
    """Set result['family'], 'flash_regions', 'detected', 'notes' based on
    the AP topology. Shared between detect_chip_family() (host-side
    scan) and detect_chip_via_combo() (combo-cmd-based scan)."""
    n_ahb = sum(1 for a in ap_fingerprint if a["class"] == 0x8 and a["designer"] == 0x23B)
    n_nordic_ctrl = sum(1 for a in ap_fingerprint if a["class"] == 0x0 and a["designer"] == 0x144)
    nordic_ap_indices = sorted([a["apsel"] for a in ap_fingerprint if a["designer"] == 0x144])
    n_faulted = sum(1 for s in ap_status.values() if s == "fault")
    dpidr = result["dpidr"]

    if n_nordic_ctrl > 0:
        # Nordic chip. Disambiguate by AP topology + DPIDR version.
        if n_ahb == 2 and n_nordic_ctrl == 2:
            # nRF5340 — App+Net cores, each with AHB-AP + CTRL-AP
            result["family"] = "nRF5340-App"
            result["flash_regions"] = FLASH_MAP["nRF5340-App"]
            result["detected"] = True
            result["notes"] = "2× ARM AHB-AP + 2× Nordic CTRL-AP → nRF5340 App core (default; use --core net for Net core map)"
        elif n_ahb == 1 and n_nordic_ctrl == 1 and 1 in nordic_ap_indices:
            # nRF52 — CTRL-AP at AP[1]
            # Sub-family inferred from DPIDR version; default to 840
            result["family"] = "nRF52840"
            result["flash_regions"] = FLASH_MAP["nRF52840"]
            result["detected"] = True
            result["notes"] = "ARM AHB-AP + Nordic CTRL-AP @ AP[1] → nRF52 family (default 840; override with --family)"
        elif n_ahb >= 1 and n_nordic_ctrl == 1 and 4 in nordic_ap_indices:
            # nRF91 — CTRL-AP at AP[4]
            result["family"] = "nRF91xx"
            result["flash_regions"] = FLASH_MAP["nRF91xx"]
            result["detected"] = True
            result["notes"] = "Nordic CTRL-AP @ AP[4] → nRF91 series"
        elif n_ahb >= 1 and n_nordic_ctrl == 1 and 2 in nordic_ap_indices:
            # CTRL-AP at AP[2] — could be either nRF54L15 (single AHB-AP +
            # CTRL-AP@2) OR a partially-locked nRF5340 (AP[0,1,3] FAULT,
            # only AP[2] CTRL-AP visible). Both share DPIDR 0x6BA02477.
            #
            # Disambiguate via AP[3] status:
            #   AP[3] ok+Nordic   → definitely nRF5340 (it has per-core CTRL-AP)
            #   AP[3] empty       → definitely nRF54L (no AP[3] in family)
            #   AP[3] faulted     → ambiguous — default to nRF5340 since
            #                       its flash map (0x00000000+1MB) reaches
            #                       a superset of where the user typically
            #                       wants to read, and is the more common
            #                       chip behind production lockup.
            ap3 = ap_status.get(3, "unscanned")
            if ap3 == "fault" or n_faulted > 0:
                result["family"] = "nRF5340-App"
                result["flash_regions"] = FLASH_MAP["nRF5340-App"]
                result["detected"] = True
                result["notes"] = (f"Partial AP scan ({n_faulted} slot(s) faulted): "
                                  f"AP[2] Nordic CTRL-AP visible, but AP[3] is {ap3!r}. "
                                  "Ambiguous between nRF5340 (locked) and nRF54L15. "
                                  "Defaulting to nRF5340-App; pass --family nRF54L15 "
                                  "if you know the target is nRF54L.")
            elif ap3 == "empty":
                result["family"] = "nRF54L15"
                result["flash_regions"] = FLASH_MAP["nRF54L15"]
                result["detected"] = True
                result["notes"] = "AP[2] Nordic CTRL-AP + AP[3] empty → nRF54L family (RRAM)"
            else:
                result["family"] = "nRF54L15"
                result["flash_regions"] = FLASH_MAP["nRF54L15"]
                result["detected"] = True
                result["notes"] = "Nordic CTRL-AP @ AP[2] → nRF54L family (RRAM)"
    elif n_ahb >= 1:
        # ARM-only AP set. Could be STM32 / RP2350 / many others.
        result["family"] = "Cortex-M-generic"
        result["notes"] = (f"AHB-AP found at AP[{first_ahb_ap}] but no recognised vendor "
                          f"fingerprint. Use dump-mem with explicit --addr/--size.")
    else:
        # No APs reached. DPIDR worked (we got here), but every AP scan failed.
        # Usually means the AHB-AP is locked by APPROTECT or the chip dropped
        # into deep sleep between the DPIDR read and the AP scan.
        result["notes"] = (f"DPIDR alive (0x{dpidr:08X}) but no APs responded to IDR "
                          "read. AHB-AP likely locked (APPROTECT) or chip in deep "
                          "sleep. Pass --family to skip auto-detect, or briefly "
                          "power-cycle the target.")


def vcmd_cortex_m_dump(dap, reset_halt: bool, ap_count: int = 8) -> dict:
    """Invoke DAP_VENDOR 0x8A and decode the packed binary response.

    Returns: {
      "status": int,       # overall status
      "dpidr":  int|None,
      "ahb_ap_sel": int|None,
      "aps":   [{"apsel":int, "status":int, "idr":int|None, "base":int|None}, …],
      "scb":   {"status":int, "count":int, "words":[int, …]},  # 64 words on success
    }
    """
    if ap_count < 1 or ap_count > 8:
        ap_count = 8
    resp = dap.vendor(VCMD_CORTEX_M_DUMP, [1 if reset_halt else 0, ap_count])

    # Expected layout (resp is the response payload AFTER the cmd echo byte):
    #   [0]      = overall status
    #   [1..4]   = DPIDR (LE u32)
    #   [5]      = ahb_ap_sel (0..7 or 0xFF)
    #   [6]      = ap_count (echo)
    #   [7..]    = per-slot 9-byte records × ap_count
    #   [...]    = scb_status (1) + scb_count (1) + scb_words (4 × 64)
    if len(resp) < 8:
        raise RuntimeError(f"cortex-m-dump response too short: {len(resp)} bytes")
    status     = resp[0]
    dpidr_u32  = resp[1] | (resp[2] << 8) | (resp[3] << 16) | (resp[4] << 24)
    ahb_ap_sel = resp[5]
    n          = resp[6]

    aps = []
    off = 7
    for i in range(n):
        if off + 9 > len(resp):
            break
        st  = resp[off]
        idr = resp[off+1] | (resp[off+2] << 8) | (resp[off+3] << 16) | (resp[off+4] << 24)
        bs  = resp[off+5] | (resp[off+6] << 8) | (resp[off+7] << 16) | (resp[off+8] << 24)
        aps.append({
            "apsel": i,
            "status": st,
            "idr":   idr if st == 0 else None,
            "base":  bs  if (st == 0 and bs not in (0, 0xFFFFFFFF)) else (bs if st == 0 else None),
        })
        off += 9

    scb_words = []
    scb_status = None
    scb_count  = 0
    if off + 2 <= len(resp):
        scb_status = resp[off]
        scb_count  = resp[off + 1]
        off += 2
        for i in range(64):
            if off + 4 > len(resp):
                break
            scb_words.append(
                resp[off] | (resp[off+1] << 8) | (resp[off+2] << 16) | (resp[off+3] << 24)
            )
            off += 4

    # ROM table block follows SCB block.
    rom_words = []
    rom_status = None
    rom_count  = 0
    if off + 2 <= len(resp):
        rom_status = resp[off]
        rom_count  = resp[off + 1]
        off += 2
        for i in range(32):
            if off + 4 > len(resp):
                break
            rom_words.append(
                resp[off] | (resp[off+1] << 8) | (resp[off+2] << 16) | (resp[off+3] << 24)
            )
            off += 4

    return {
        "status":     status,
        "dpidr":      dpidr_u32 if dpidr_u32 != 0 else None,
        "ahb_ap_sel": ahb_ap_sel if ahb_ap_sel != 0xFF else None,
        "aps":        aps,
        "scb":        {"status": scb_status, "count": scb_count, "words": scb_words},
        "rom":        {"status": rom_status, "count": rom_count, "words": rom_words},
    }
