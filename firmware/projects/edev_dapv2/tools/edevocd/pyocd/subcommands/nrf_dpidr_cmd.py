# edevocd — edev_dapv2-specific subcommand
# SPDX-License-Identifier: Apache-2.0
#
# `pyocd nrf-dpidr` — raw DPIDR read via DAPAccess. Cheapest possible
# DAP-level connectivity check; mirrors OpenOCD's `Info : SWD DPIDR …`
# log. Bypasses pyocd's CoreSight target plugin so it works even when
# the Tier 1 host-side SWD bug blocks `pyocd commander`.

import argparse
import logging
from typing import List

from .base import SubcommandBase
from ..probe.pydapaccess import DAPAccess
from ..probe.pydapaccess.dap_access_api import DAPAccessIntf

LOG = logging.getLogger(__name__)


class NrfDpidrSubcommand(SubcommandBase):
    """@brief Read DPIDR via raw DAPAccess — DAP-level connectivity check."""

    NAMES = ["nrf-dpidr"]
    HELP = "Read DPIDR via raw DAPAccess (DAP-level connectivity check)."
    DEFAULT_LOG_LEVEL = logging.INFO

    @classmethod
    def get_args(cls) -> List[argparse.ArgumentParser]:
        parser = argparse.ArgumentParser(description="nrf-dpidr", add_help=False)
        g = parser.add_argument_group("nrf-dpidr options")
        g.add_argument("-u", "--uid", "--probe", dest="unique_id",
                       help="Select probe by full or partial unique ID.")
        g.add_argument("-f", "--frequency", type=int, default=1_000_000, metavar="HZ",
                       help="SWD clock in Hz (default 1 MHz).")
        return [cls.CommonOptions.LOGGING, parser]

    def invoke(self) -> int:
        all_daps = DAPAccess.get_connected_devices()
        if not all_daps:
            LOG.error("no CMSIS-DAP probes found")
            return 1

        if self._args.unique_id:
            matches = [d for d in all_daps if self._args.unique_id in d.get_unique_id()]
        else:
            matches = all_daps

        if not matches:
            LOG.error("no probe matched %r", self._args.unique_id)
            return 1
        if len(matches) > 1 and not self._args.unique_id:
            LOG.error("multiple probes; pick one with -u <uid>")
            return 1

        dap = matches[0]
        dap.open()
        try:
            dap.set_clock(self._args.frequency)
            dap.connect(DAPAccessIntf.PORT.SWD)

            # Standard non-dormant SWJ wakeup.
            dap.swj_sequence(51, 0xffffffffffffff)
            dap.swj_sequence(16, 0xe79e)
            dap.swj_sequence(51, 0xffffffffffffff)
            dap.swj_sequence(8,  0x00)

            # DPIDR = read DP register 0x00. This MUST be the first
            # transaction after a line reset (ADIv5 §B4.2.2).
            dpidr = dap.read_reg(DAPAccessIntf.REG.DP_0x0)
            LOG.info("SWD DPIDR 0x%08x", dpidr)

            # Decode the key fields for quick eyeballing.
            designer = (dpidr >> 1) & 0x7FF
            version  = (dpidr >> 12) & 0xF
            partno   = (dpidr >> 20) & 0xFF
            revision = (dpidr >> 28) & 0xF
            LOG.info("  designer=0x%03X version=%d (DPv%d) partno=0x%02X revision=%d",
                     designer, version, version, partno, revision)
            return 0

        except DAPAccessIntf.Error as e:
            LOG.error("DAP error: %s", e)
            return 2
        finally:
            try:
                dap.disconnect()
            except Exception:
                pass
            dap.close()
