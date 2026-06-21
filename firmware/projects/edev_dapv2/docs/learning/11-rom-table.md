# Chapter 11 — CoreSight ROM Tables & Component Discovery

## Status

🚧 **Stub** — to be filled in. Outline below.

## What you'll learn

1. What a CoreSight ROM table is — the chip's "directory" of debug components.
2. The 4-byte ROM table entry format (offset, format, present) and how to walk it.
3. PIDR / CIDR — the 8-register fingerprint every CoreSight component carries.

## Outline

- **§11.1** The motivation: standard SCS addresses (0xE000ED00 etc.) are fixed by ARM, but chip vendors add custom debug components (Nordic ECC, ITM extensions, etc.). ROM table = lookup directory.
- **§11.2** Reading the ROM table: AP.BASE points at it. First N words = entries, each 32-bit. Entry layout: bit 0 = present, bit 1 = format, bits 12..31 = offset (signed!) in 0x1000 units.
- **§11.3** What's typically in a Cortex-M ROM table: SCS, DWT, FPB, ITM, TPIU/SWO, ETM.
- **§11.4** Identifying a component once you've found its base: read its PIDR (4 32-bit registers at +0xFE0..+0xFEC) + CIDR (4 32-bit at +0xFF0..+0xFFC). The 88-bit composite ID maps to a CoreSight component database.
- **§11.5** Nested ROM tables — a top-level ROM table may point at sub-ROM-tables. Walk recursively (with a max-depth guard).

## Source pointers

- `src/dap/dap_vendor.c::do_edev_cortex_m_dump` — the inline ROM walk in the combo command.
- `tools/edevocd/pyocd/subcommands/edev_info_cmd.py::_walk_rom_table` — host-side walker.
- `tools/edevocd/pyocd/subcommands/edev_info_cmd.py::WELL_KNOWN_PERIPHS` — peripheral address → name map (the lookup we use for the "← SCS" annotations in the report).
