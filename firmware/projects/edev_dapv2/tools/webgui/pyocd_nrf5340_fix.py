"""pyocd user target fix for nRF5340 — Net core flash sector_size=0 bug.

Nordic's CMSIS pack (nrf53xx_network.flm) declares IROM2 (Net flash at
0x01000000) with sector_size=0 in its FlashDevice descriptor. pyocd's flash
loader divides by sector_size when laying out erase plans, hitting
ZeroDivisionError before any actual SWD operation.

Patches: after a session is opened, locate any FlashRegion whose
sector_size is 0 and overwrite _attributes['sector_size'] to a sane value
(4 KB — matches App side and the Nordic spec for nRF5340 Net flash erase
page).
"""
import logging
from pyocd.core.memory_map import FlashRegion

log = logging.getLogger(__name__)


def fixup_session(session):
    """Patch sector_size on any FlashRegion that has sector_size == 0.

    Call AFTER session.open() and BEFORE any flash operation. Idempotent.
    Returns list of (name, start, new_sector_size) tuples for what was fixed.
    """
    fixed = []
    for r in session.target.get_memory_map():
        if not isinstance(r, FlashRegion):
            continue
        if r._attributes.get('sector_size', 0) in (0, None):
            page = r._attributes.get('page_size', 0) or 0x1000
            new_size = page if page >= 0x1000 else 0x1000
            r._attributes['sector_size'] = new_size
            fixed.append((r.name, r.start, new_size))
            log.warning("Patched %s @ 0x%08x sector_size=0 -> 0x%x",
                        r.name, r.start, new_size)
    return fixed
