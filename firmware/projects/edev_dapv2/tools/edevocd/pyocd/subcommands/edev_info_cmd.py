# `edevocd info` — comprehensive Cortex-M debug-port dump via edev_dapv2.
# SPDX-License-Identifier: Apache-2.0
#
# Reads DPIDR → scans AP slots 0..7 → for each MEM-AP reads BASE →
# for the AHB-AP reads CPUID, DHCSR, DEMCR, MPU TYPE, FPU CPACR,
# DWT CTRL, FPB CTRL, and (optionally) walks the ROM table.
#
# Two output modes:
#   default  — human-readable text to stdout
#   --json   — JSON to stdout: {"data": {…structured…}, "text": "…same text…"}
#
# All reads go through DAP_VENDOR 0x86 / 0x88 on edev_dapv2 — bypasses
# pyocd's CoreSight target plugin entirely, so this works even with
# the Tier 1 host-side SWD bug unfixed.

import argparse
import io
import json
import logging
from typing import Any, Dict, List, Optional

from .base import SubcommandBase
from ._edev_helpers import (
    open_probe_swd, vcmd_mem_read, vcmd_ap_read, vcmd_mem_write,
    vcmd_ap_read_retry, vcmd_mem_read_retry, vcmd_cortex_m_dump,
    status_name, VCMD_NRF_SYS_RESET,
)

LOG = logging.getLogger(__name__)


# ───────── Cortex-M decode tables ─────────

IMPLEMENTER = {
    0x41: "ARM", 0x44: "DEC", 0x4D: "Motorola/Freescale", 0x51: "Qualcomm",
    0x56: "Marvell", 0x69: "Intel",
}

ARCH = {
    # The CPUID architecture nibble is coarse — multiple Cortex-M families
    # share a value. Read the PARTNO to disambiguate.
    0xC: "ARMv6-M / ARMv8-M Baseline",
    0xF: "ARMv7-M / ARMv7E-M / ARMv8-M Mainline",
}

PARTNO = {
    0xC20: "Cortex-M0",   0xC21: "Cortex-M1",   0xC23: "Cortex-M3",
    0xC24: "Cortex-M4",   0xC27: "Cortex-M7",   0xC60: "Cortex-M0+",
    0xD20: "Cortex-M23",  0xD21: "Cortex-M33",  0xD22: "Cortex-M55",
    0xD23: "Cortex-M85",
}

AP_CLASS = {
    0x0: "(custom / non-standard)",
    0x1: "(reserved)",
    0x8: "MEM-AP",
    0x9: "JTAG-AP",
}

MEM_AP_TYPE = {
    0x1: "AMBA AHB-3",
    0x2: "AMBA APB-2/3",
    0x4: "AMBA AXI3/4",
    0x6: "AMBA AHB-5",
    0x7: "AMBA APB-4/5",
    0x8: "AMBA AXI5",
}

# Well-known peripherals at fixed SCS addresses on most Cortex-M targets.
WELL_KNOWN_PERIPHS = {
    0xE000E000: "SCS (NVIC + SCB + SysTick)",
    0xE0001000: "DWT — Data Watchpoint & Trace",
    0xE0002000: "FPB — Flash Patch & Breakpoint",
    0xE0000000: "ITM — Instrumentation Trace Macrocell",
    0xE0040000: "TPIU — Trace Port Interface Unit",
    0xE0041000: "ETM — Embedded Trace Macrocell",
}

# Designer codes worth labelling. Encoded as (cont<<7)|id.
DESIGNER_LABEL = {
    0x23B: "ARM",
    0x144: "Nordic Semiconductor",
    0x47F: "Apple",
}

# Cortex-M SCB / debug register addresses.
ADDR_CPUID    = 0xE000ED00
ADDR_ICSR     = 0xE000ED04
ADDR_AIRCR    = 0xE000ED0C
ADDR_DHCSR    = 0xE000EDF0
ADDR_DEMCR    = 0xE000EDFC
ADDR_SHCSR    = 0xE000ED24
ADDR_CCR      = 0xE000ED14
ADDR_MPU_TYPE = 0xE000ED90
ADDR_CPACR    = 0xE000ED88
ADDR_DWT_CTRL = 0xE0001000
ADDR_FPB_CTRL = 0xE0002000


# ───────── Decode helpers (called from _collect, return dicts) ─────────

def _decode_dpidr(dpidr: int) -> Dict[str, Any]:
    return {
        "designer": (dpidr >>  1) & 0x7FF,
        "version":  (dpidr >> 12) & 0xF,
        "partno":   (dpidr >> 20) & 0xFF,
        "revision": (dpidr >> 28) & 0xF,
    }


def _decode_ap_idr(idr: int) -> Dict[str, Any]:
    revision = (idr >> 28) & 0xF
    jep_cont = (idr >> 24) & 0xF
    jep_id   = (idr >> 17) & 0x7F
    cls      = (idr >> 13) & 0xF
    variant  = (idr >>  4) & 0xF
    typ      = (idr      ) & 0xF
    designer = (jep_cont << 7) | jep_id
    class_name = AP_CLASS.get(cls, f"class 0x{cls:X}")
    type_name = MEM_AP_TYPE.get(typ, "") if cls == 0x8 else ""
    return {
        "revision":      revision,
        "designer":      designer,
        "designer_name": DESIGNER_LABEL.get(designer, ""),
        "class":         cls,
        "class_name":    class_name,
        "variant":       variant,
        "type":          typ,
        "type_name":     type_name,
    }


def _decode_cpuid(cpuid: int) -> Dict[str, Any]:
    impl  = (cpuid >> 24) & 0xFF
    var   = (cpuid >> 20) & 0xF
    arch  = (cpuid >> 16) & 0xF
    part  = (cpuid >>  4) & 0xFFF
    rev   = (cpuid      ) & 0xF
    return {
        "implementer":      impl,
        "implementer_name": IMPLEMENTER.get(impl, ""),
        "variant":          var,
        "architecture":     arch,
        "architecture_name": ARCH.get(arch, ""),
        "partno":           part,
        "partno_name":      PARTNO.get(part, ""),
        "revision":         rev,
    }


def _decode_dhcsr_flags(dhcsr: int) -> List[str]:
    flags = []
    if dhcsr & (1 <<  0): flags.append("C_DEBUGEN")
    if dhcsr & (1 <<  1): flags.append("C_HALT")
    if dhcsr & (1 <<  2): flags.append("C_STEP")
    if dhcsr & (1 <<  3): flags.append("C_MASKINTS")
    if dhcsr & (1 << 16): flags.append("S_REGRDY")
    if dhcsr & (1 << 17): flags.append("S_HALT")
    if dhcsr & (1 << 18): flags.append("S_SLEEP")
    if dhcsr & (1 << 19): flags.append("S_LOCKUP")
    if dhcsr & (1 << 20): flags.append("S_SDE")            # ARMv8-M Mainline
    if dhcsr & (1 << 24): flags.append("S_RESTART_ST")     # ARMv8-M
    if dhcsr & (1 << 25): flags.append("S_RESET_ST")
    if dhcsr & (1 << 26): flags.append("S_RETIRE_ST")
    return flags


# ───────── Low-level read with logging-on-failure ─────────

def _try_read(dap, addr: int, label: str) -> Optional[int]:
    try:
        status, words = vcmd_mem_read_retry(dap, addr, 1)
        if status != 0 or not words:
            LOG.debug("read %s @ 0x%08X → %s", label, addr, status_name(status))
            return None
        return words[0]
    except Exception as e:
        LOG.debug("read %s @ 0x%08X failed: %s", label, addr, e)
        return None


def _walk_rom_table(dap, base: int, max_entries: int = 32) -> List[Dict[str, Any]]:
    try:
        status, words = vcmd_mem_read_retry(dap, base, min(max_entries, 64))
    except Exception:
        return []
    if status != 0:
        return []
    out = []
    for i, entry in enumerate(words):
        if entry == 0:
            break  # end-of-table marker
        if not (entry & 1):
            continue  # not present
        off = entry & 0xFFFFF000
        if off & 0x80000000:
            off = off - 0x100000000
        abs_addr = (base + off) & 0xFFFFFFFF
        out.append({
            "offset_in_table": i * 4,
            "abs_addr":        abs_addr,
            "label":           WELL_KNOWN_PERIPHS.get(abs_addr, ""),
        })
    return out


# ───────── Collect: pure data gathering, no printing ─────────

def _combo_to_data(dump: Dict[str, Any], probe_info: Dict[str, Any]) -> Dict[str, Any]:
    """Map vcmd_cortex_m_dump() output into the existing info `data` schema."""
    result: Dict[str, Any] = {
        "probe":        probe_info,
        "dp":           None,
        "aps":          [],
        "ahb_ap_sel":   dump.get("ahb_ap_sel"),
        "rom_base":     None,
        "core":         None,
        "debug_state":  None,
        "features":     None,
        "rom_table":    None,
        "errors":       [],
    }

    if dump.get("dpidr") is not None:
        result["dp"] = {"dpidr": dump["dpidr"], **_decode_dpidr(dump["dpidr"])}

    # APs
    for ap in dump["aps"]:
        slot: Dict[str, Any] = {"apsel": ap["apsel"]}
        st = ap["status"]
        if st != 0:
            slot["error"] = f"scan failed: {status_name(st)}"
        elif ap["idr"] is None or ap["idr"] == 0:
            slot["empty"] = True
        else:
            slot["idr"] = ap["idr"]
            slot.update(_decode_ap_idr(ap["idr"]))
            if ap.get("base") not in (None, 0, 0xFFFFFFFF):
                slot["base"] = ap["base"]
                slot["rom_base"] = ap["base"] & 0xFFFFF000
                if ap["apsel"] == dump.get("ahb_ap_sel"):
                    result["rom_base"] = slot["rom_base"]
            elif ap.get("base") is not None:
                slot["base"] = ap["base"]
        result["aps"].append(slot)

    # SCB block → core + debug_state + features
    # Use whatever words we got, even if partial — the chip may stop ACKing
    # partway through (APPROTECT / Trustzone on locked nRF5340). CPUID is at
    # word[0] so even count=1 gives us core identification.
    scb = dump.get("scb") or {}
    scb_count = scb.get("count", 0)
    scb_words = scb.get("words", [])[:scb_count]
    if scb_count >= 1:
        # If we got fewer than 64 words, surface that as an informational
        # error so the user knows why some regs show "—".
        if scb_count < 64 or scb.get("status") != 0:
            result["errors"].append(
                f"SCB bulk read partial: status={status_name(scb.get('status', 0xFF))}, "
                f"got {scb_count}/64 words (chip may have APPROTECT / Trustzone "
                f"limits on the AHB-AP)"
            )

        def w_at(addr: int) -> Optional[int]:
            idx = (addr - 0xE000ED00) // 4
            if 0 <= idx < len(scb_words):
                return scb_words[idx]
            return None

        cpuid = w_at(ADDR_CPUID)
        if cpuid is not None:
            result["core"] = {"cpuid": cpuid, **_decode_cpuid(cpuid),
                              "ap_used": dump.get("ahb_ap_sel")}

        dhcsr = w_at(ADDR_DHCSR)
        result["debug_state"] = {
            "dhcsr":       dhcsr,
            "dhcsr_flags": _decode_dhcsr_flags(dhcsr) if dhcsr is not None else [],
            "demcr":       w_at(ADDR_DEMCR),
            "aircr":       w_at(ADDR_AIRCR),
            "icsr":        w_at(ADDR_ICSR),
            "shcsr":       w_at(ADDR_SHCSR),
            "ccr":         w_at(ADDR_CCR),
        }

        mpu_type = w_at(ADDR_MPU_TYPE)
        cpacr    = w_at(ADDR_CPACR)
        result["features"] = {
            "mpu_type":         mpu_type,
            "mpu_regions":      ((mpu_type >> 8) & 0xFF) if mpu_type is not None else None,
            "cpacr":            cpacr,
            "fpu_cp10":         ((cpacr >> 20) & 0x3) if cpacr is not None else None,
            "fpu_cp11":         ((cpacr >> 22) & 0x3) if cpacr is not None else None,
            "fpu_enabled":      (
                cpacr is not None
                and ((cpacr >> 20) & 0x3) == 0x3
                and ((cpacr >> 22) & 0x3) == 0x3
            ),
            "dwt_ctrl":         None,   # outside SCB block — populated later if needed
            "dwt_comparators":  None,
            "fpb_ctrl":         None,
            "fpb_breakpoints":  None,
        }
    else:
        # No SCB words at all. If we did get APs (especially MEM-APs), this
        # is the signature of an APPROTECT-locked target: DP works, AP IDR
        # readable, but AHB-AP memory access denied. Surface that diagnosis.
        populated = [a for a in result["aps"] if "idr" in a]
        if any(a.get("class") == 0x8 for a in populated):
            result["errors"].append(
                "AHB-AP appears to be locked (DP+AP scan succeeded, but memory "
                "reads fault). On Nordic chips this means UICR.APPROTECT is "
                "enabled or the running firmware disabled debug. Recovery requires "
                "CTRL-AP ERASEALL (mass-erase — destroys firmware) or signed ADAC."
            )
        result["errors"].append(
            f"SCB block: no words read (status={status_name(scb.get('status', 0xFF))})"
        )

    # ROM table from combo response
    rom = dump.get("rom") or {}
    if rom.get("status") == 0 and rom.get("words") and result["rom_base"]:
        entries = []
        for i, entry in enumerate(rom["words"]):
            if entry == 0:
                break
            if not (entry & 1):
                continue
            off = entry & 0xFFFFF000
            if off & 0x80000000:
                off = off - 0x100000000
            abs_addr = (result["rom_base"] + off) & 0xFFFFFFFF
            entries.append({
                "offset_in_table": i * 4,
                "abs_addr":        abs_addr,
                "label":           WELL_KNOWN_PERIPHS.get(abs_addr, ""),
            })
        result["rom_table"] = {"base": result["rom_base"], "entries": entries}

    return result


def _collect(dap, ap_scan_max: int, halt_first: bool, walk_rom: bool,
             dpidr_known: Optional[int] = None) -> Dict[str, Any]:
    result: Dict[str, Any] = {
        "probe": {
            "uid":     dap.get_unique_id(),
            "vendor":  getattr(dap, "vendor_name",  None),
            "product": getattr(dap, "product_name", None),
        },
        "dp":           None,
        "aps":          [],
        "ahb_ap_sel":   None,
        "rom_base":     None,
        "core":         None,
        "debug_state":  None,
        "features":     None,
        "rom_table":    None,
        "errors":       [],
    }

    # DP — prefer pre-captured dpidr (no extra DAP_Transfer needed; avoids
    # a second host-side read that can fail when the chip dozes between
    # the probe step and collection). Vendor cmds below do their own
    # dp_init internally and will re-wake the wire as needed.
    dpidr = dpidr_known
    if dpidr is None:
        from ..probe.pydapaccess.dap_access_api import DAPAccessIntf
        try:
            dpidr = dap.read_reg(DAPAccessIntf.REG.DP_0x0)
        except Exception as e:
            result["errors"].append(f"DPIDR read failed: {e}")
            return result
    result["dp"] = {"dpidr": dpidr, **_decode_dpidr(dpidr)}

    # AP scan — each AP IDR read is its own vendor cmd 0x88 with retry.
    # The vendor cmd's internal dp_init wakes the wire if it dropped.
    for apsel in range(ap_scan_max):
        slot: Dict[str, Any] = {"apsel": apsel}
        try:
            st, idr = vcmd_ap_read_retry(dap, apsel, 0xFC)
        except Exception as e:
            slot["error"] = f"read failed: {e}"
            result["aps"].append(slot)
            continue
        if st != 0:
            slot["error"] = f"scan failed: {status_name(st)}"
            result["aps"].append(slot)
            continue
        if idr == 0:
            slot["empty"] = True
            result["aps"].append(slot)
            continue
        slot["idr"] = idr
        slot.update(_decode_ap_idr(idr))
        # If MEM-AP, capture BASE.
        if slot.get("class") == 0x8:
            try:
                bst, base = vcmd_ap_read_retry(dap, apsel, 0xF8)
                if bst == 0:
                    slot["base"] = base
                    if base not in (0, 0xFFFFFFFF):
                        slot["rom_base"] = base & 0xFFFFF000
            except Exception:
                pass
            if result["ahb_ap_sel"] is None:
                result["ahb_ap_sel"] = apsel
                if slot.get("rom_base"):
                    result["rom_base"] = slot["rom_base"]
        result["aps"].append(slot)

    if result["ahb_ap_sel"] is None:
        result["errors"].append("no MEM-AP found — cannot read Cortex-M core regs")
        return result

    # Optional pre-halt to mitigate sleepy targets.
    if halt_first:
        try:
            hst, _ = vcmd_mem_write(dap, ADDR_DHCSR, [0xA05F0003])
            if hst != 0:
                result["errors"].append(f"halt write failed: {status_name(hst)}")
        except Exception as e:
            result["errors"].append(f"halt write error: {e}")

    # ─── Bulk SCB read — KEY OPTIMISATION ───
    #
    # All of CPUID, ICSR, AIRCR, CCR, SHCSR, CPACR, MPU_TYPE, DHCSR, DEMCR
    # live in the contiguous SCB block 0xE000ED00..0xE000EDFC (64 words).
    # Reading them in ONE vendor cmd (auto-incrementing AHB-AP) instead of
    # 9 separate cmds keeps the DP awake long enough on sleepy targets
    # (nRF5340) to capture everything before the chip drifts back to sleep.
    #
    # Before this: 21 vendor cmds total; sleepy nRF5340 typically died
    # mid-sequence. After: 13 vendor cmds, full data captured reliably.
    SCB_BASE = 0xE000ED00
    SCB_WORDS = 64                       # 0xE000ED00..0xE000EDFC inclusive
    scb_block: Optional[List[int]] = None
    try:
        bst, words = vcmd_mem_read_retry(dap, SCB_BASE, SCB_WORDS)
        if bst == 0 and len(words) == SCB_WORDS:
            scb_block = words
        else:
            result["errors"].append(
                f"bulk SCB read returned status={status_name(bst)}, "
                f"count={len(words)}/{SCB_WORDS}"
            )
    except Exception as e:
        result["errors"].append(f"bulk SCB read failed: {e}")

    def _scb_word(reg_addr: int) -> Optional[int]:
        if scb_block is None:
            return None
        idx = (reg_addr - SCB_BASE) // 4
        if 0 <= idx < len(scb_block):
            return scb_block[idx]
        return None

    # Core (CPUID = SCB+0x00)
    cpuid = _scb_word(ADDR_CPUID)
    if cpuid is not None:
        result["core"] = {"cpuid": cpuid, **_decode_cpuid(cpuid),
                          "ap_used": result["ahb_ap_sel"]}

    # Debug state (DHCSR/DEMCR/AIRCR/ICSR/SHCSR/CCR all in SCB block)
    dhcsr = _scb_word(ADDR_DHCSR)
    debug_state: Dict[str, Any] = {
        "dhcsr":       dhcsr,
        "dhcsr_flags": _decode_dhcsr_flags(dhcsr) if dhcsr is not None else [],
        "demcr":       _scb_word(ADDR_DEMCR),
        "aircr":       _scb_word(ADDR_AIRCR),
        "icsr":        _scb_word(ADDR_ICSR),
        "shcsr":       _scb_word(ADDR_SHCSR),
        "ccr":         _scb_word(ADDR_CCR),
    }
    result["debug_state"] = debug_state

    # Features (MPU TYPE / CPACR also live in SCB block; DWT/FPB are outside)
    mpu_type = _scb_word(ADDR_MPU_TYPE)
    cpacr    = _scb_word(ADDR_CPACR)
    dwt_ctrl = _try_read(dap, ADDR_DWT_CTRL, "DWT CTRL")
    fpb_ctrl = _try_read(dap, ADDR_FPB_CTRL, "FPB CTRL")
    features: Dict[str, Any] = {
        "mpu_type":     mpu_type,
        "mpu_regions":  ((mpu_type >> 8) & 0xFF) if mpu_type is not None else None,
        "cpacr":        cpacr,
        "fpu_cp10":     ((cpacr >> 20) & 0x3) if cpacr is not None else None,
        "fpu_cp11":     ((cpacr >> 22) & 0x3) if cpacr is not None else None,
        "fpu_enabled":  (
            cpacr is not None
            and ((cpacr >> 20) & 0x3) == 0x3
            and ((cpacr >> 22) & 0x3) == 0x3
        ),
        "dwt_ctrl":     dwt_ctrl,
        "dwt_comparators": ((dwt_ctrl >> 28) & 0xF) if dwt_ctrl is not None else None,
        "fpb_ctrl":     fpb_ctrl,
        "fpb_breakpoints": (
            (((fpb_ctrl >> 4) & 0xF) | (((fpb_ctrl >> 12) & 0x7) << 4))
            if fpb_ctrl is not None else None
        ),
    }
    result["features"] = features

    # ROM table
    if walk_rom and result["rom_base"]:
        result["rom_table"] = {
            "base":    result["rom_base"],
            "entries": _walk_rom_table(dap, result["rom_base"]),
        }

    return result


# ───────── Human-readable rendering (string-returning, side-effect-free) ─────────

def _h32(v: Optional[int]) -> str:
    return "—" if v is None else f"0x{v:08X}"


def _format_human(data: Dict[str, Any]) -> str:
    o = io.StringIO()
    p = lambda *args: print(*args, file=o)

    # Probe
    pr = data["probe"]
    p()
    p("── Probe ──")
    p(f"  uid:          {pr['uid']}")
    if pr.get("vendor"):  p(f"  vendor:       {pr['vendor']}")
    if pr.get("product"): p(f"  product:      {pr['product']}")

    # Errors at top (if any)
    if data.get("errors"):
        p()
        p("── Errors ──")
        for e in data["errors"]:
            p(f"  ! {e}")

    # DP
    dp = data.get("dp")
    if dp:
        p()
        p("── DP (Debug Port) ──")
        p(f"  DPIDR:        {_h32(dp['dpidr'])}")
        p(f"    designer    0x{dp['designer']:03X}")
        p(f"    version     {dp['version']}  (DPv{dp['version']})")
        p(f"    partno      0x{dp['partno']:02X}")
        p(f"    revision    {dp['revision']}")

    # APs
    if data.get("aps"):
        p()
        p(f"── APs (scanned 0..{len(data['aps']) - 1}) ──")
        for slot in data["aps"]:
            apsel = slot["apsel"]
            if slot.get("empty"):
                p(f"  AP[{apsel}]  empty")
                continue
            if slot.get("error"):
                p(f"  AP[{apsel}]  {slot['error']}")
                continue
            type_str = f" ({slot['type_name']})" if slot.get("type_name") else ""
            designer_str = f"  {slot['designer_name']}" if slot.get("designer_name") else ""
            p(f"  AP[{apsel}]  IDR={_h32(slot['idr'])}  {slot['class_name']}{type_str}{designer_str}")
            p(f"    revision    {slot['revision']}")
            p(f"    designer    0x{slot['designer']:03X}" +
              (f"  ({slot['designer_name']})" if slot['designer_name'] else ""))
            p(f"    class       0x{slot['class']:X}  ({slot['class_name']})")
            p(f"    variant     {slot['variant']}")
            p(f"    type        0x{slot['type']:X}" +
              (f"  ({slot['type_name']})" if slot.get("type_name") else ""))
            if "base" in slot:
                p(f"    BASE        {_h32(slot['base'])}")

    # Core
    core = data.get("core")
    if core:
        p()
        p(f"── Cortex-M core (via AP[{core['ap_used']}]) ──")
        p(f"  CPUID         {_h32(core['cpuid'])}")
        p(f"    implementer  0x{core['implementer']:02X}  ({core['implementer_name'] or '?'})")
        p(f"    variant      0x{core['variant']:X}")
        p(f"    architecture 0x{core['architecture']:X}  ({core['architecture_name'] or '?'})")
        p(f"    partno       0x{core['partno']:03X}  ({core['partno_name'] or '?'})")
        p(f"    revision     0x{core['revision']:X}")

    # Debug state
    ds = data.get("debug_state")
    if ds:
        p()
        p("── Debug state ──")
        p(f"  DHCSR         {_h32(ds['dhcsr'])}")
        if ds.get("dhcsr_flags"):
            p(f"    flags: {', '.join(ds['dhcsr_flags'])}")
        p(f"  DEMCR         {_h32(ds['demcr'])}")
        p(f"  AIRCR         {_h32(ds['aircr'])}")
        p(f"  ICSR          {_h32(ds['icsr'])}")
        p(f"  SHCSR         {_h32(ds['shcsr'])}")
        p(f"  CCR           {_h32(ds['ccr'])}")

    # Features
    ft = data.get("features")
    if ft:
        p()
        p("── Features ──")
        if ft["mpu_type"] is not None:
            p(f"  MPU TYPE      {_h32(ft['mpu_type'])}  → {ft['mpu_regions']} regions")
        if ft["cpacr"] is not None:
            p(f"  CPACR         {_h32(ft['cpacr'])}  → FPU "
              f"{'enabled' if ft['fpu_enabled'] else 'disabled'} "
              f"(CP10={ft['fpu_cp10']}, CP11={ft['fpu_cp11']})")
        if ft["dwt_ctrl"] is not None:
            p(f"  DWT CTRL      {_h32(ft['dwt_ctrl'])}  → {ft['dwt_comparators']} comparators (watchpoints)")
        if ft["fpb_ctrl"] is not None:
            p(f"  FPB CTRL      {_h32(ft['fpb_ctrl'])}  → {ft['fpb_breakpoints']} code comparators (breakpoints)")

    # ROM table
    rt = data.get("rom_table")
    if rt and rt.get("entries"):
        p()
        p(f"── ROM table @ {_h32(rt['base'])} ──")
        for e in rt["entries"]:
            suffix = f"  ← {e['label']}" if e.get("label") else ""
            p(f"  +0x{e['offset_in_table']:04X}        {_h32(e['abs_addr'])}{suffix}")
    elif data.get("rom_base") and not data.get("rom_table"):
        p()
        p(f"── ROM table @ {_h32(data['rom_base'])} ──")
        p("  (failed to read or empty)")

    return o.getvalue()


# ───────── Subcommand ─────────

class InfoSubcommand(SubcommandBase):
    """Comprehensive Cortex-M debug-port dump."""
    NAMES = ["info"]
    HELP  = "Dump DPIDR, AP IDRs, CPUID, DHCSR, ROM table, and more."
    DEFAULT_LOG_LEVEL = logging.INFO

    @classmethod
    def get_args(cls) -> List[argparse.ArgumentParser]:
        p = argparse.ArgumentParser(description="info", add_help=False)
        g = p.add_argument_group("info")
        g.add_argument("-u", "--uid", "--probe", dest="unique_id")
        g.add_argument("-f", "--frequency", type=int, default=1_000_000)
        g.add_argument("--ap-scan-max", type=int, default=8,
                       help="Scan AP slots 0..N-1 (default 8).")
        g.add_argument("--no-rom-walk", action="store_true",
                       help="Skip ROM table walk.")
        g.add_argument("--halt", action="store_true",
                       help="Halt the CPU before reading regs (DHCSR C_HALT) "
                            "— mitigates sleepy targets (nRF5340 etc.).")
        g.add_argument("--reset-halt", action="store_true",
                       help="Reset + halt at vector first (same DAPAccess session), "
                            "then collect — most reliable on deep-sleeping nRF5340. "
                            "Invokes DAP_VENDOR 0x83 with halt=1 before any reads.")
        g.add_argument("--json", action="store_true",
                       help="Emit JSON ({data: …, text: …}) instead of human text.")
        return [cls.CommonOptions.LOGGING, p]

    def invoke(self) -> int:
        from ..probe.pydapaccess.dap_access_api import DAPAccessIntf
        import time

        try:
            with open_probe_swd(self._args.unique_id, self._args.frequency) as dap:
                # ─── Step 1: probe-of-life — read DPIDR, retry if no ACK.
                # ────────────────────────────────────────────────────
                # Per user 2026-06-19: "if uhdap detect DPIDR then do reset
                # and get all info". So this is the gate — if DPIDR doesn't
                # answer after retries, the chip is in System OFF and no
                # software-only recovery from edevocd is possible.
                dpidr = None
                last_err = None
                for attempt in range(5):
                    try:
                        dpidr = dap.read_reg(DAPAccessIntf.REG.DP_0x0)
                        LOG.info("DPIDR alive: 0x%08X (attempt %d)", dpidr, attempt + 1)
                        break
                    except Exception as e:
                        last_err = e
                        LOG.debug("DPIDR attempt %d failed: %s", attempt + 1, e)
                        # Re-issue SWJ wakeup before retry — sleepy targets
                        # sometimes need multiple line resets to respond.
                        try:
                            dap.swj_sequence(51, 0xffffffffffffff)
                            dap.swj_sequence(16, 0xe79e)
                            dap.swj_sequence(51, 0xffffffffffffff)
                            dap.swj_sequence(8,  0x00)
                        except Exception:
                            pass
                        time.sleep(0.05)

                if dpidr is None:
                    msg = (f"DPIDR unreachable after 5 attempts ({last_err}). "
                           "Target appears to be in deep sleep / System OFF — "
                           "briefly disconnect VTREF or power-cycle the target.")
                    LOG.error(msg)
                    data = {
                        "probe": {
                            "uid":     dap.get_unique_id(),
                            "vendor":  getattr(dap, "vendor_name",  None),
                            "product": getattr(dap, "product_name", None),
                        },
                        "dp":           None,
                        "aps":          [],
                        "ahb_ap_sel":   None,
                        "rom_base":     None,
                        "core":         None,
                        "debug_state":  None,
                        "features":     None,
                        "rom_table":    None,
                        "errors":       [msg],
                    }
                    text = _format_human(data)
                    if self._args.json:
                        print(json.dumps({"data": data, "text": text}, indent=2))
                    else:
                        print(text, end="")
                    return 2

                # ─── Step 2: combo cmd 0x8A — reset+halt + AP scan + bulk
                # SCB + ROM table in ONE firmware call. Eliminates the
                # host-side gap that lets sleepy nRF5340 drift back to
                # System OFF. Falls back to legacy multi-call path if the
                # combo cmd isn't supported (older firmware).
                # ────────────────────────────────────────────────────
                probe_info = {
                    "uid":     dap.get_unique_id(),
                    "vendor":  getattr(dap, "vendor_name",  None),
                    "product": getattr(dap, "product_name", None),
                }
                data = None
                try:
                    LOG.info("invoking combo dump (DAP_VENDOR 0x8A)…")
                    dump = vcmd_cortex_m_dump(
                        dap,
                        reset_halt=self._args.reset_halt,
                        ap_count=self._args.ap_scan_max,
                    )
                    LOG.info("combo dump status=%s, %d APs decoded, SCB status=%s (%d words), ROM %d entries",
                             status_name(dump["status"]),
                             len([a for a in dump["aps"] if a["idr"]]),
                             status_name(dump["scb"]["status"]) if dump["scb"]["status"] is not None else "—",
                             dump["scb"]["count"],
                             dump["rom"]["count"])
                    data = _combo_to_data(dump, probe_info)
                except Exception as e:
                    LOG.warning("combo dump unavailable / failed (%s) — using legacy path", e)

                # If combo got partial SCB, chase critical missing regs via
                # individual single-word vendor cmds. Trustzone-locked regions
                # often block bulk reads but allow single-word with a fresh
                # CSW+TAR setup. Worth trying for DHCSR/DEMCR/MPU/CPACR/DWT/FPB.
                if data is not None:
                    ds = data.get("debug_state") or {}
                    ft = data.get("features") or {}
                    if ds.get("dhcsr") is None:
                        v = _try_read(dap, ADDR_DHCSR, "DHCSR")
                        if v is not None:
                            ds["dhcsr"] = v
                            ds["dhcsr_flags"] = _decode_dhcsr_flags(v)
                            data["debug_state"] = ds
                    if ds.get("demcr") is None:
                        v = _try_read(dap, ADDR_DEMCR, "DEMCR")
                        if v is not None:
                            ds["demcr"] = v
                            data["debug_state"] = ds
                    if ft.get("mpu_type") is None:
                        v = _try_read(dap, ADDR_MPU_TYPE, "MPU TYPE")
                        if v is not None:
                            ft["mpu_type"] = v
                            ft["mpu_regions"] = (v >> 8) & 0xFF
                            data["features"] = ft
                    if ft.get("cpacr") is None:
                        v = _try_read(dap, ADDR_CPACR, "CPACR")
                        if v is not None:
                            ft["cpacr"] = v
                            cp10 = (v >> 20) & 0x3
                            cp11 = (v >> 22) & 0x3
                            ft["fpu_cp10"] = cp10
                            ft["fpu_cp11"] = cp11
                            ft["fpu_enabled"] = cp10 == 0x3 and cp11 == 0x3
                            data["features"] = ft
                    if ft.get("dwt_ctrl") is None:
                        v = _try_read(dap, ADDR_DWT_CTRL, "DWT CTRL")
                        if v is not None:
                            ft["dwt_ctrl"] = v
                            ft["dwt_comparators"] = (v >> 28) & 0xF
                            data["features"] = ft
                    if ft.get("fpb_ctrl") is None:
                        v = _try_read(dap, ADDR_FPB_CTRL, "FPB CTRL")
                        if v is not None:
                            ft["fpb_ctrl"] = v
                            ft["fpb_breakpoints"] = ((v >> 4) & 0xF) | (((v >> 12) & 0x7) << 4)
                            data["features"] = ft

                if data is None:
                    # Fallback: legacy multi-vendor-cmd path.
                    if self._args.reset_halt:
                        try:
                            resp = dap.vendor(VCMD_NRF_SYS_RESET, [1])
                            st = resp[0] if resp else 0xFF
                            if st != 0:
                                LOG.warning("reset+halt: %s — continuing anyway",
                                            status_name(st))
                        except Exception as e:
                            LOG.warning("reset+halt error: %s — continuing", e)
                    data = _collect(
                        dap,
                        ap_scan_max=self._args.ap_scan_max,
                        halt_first=self._args.halt,
                        walk_rom=not self._args.no_rom_walk,
                        dpidr_known=dpidr,
                    )
            text = _format_human(data)
            if self._args.json:
                print(json.dumps({"data": data, "text": text}, indent=2))
            else:
                print(text, end="")
            return 0
        except Exception as e:
            LOG.error("error: %s", e)
            return 1
