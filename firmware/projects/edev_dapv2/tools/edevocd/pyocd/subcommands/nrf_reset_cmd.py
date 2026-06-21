# edevocd (pyocd fork) — edev_dapv2-specific subcommand
# SPDX-License-Identifier: Apache-2.0
#
# `pyocd nrf-reset` — equivalent of `nrfjprog --reset`. Invokes the
# DAP_VENDOR 0x83 command on the edev_dapv2 probe, which runs the
# AIRCR.SYSRESETREQ sequence entirely on-probe. Works without an
# nRESET wire and bypasses pyocd's coresight target discovery (which
# is currently blocked by the edev_dapv2 Tier 1 SWD bug — see memory
# project_edev_dapv2_tier1_diagnosis_2026_06_19).

import argparse
import logging
import sys
from typing import List

from .base import SubcommandBase
from ..probe.pydapaccess import DAPAccess
from ..probe.pydapaccess.dap_access_api import DAPAccessIntf

LOG = logging.getLogger(__name__)

# Vendor command 0x83 = DAP_VENDOR0 (0x80) + 3.
EDEV_NRF_SYS_RESET_INDEX = 3

# Status bytes returned by the firmware (must mirror src/dap/dap_vendor.c).
_STATUS = {
    0x00: "OK",
    0x01: "PORT_NOT_SWD: probe never had DAP_Connect(SWD) called",
    0x02: "DP_FAIL: DP power-up / sticky-error recovery timed out",
    0x03: "AP_FAIL: AHB-AP rejected memory writes (likely APPROTECT locked — try --debugreset)",
    0x04: "RESET_TIMEOUT: DHCSR.S_RESET_ST never cleared after AIRCR write",
    0x05: "HALT_TIMEOUT: target didn't halt at reset vector (DEMCR.VC_CORERESET may not be honored)",
}


class NrfResetSubcommand(SubcommandBase):
    """@brief `edevocd nrf-reset` — Cortex-M AIRCR sys-reset via edev_dapv2 DAP_VENDOR 0x83.

    Equivalent of `nrfjprog --reset`. Drives the full AIRCR.SYSRESETREQ
    sequence from inside the probe firmware. No target plugin needed,
    no nRESET wire needed.
    """

    NAMES = ["nrf-reset"]
    HELP = "Reset target via on-probe AIRCR.SYSRESETREQ (edev_dapv2 vendor cmd 0x83)."
    DEFAULT_LOG_LEVEL = logging.INFO

    @classmethod
    def get_args(cls) -> List[argparse.ArgumentParser]:
        parser = argparse.ArgumentParser(description="nrf-reset", add_help=False)
        group = parser.add_argument_group("nrf-reset options")
        group.add_argument(
            "-l", "--halt",
            action="store_true",
            help="Halt the core at the reset vector (sets DEMCR.VC_CORERESET).",
        )
        group.add_argument(
            "-u", "--uid", "--probe",
            dest="unique_id", metavar="UID",
            help="Select probe by full or partial unique ID.",
        )
        group.add_argument(
            "-f", "--frequency",
            type=int, default=1_000_000, metavar="HZ",
            help="SWD clock in Hz (default 1 MHz).",
        )
        return [cls.CommonOptions.LOGGING, parser]

    def invoke(self) -> int:
        # Find candidate probes.
        all_daps = DAPAccess.get_connected_devices()
        if not all_daps:
            LOG.error("no CMSIS-DAP probes found")
            return 1

        if self._args.unique_id:
            matches = [d for d in all_daps if self._args.unique_id in d.get_unique_id()]
        else:
            matches = all_daps

        if not matches:
            LOG.error("no probe matched UID %r; available: %s",
                      self._args.unique_id,
                      [d.get_unique_id() for d in all_daps])
            return 1
        if len(matches) > 1 and not self._args.unique_id:
            LOG.error("multiple probes connected; pick one with -u <uid>: %s",
                      [d.get_unique_id() for d in matches])
            return 1

        dap = matches[0]
        LOG.info("opening probe %s", dap.get_unique_id())
        dap.open()
        try:
            # Establish SWD link.
            #
            # Use the raw protocol surface (not pyocd's CoreSight target
            # discovery) — that's the layer Tier 1 blocks. Our vendor
            # command does the rest of the SWD work on-probe.
            dap.set_clock(self._args.frequency)
            dap.connect(DAPAccessIntf.PORT.SWD)

            # Standard non-dormant SWJ wakeup. nRF52/53/91/54L all accept
            # the deprecated 0xE79E switch code. Future: switch to
            # dormant-state wakeup if we see DPv2 targets refusing.
            dap.swj_sequence(51, 0xffffffffffffff)   # line reset (≥50 ones)
            dap.swj_sequence(16, 0xe79e)             # JTAG-to-SWD select
            dap.swj_sequence(51, 0xffffffffffffff)   # line reset again
            dap.swj_sequence(8,  0x00)               # 8 idle cycles

            # Fire the vendor command. Request: [0x83, halt_byte].
            halt = 1 if self._args.halt else 0
            LOG.info("invoking DAP_VENDOR 0x83 (halt=%d)…", halt)
            resp = dap.vendor(EDEV_NRF_SYS_RESET_INDEX, [halt])

            if not resp:
                LOG.error("empty vendor response")
                return 1
            status = resp[0]
            label = _STATUS.get(status, f"UNKNOWN_STATUS(0x{status:02X})")

            if status == 0x00:
                LOG.info("reset OK%s", " (halted at vector)" if self._args.halt else "")
                return 0
            else:
                LOG.error("reset failed: %s", label)
                return 2

        except DAPAccessIntf.Error as e:
            LOG.error("DAP error: %s", e)
            return 3
        finally:
            try:
                dap.disconnect()
            except Exception:
                pass
            dap.close()
