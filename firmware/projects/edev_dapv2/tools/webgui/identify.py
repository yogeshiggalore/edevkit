"""identify.py — detect MCU vendor + part from standard ARM + vendor ID registers.

Strategy:
  1. Read ARM CPUID at 0xE000ED00 → core family.
  2. Read top-level ROM table PIDR/CIDR at 0xE00FFFD0..0xE00FFFFC → JEP106 designer.
  3. Walk ROM table entries at 0xE00FF000+ to enumerate CoreSight components.
  4. Based on designer JEP106, run a vendor-specific probe (Nordic FICR,
     STM32 DBGMCU, RP2350 SYSINFO, ...).

All memory reads go through `probe-rs read --chip cortex_m`, which is the
vendor-agnostic generic target. This works regardless of the user's session
`chip` setting, so the panel works *before* you've identified the chip.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Optional

import probers


# ── ARM CPUID part codes ──────────────────────────────────────────────

CPUID_PARTS = {
    0xC20: "Cortex-M0",
    0xC60: "Cortex-M0+",
    0xC23: "Cortex-M3",
    0xC24: "Cortex-M4",
    0xC27: "Cortex-M7",
    0xD20: "Cortex-M23",
    0xD21: "Cortex-M33",
    0xD22: "Cortex-M55",
    0xD23: "Cortex-M85",
}


# ── JEP106 designer codes for silicon vendors ────────────────────────

# JEP106 packed as 11-bit value: (continuation << 7) | identity.
JEP106 = {
    0x23B: "ARM Ltd",                    # bank 4, ID 0x3B
    0x144: "Nordic Semiconductor",       # bank 2, ID 0x44
    0x020: "STMicroelectronics",         # bank 0, ID 0x20
    0x017: "Texas Instruments",          # bank 0, ID 0x17
    0x01F: "Microchip / Atmel",          # bank 0, ID 0x1F
    0x015: "NXP",                        # bank 0, ID 0x15
    0x00E: "Freescale / NXP (Kinetis)",  # bank 0, ID 0x0E
    0x070: "Renesas Electronics",        # bank 0, ID 0x70
    0x493: "Raspberry Pi Ltd",           # bank 9, ID 0x13
    0x0AE: "Espressif Systems",          # bank 1, ID 0x2E
    0x21C: "Silicon Labs",               # bank 4, ID 0x1C
    0x349: "Energy Micro (Silicon Labs)",# bank 6, ID 0x49
    0x021: "Cypress / Infineon",         # bank 0, ID 0x21
    0x06E: "Maxim Integrated",           # bank 0, ID 0x6E
    0x751: "GigaDevice",                 # bank 14, ID 0x51
}


# ── Data shapes ───────────────────────────────────────────────────────

@dataclass
class RomEntry:
    address: str               # "0xE00FF000"
    designer: int              # 11-bit JEP106
    designer_name: str
    part: int                  # 12-bit part number
    component_class: int       # CIDR1 bits 7:4 (1=ROM table, 9=CoreSight, etc.)


@dataclass
class IdentifyResult:
    success: bool
    error: str = ""
    cpuid: Optional[int] = None
    core: str = ""
    core_revision: str = ""
    implementer: str = ""
    rom_designer_jep106: Optional[int] = None
    rom_designer_name: str = ""
    rom_components: list = field(default_factory=list)
    vendor: str = ""
    part_name: str = ""
    part_number: str = ""
    part_variant: str = ""
    flash_kb: Optional[int] = None
    ram_kb: Optional[int] = None
    package: str = ""
    raw_regs: dict = field(default_factory=dict)
    notes: list = field(default_factory=list)


# ── Low-level helpers ─────────────────────────────────────────────────

# probe-rs generic-target chip names, tried in priority order until one
# accepts the connection. Caching the first hit avoids the ~2×fallback
# latency on every subsequent read in the same identify run.
GENERIC_CHIPS = ["Cortex-M4", "Cortex-M33", "Cortex-M7", "Cortex-M0"]


async def _read_words(probe_args, address, count, timeout=20.0):
    """Generic Cortex-M read returning a list of N words, or None on failure.

    Tries probe-rs generic chip names in order; once a chip works it sticks
    in `probe_args["chip"]` for the rest of this identify run.
    """
    chips = [probe_args["chip"]] if probe_args.get("chip") else GENERIC_CHIPS
    for chip in chips:
        code, data, err = await probers.read_words(
            chip=chip,
            speed_khz=probe_args["speed_khz"],
            address=address,
            word_count=count,
            serial=probe_args.get("serial"),
            vid_pid=probe_args.get("vid_pid"),
            timeout=timeout,
        )
        if code == 0 and data and len(data) == count * 4:
            probe_args["chip"] = chip
            return list(struct.unpack(f"<{count}I", data))
    return None


def _parse_cpuid(cpuid: int):
    impl = (cpuid >> 24) & 0xFF
    variant = (cpuid >> 20) & 0xF
    partno = (cpuid >> 4) & 0xFFF
    revision = cpuid & 0xF
    implementer = "ARM Ltd" if impl == 0x41 else f"0x{impl:02x}"
    core = CPUID_PARTS.get(partno, f"unknown Cortex-M (partno=0x{partno:03x})")
    return core, f"r{variant}p{revision}", implementer


def _parse_pidr_cidr(words):
    """Decode 12 words from BASE+0xFD0..0xFFC into (designer, part, class).

    Layout (ADIv5):
      PIDR4=+0xFD0, PIDR5=+0xFD4, PIDR6=+0xFD8, PIDR7=+0xFDC,
      PIDR0=+0xFE0, PIDR1=+0xFE4, PIDR2=+0xFE8, PIDR3=+0xFEC,
      CIDR0=+0xFF0, CIDR1=+0xFF4, CIDR2=+0xFF8, CIDR3=+0xFFC.
    """
    if len(words) < 12:
        return None
    pidr4 = words[0]
    pidr0, pidr1, pidr2, _pidr3 = words[4:8]
    cidr0, cidr1, cidr2, cidr3 = words[8:12]
    # CIDR preamble: 0x0D, _, 0x05, 0xB1
    if (cidr0 & 0xFF) != 0x0D or (cidr2 & 0xFF) != 0x05 or (cidr3 & 0xFF) != 0xB1:
        return None
    part = (pidr0 & 0xFF) | ((pidr1 & 0x0F) << 8)
    des_lo = (pidr1 >> 4) & 0x0F
    des_mid = pidr2 & 0x07
    des_hi = pidr4 & 0x0F
    designer = (des_hi << 7) | (des_mid << 4) | des_lo
    component_class = (cidr1 >> 4) & 0x0F
    return designer, part, component_class


# ── Vendor-specific probes ────────────────────────────────────────────

async def _nordic_probe(probe_args, res):
    """nRF52/53 FICR.INFO at 0x10000100 (5 words)."""
    words = await _read_words(probe_args, 0x10000100, 5)
    if not words:
        res.notes.append("Nordic FICR.INFO read failed")
        return
    part_id, variant_raw, package, ram, flash = words
    variant = "".join(
        chr(b) if 32 <= b < 127 else "?"
        for b in variant_raw.to_bytes(4, "big")
    )

    # Known Nordic PACKAGE codes (FICR.INFO.PACKAGE).
    PACKAGES = {
        0x2000: "QFAA", 0x2001: "CHAA", 0x2002: "CIAA",
        0x2003: "QIAA", 0x2004: "CKAA", 0x2005: "CLAA",
    }

    res.vendor = "nordic"
    res.part_number = f"0x{part_id:X}"
    res.part_name = (
        f"nRF{part_id:X}" if 0x50000 <= part_id < 0x60000 else f"0x{part_id:X}"
    )
    res.part_variant = variant
    res.flash_kb = flash if flash != 0xFFFFFFFF else None
    res.ram_kb = ram if ram != 0xFFFFFFFF else None
    res.package = PACKAGES.get(package, f"0x{package:08X}")
    res.raw_regs.update({
        "FICR.PART    (0x10000100)": f"0x{part_id:08X}",
        "FICR.VARIANT (0x10000104)": f'0x{variant_raw:08X}  →  "{variant}"',
        "FICR.PACKAGE (0x10000108)": f"0x{package:08X}",
        "FICR.RAM_KB  (0x1000010C)": str(ram)   if ram   != 0xFFFFFFFF else "0xFFFFFFFF",
        "FICR.FLASH_KB(0x10000110)": str(flash) if flash != 0xFFFFFFFF else "0xFFFFFFFF",
    })


async def _stm32_probe(probe_args, res):
    """ST DBGMCU_IDCODE — 0xE0042000 (F0/F1/F2/F3/F4/L0/L1/L4) or 0xE0044000 (F7/H7)."""
    DEV_NAMES = {
        0x411: "STM32F2 (F205/F215/F207/F217)",
        0x413: "STM32F4 (F405/F415/F407/F417)",
        0x419: "STM32F4 (F42x/F43x)",
        0x431: "STM32F411",
        0x441: "STM32F412",
        0x423: "STM32F401 (B/C)",
        0x433: "STM32F401 (D/E)",
        0x451: "STM32F7 (F76x/F77x)",
        0x450: "STM32H7 (H742/H743/H753)",
        0x480: "STM32H7 (H723/H733/H725/H735/H730)",
        0x415: "STM32L4 (L4x6)",
        0x435: "STM32L4 (L43x/L44x)",
    }
    res.vendor = "stmicro"
    for addr in (0xE0042000, 0xE0044000):
        words = await _read_words(probe_args, addr, 1)
        if not words or words[0] in (0, 0xFFFFFFFF):
            continue
        val = words[0]
        dev = val & 0xFFF
        rev = (val >> 16) & 0xFFFF
        res.raw_regs[f"DBGMCU_IDCODE (0x{addr:08X})"] = (
            f"0x{val:08X}  (DEV=0x{dev:03X} REV=0x{rev:04X})"
        )
        if dev in DEV_NAMES:
            res.part_name = DEV_NAMES[dev]
            res.part_number = f"0x{dev:03X}"
            return


async def _rp2_probe(probe_args, res):
    """RP2040 / RP2350 SYSINFO at 0x40000000."""
    words = await _read_words(probe_args, 0x40000000, 2)
    if not words:
        return
    chip_id, platform = words
    part = (chip_id >> 12) & 0xFFFF
    PARTS = {0x0002: "RP2040", 0x0004: "RP2350"}
    res.vendor = "raspberry_pi"
    res.part_number = f"0x{part:04X}"
    res.part_name = PARTS.get(part, f"0x{part:04X}")
    res.raw_regs.update({
        "SYSINFO.CHIP_ID  (0x40000000)": f"0x{chip_id:08X}",
        "SYSINFO.PLATFORM (0x40000004)": f"0x{platform:08X}",
    })


async def _microchip_sam_probe(probe_args, res):
    """Microchip SAM DSU.DID at 0x41002018.
       Layout (SAMD/SAML/SAMC/SAME):
         31:28 PROCESSOR  (1=M0+, 6=M4)
         27:23 FAMILY     (0=D, 1=L, 2=C, 3=R, 4=E, 5=S, 6=N)
         22:19 SERIES     (0=10, 1=11, 2=20, 3=21, 4=50/51, ...)
         15:8  DEVSEL     (specific die)
         7:0   REVISION
    """
    words = await _read_words(probe_args, 0x41002018, 1)
    if not words:
        res.notes.append("Microchip DSU.DID read failed (chip may not have DSU)")
        return
    did = words[0]
    if did in (0, 0xFFFFFFFF):
        return
    proc   = (did >> 28) & 0xF
    family = (did >> 23) & 0x1F
    series = (did >> 19) & 0xF
    devsel = (did >> 8)  & 0xFF
    rev    =  did        & 0xFF

    FAM = {0:"D", 1:"L", 2:"C", 3:"R", 4:"E", 5:"S", 6:"N"}
    SER = {0:"10", 1:"11", 2:"20", 3:"21", 4:"51", 5:"22"}
    PROC= {1:"Cortex-M0+", 6:"Cortex-M4"}

    fam = FAM.get(family, f"?{family}")
    ser = SER.get(series, f"?{series}")
    res.vendor = "microchip"
    res.part_name = f"SAM{fam}{ser} (devsel=0x{devsel:02X})"
    res.part_number = f"0x{(did>>8) & 0xFFFFFF:06X}"
    res.part_variant = chr(ord("A") + rev) if rev < 26 else f"rev0x{rev:02X}"
    res.raw_regs["DSU.DID (0x41002018)"] = (
        f"0x{did:08X}  (PROC={PROC.get(proc,'?')} FAMILY={fam} SERIES={ser} "
        f"DEVSEL=0x{devsel:02X} REV=0x{rev:02X})"
    )


async def _ti_probe(probe_args, res):
    """TI: try MSP432 TLV first (0x00201004), then Tiva DID0/DID1 (0x400FE000)."""
    res.vendor = "ti"

    # MSP432P4xx TLV.DEVICE_ID
    w = await _read_words(probe_args, 0x00201004, 2)
    if w and w[0] not in (0, 0xFFFFFFFF):
        dev_id, hw_rev = w[0], w[1]
        dev = (dev_id >> 16) & 0xFFFF
        MSP432_PARTS = {
            0xA000: "MSP432P401R", 0xA001: "MSP432P401M",
            0xA002: "MSP432P411V", 0xA003: "MSP432P411R",
            0xA004: "MSP432P401V",
        }
        res.raw_regs["TLV.DEVICE_ID (0x00201004)"] = f"0x{dev_id:08X}"
        res.raw_regs["TLV.HW_REV    (0x00201008)"] = f"0x{hw_rev:08X}"
        if dev in MSP432_PARTS:
            res.part_name = MSP432_PARTS[dev]
            res.part_number = f"0x{dev:04X}"
            return

    # Tiva TM4C SYSCTL_DID0/DID1
    w = await _read_words(probe_args, 0x400FE000, 2)
    if not w:
        return
    did0, did1 = w
    if did1 in (0, 0xFFFFFFFF):
        return
    ver    = (did1 >> 28) & 0xF
    family = (did1 >> 24) & 0xF
    partno = (did1 >> 16) & 0xFF
    pinct  = (did1 >> 13) & 0x7
    temp   = (did1 >>  5) & 0x7
    pkg    = (did1 >>  3) & 0x3
    rohs   = (did1 >>  2) & 0x1
    qual   =  did1        & 0x3
    res.raw_regs["SYSCTL_DID0 (0x400FE000)"] = f"0x{did0:08X}"
    res.raw_regs["SYSCTL_DID1 (0x400FE004)"] = (
        f"0x{did1:08X}  (VER={ver} FAMILY={family} PARTNO=0x{partno:02X})"
    )
    res.part_name = f"Tiva TM4C (partno=0x{partno:02X})"
    res.part_number = f"0x{partno:02X}"


async def _kinetis_probe(probe_args, res):
    """NXP Kinetis SIM_SDID at 0x40048024."""
    res.vendor = "nxp_kinetis"
    w = await _read_words(probe_args, 0x40048024, 1)
    if not w or w[0] in (0, 0xFFFFFFFF):
        res.notes.append("Kinetis SIM_SDID read failed")
        return
    sdid = w[0]
    family_id = (sdid >> 28) & 0xF
    subfam_id = (sdid >> 24) & 0xF
    series_id = (sdid >> 20) & 0xF
    rev_id    = (sdid >> 12) & 0xF
    die_id    = (sdid >> 7)  & 0x1F
    pin_id    =  sdid        & 0xF

    FAM = {0:"KL0", 1:"KL1/KL2", 2:"K", 3:"KL2x", 4:"KV",
           5:"KE", 6:"KW", 7:"KS"}
    PIN = {2:"32", 4:"48", 5:"64", 6:"80/81", 7:"100", 8:"121", 9:"144"}
    fam = FAM.get(family_id, f"family={family_id}")
    pin = PIN.get(pin_id, f"pin={pin_id}")
    res.part_name = f"Kinetis {fam}x{subfam_id} ({pin}-pin)"
    res.part_number = f"FAM={family_id} SUB={subfam_id} SER={series_id} DIE=0x{die_id:02X}"
    res.part_variant = f"rev{rev_id}"
    res.raw_regs["SIM_SDID (0x40048024)"] = (
        f"0x{sdid:08X}  (FAM={family_id} SUB={subfam_id} SER={series_id} "
        f"REV={rev_id} DIE=0x{die_id:02X} PIN={pin_id})"
    )


async def _silabs_probe(probe_args, res):
    """Silicon Labs DEVINFO for EFM32 / EFR32 (Gecko family).

    The classic Gecko DEVINFO sits at 0x0FE081B0 + offsets. PART register
    at 0x0FE081FC encodes family + device number. Newer Series 2 chips
    have moved DEVINFO around — we only attempt the classic layout here.
    """
    res.vendor = "silabs"
    w = await _read_words(probe_args, 0x0FE081E4, 8)  # MEMINFO + PART
    if not w:
        res.notes.append("Silabs DEVINFO read failed")
        return
    meminfo  = w[0]   # @0x0FE081E4
    part     = w[6]   # @0x0FE081FC

    if part in (0, 0xFFFFFFFF):
        return

    family_id   = (part >> 24) & 0xFF
    device_num  = (part >> 16) & 0xFF    # really 16-bit device num for some families
    prod_rev    =  part        & 0xFF

    # Classic EFM32 Gecko family IDs. EFR32 Series-1/2 use a different
    # DEVINFO layout — they need their own probe; until then we just
    # show the raw PART register so the user can decode it manually.
    FAMILY = {
        0x47: "EFM32G",  0x48: "EFM32GG", 0x49: "EFM32TG",
        0x4A: "EFM32LG", 0x4B: "EFM32WG", 0x4D: "EFM32HG",
        0x51: "EFM32ZG",
    }
    fam = FAMILY.get(family_id, f"family=0x{family_id:02X}")

    flash_kb = meminfo & 0xFFFF
    sram_kb  = (meminfo >> 16) & 0xFFFF
    if flash_kb and flash_kb != 0xFFFF:
        res.flash_kb = flash_kb
    if sram_kb and sram_kb != 0xFFFF:
        res.ram_kb = sram_kb

    res.part_name = f"{fam} dev={device_num}"
    res.part_number = f"0x{part:08X}"
    res.part_variant = f"rev0x{prod_rev:02X}"
    res.raw_regs["DEVINFO.PART    (0x0FE081FC)"] = f"0x{part:08X}"
    res.raw_regs["DEVINFO.MEMINFO (0x0FE081E4)"] = (
        f"0x{meminfo:08X}  (FLASH={flash_kb} KB  SRAM={sram_kb} KB)"
    )


VENDOR_PROBES = {
    0x144: _nordic_probe,
    0x020: _stm32_probe,
    0x493: _rp2_probe,
    0x01F: _microchip_sam_probe,   # Microchip / Atmel SAM
    0x017: _ti_probe,               # TI MSP432, Tiva
    0x00E: _kinetis_probe,          # Freescale / NXP Kinetis
    0x015: _kinetis_probe,          # NXP variants under newer designer
    0x21C: _silabs_probe,           # Silicon Labs (post-EnergyMicro)
    0x349: _silabs_probe,           # Energy Micro (legacy EFM32)
}


# ── ROM table walk ────────────────────────────────────────────────────

async def _walk_rom_table(probe_args, base, res):
    """Read top-level PIDR/CIDR then enumerate non-empty entries."""
    # Top-level designer
    top = await _read_words(probe_args, base + 0xFD0, 12)
    if top:
        parsed = _parse_pidr_cidr(top)
        if parsed:
            designer, part, klass = parsed
            res.rom_designer_jep106 = designer
            res.rom_designer_name = JEP106.get(
                designer, f"unknown (0x{designer:03X})"
            )
            res.rom_components.append(RomEntry(
                address=f"0x{base:08X}",
                designer=designer,
                designer_name=res.rom_designer_name,
                part=part,
                component_class=klass,
            ))

    # Entries: up to 32 of them, stop at zero terminator
    entries = await _read_words(probe_args, base, 32)
    if not entries:
        return
    for entry in entries:
        if entry == 0:
            break
        if not (entry & 0x1):           # PRESENT bit
            continue
        # bits 31:12 are signed offset in 4 KB units
        off_raw = (entry >> 12) & 0xFFFFF
        if off_raw & 0x80000:
            off_raw -= 0x100000
        sub_addr = (base + off_raw * 0x1000) & 0xFFFFFFFF
        sub = await _read_words(probe_args, sub_addr + 0xFD0, 12)
        if not sub:
            continue
        parsed = _parse_pidr_cidr(sub)
        if not parsed:
            continue
        d, p, k = parsed
        res.rom_components.append(RomEntry(
            address=f"0x{sub_addr:08X}",
            designer=d,
            designer_name=JEP106.get(d, f"unknown (0x{d:03X})"),
            part=p,
            component_class=k,
        ))


# ── Public entry ──────────────────────────────────────────────────────

async def identify(*, serial, vid_pid, speed_khz):
    res = IdentifyResult(success=False)
    probe_args = {
        "serial": serial,
        "vid_pid": vid_pid,
        "speed_khz": speed_khz,
        "chip": None,   # filled in on first successful read
    }

    # 1. CPUID
    words = await _read_words(probe_args, 0xE000ED00, 1)
    if not words:
        res.error = (
            "CPUID read failed — check probe wiring, target power, or APPROTECT lock"
        )
        return res
    cpuid = words[0]
    res.cpuid = cpuid
    res.core, res.core_revision, res.implementer = _parse_cpuid(cpuid)

    # 2. ROM table walk
    await _walk_rom_table(probe_args, 0xE00FF000, res)

    # 3. Vendor-specific probe based on top-level ROM table designer
    probe_fn = VENDOR_PROBES.get(res.rom_designer_jep106)
    if probe_fn:
        await probe_fn(probe_args, res)
    elif res.rom_designer_jep106 is not None:
        res.notes.append(
            f"No vendor probe wired for designer 0x{res.rom_designer_jep106:03X}"
            f" ({res.rom_designer_name}) — extend identify.VENDOR_PROBES"
        )

    res.success = True
    return res
