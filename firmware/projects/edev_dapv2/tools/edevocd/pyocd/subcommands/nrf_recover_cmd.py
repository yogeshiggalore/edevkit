# edevocd (pyocd fork) — edev_dapv2-specific subcommand
# SPDX-License-Identifier: Apache-2.0
#
# `pyocd nrf-recover` — equivalent of `nrfjprog --recover`. Issues the
# Nordic CTRL-AP ERASEALL sequence to mass-erase the chip and clear
# APPROTECT lockout. Destructive: erases all flash + UICR + RAM.
#
# Sequence (per Nordic CTRL-AP docs):
#   1. Assert CTRL-AP.RESET = 1                (hold chip in reset)
#   2. Write 1 to CTRL-AP.ERASEALL (offset 0x004)
#   3. Poll CTRL-AP.ERASEALLSTATUS (offset 0x008) until 0 (ready)
#   4. Write 0 to CTRL-AP.RESET                (release reset)
#
# CTRL-AP slot depends on family:
#   nRF52*       → AP[1]
#   nRF5340-App  → AP[2]
#   nRF5340-Net  → AP[3]
#   nRF91*       → AP[4]
#   nRF54L*      → AP[2]

import argparse
import logging
import time
from typing import List

from .base import SubcommandBase
from ._edev_helpers import (
    open_probe_swd, vcmd_ap_write, vcmd_ap_read,
    detect_chip_family, status_name,
)

LOG = logging.getLogger(__name__)

# CTRL-AP register offsets (constant across families).
CTRL_AP_RESET           = 0x000
CTRL_AP_ERASEALL        = 0x004
CTRL_AP_ERASEALLSTATUS  = 0x008

NORDIC_CTRL_AP_SLOT = {
    "nRF52840":     1,
    "nRF52833":     1,
    "nRF52832":     1,
    "nRF5340-App":  2,
    "nRF5340-Net":  3,
    "nRF91xx":      4,
    "nRF54L15":     2,
}


class NrfRecoverSubcommand(SubcommandBase):
    """@brief `edevocd nrf-recover` — Nordic CTRL-AP mass-erase recovery.

    Equivalent of `nrfjprog --recover`. DESTRUCTIVE: erases all flash
    (App core + Net core on multi-core), UICR, and RAM, then releases
    APPROTECT lockout. The chip becomes blank — you must reflash
    desired firmware before next reset to keep it accessible.
    """

    NAMES = ["nrf-recover"]
    HELP = "Mass-erase chip via Nordic CTRL-AP ERASEALL (UNLOCKS but ERASES)."
    DEFAULT_LOG_LEVEL = logging.INFO

    @classmethod
    def get_args(cls) -> List[argparse.ArgumentParser]:
        p = argparse.ArgumentParser(description="nrf-recover", add_help=False)
        g = p.add_argument_group("nrf-recover options")
        g.add_argument("--family", metavar="NAME",
                       help="Override detected family (e.g. nRF5340-App, nRF5340-Net). "
                            "Skips auto-detect.")
        g.add_argument("--apsel", type=int, default=None,
                       help="Override CTRL-AP slot (default: from family).")
        g.add_argument("--both-cores", action="store_true",
                       help="On nRF5340, recover BOTH App and Net cores "
                            "(via AP[2] then AP[3]). Required to fully unlock "
                            "a multi-core chip.")
        g.add_argument("--timeout", type=float, default=30.0, metavar="SEC",
                       help="Max time to wait for ERASEALL completion (default 30 s).")
        g.add_argument("-u", "--uid", "--probe", dest="unique_id")
        g.add_argument("-f", "--frequency", type=int, default=1_000_000)
        g.add_argument("--yes", action="store_true",
                       help="Skip the destructive-confirmation prompt.")
        return [cls.CommonOptions.LOGGING, p]

    def _eraseall_one(self, dap, apsel: int, timeout: float) -> bool:
        """Run ERASEALL on a single CTRL-AP. Returns True on success."""
        LOG.info("AP[%d]: asserting CTRL-AP.RESET=1 (hold chip in reset)", apsel)
        vcmd_ap_write(dap, apsel, CTRL_AP_RESET, 1)
        time.sleep(0.02)

        LOG.info("AP[%d]: writing 1 to CTRL-AP.ERASEALL (0x004) — starting mass erase", apsel)
        vcmd_ap_write(dap, apsel, CTRL_AP_ERASEALL, 1)

        # Poll ERASEALLSTATUS until ready. Typical: ~200ms. Worst case: 30s.
        t_start = time.monotonic()
        last_log = 0.0
        while True:
            elapsed = time.monotonic() - t_start
            if elapsed > timeout:
                LOG.error("AP[%d]: ERASEALLSTATUS poll timed out after %.1f s",
                          apsel, elapsed)
                return False
            try:
                st, value = vcmd_ap_read(dap, apsel, CTRL_AP_ERASEALLSTATUS)
            except Exception as e:
                LOG.warning("AP[%d]: poll exception (%s) — retrying", apsel, e)
                time.sleep(0.05)
                continue
            if st == 0 and value == 0:
                LOG.info("AP[%d]: ERASEALL completed in %.2f s", apsel, elapsed)
                break
            if elapsed - last_log > 1.0:
                LOG.info("  …still erasing (status=0x%X, %.1fs elapsed)", value, elapsed)
                last_log = elapsed
            time.sleep(0.05)

        LOG.info("AP[%d]: releasing CTRL-AP.RESET=0", apsel)
        vcmd_ap_write(dap, apsel, CTRL_AP_RESET, 0)
        return True

    def invoke(self) -> int:
        if not self._args.yes:
            LOG.warning("nrf-recover is DESTRUCTIVE: erases all flash + UICR + RAM.")
            LOG.warning("Pass --yes to proceed.")
            return 2

        try:
            with open_probe_swd(self._args.unique_id, self._args.frequency) as dap:
                # Determine which AP(s) to target.
                ap_list = []
                if self._args.apsel is not None:
                    ap_list = [self._args.apsel]
                else:
                    family = self._args.family
                    if family is None:
                        info = detect_chip_family(dap)
                        if not info["detected"]:
                            LOG.error("chip family not detected — pass --family or --apsel")
                            return 1
                        family = info["family"]
                        LOG.info("detected family: %s (DPIDR 0x%08X)",
                                 family, info["dpidr"])
                    apsel = NORDIC_CTRL_AP_SLOT.get(family)
                    if apsel is None:
                        LOG.error("no CTRL-AP slot known for family %r", family)
                        return 1
                    ap_list = [apsel]
                    if self._args.both_cores and family == "nRF5340-App":
                        ap_list.append(3)   # Net core CTRL-AP
                        LOG.info("both-cores: will also erase Net core via AP[3]")

                all_ok = True
                for apsel in ap_list:
                    if not self._eraseall_one(dap, apsel, self._args.timeout):
                        all_ok = False
                        LOG.error("AP[%d]: ERASEALL failed", apsel)

                if all_ok:
                    LOG.info("recover OK — chip is mass-erased and unlocked. "
                             "Flash desired firmware before next reset.")
                    return 0
                return 2

        except Exception as e:
            LOG.error("recover failed: %s", e)
            return 3
