# `edevocd mem-read` and `edevocd mem-write` — raw memory access via edev_dapv2.
# SPDX-License-Identifier: Apache-2.0

import argparse
import logging
import sys
from typing import List

from .base import SubcommandBase
from ._edev_helpers import (
    open_probe_swd, vcmd_mem_read, vcmd_mem_write,
    parse_int_base_0, status_name,
)

LOG = logging.getLogger(__name__)


class MemReadSubcommand(SubcommandBase):
    """Read N 32-bit words from target memory via DAP_VENDOR 0x86."""
    NAMES = ["mem-read"]
    HELP  = "Read N 32-bit words from target memory (via edev_dapv2 vendor 0x86)."
    DEFAULT_LOG_LEVEL = logging.WARNING

    @classmethod
    def get_args(cls) -> List[argparse.ArgumentParser]:
        p = argparse.ArgumentParser(description="mem-read", add_help=False)
        g = p.add_argument_group("mem-read")
        g.add_argument("addr", type=parse_int_base_0,
                       help="Address (hex 0x…, dec, or 0b…). Must be 4-byte aligned.")
        g.add_argument("count", type=int, nargs="?", default=1,
                       help="Number of 32-bit words to read (default 1, max 64).")
        g.add_argument("-u", "--uid", "--probe", dest="unique_id")
        g.add_argument("-f", "--frequency", type=int, default=1_000_000)
        return [cls.CommonOptions.LOGGING, p]

    def invoke(self) -> int:
        try:
            with open_probe_swd(self._args.unique_id, self._args.frequency) as dap:
                addr = self._args.addr & 0xFFFFFFFF
                count = max(1, min(64, self._args.count))
                status, words = vcmd_mem_read(dap, addr, count)
                if status != 0:
                    LOG.error("mem-read failed: %s", status_name(status))
                    return 2
                # Hex dump: 16 bytes (4 words) per line.
                for i in range(0, len(words), 4):
                    chunk = words[i:i+4]
                    hex_part = " ".join(f"{w:08x}" for w in chunk)
                    print(f"  0x{addr + i*4:08X}:  {hex_part}")
                return 0
        except Exception as e:
            LOG.error("error: %s", e)
            return 1


class MemWriteSubcommand(SubcommandBase):
    """Write N 32-bit words to target memory via DAP_VENDOR 0x87."""
    NAMES = ["mem-write"]
    HELP  = "Write 32-bit words to target memory (via edev_dapv2 vendor 0x87)."
    DEFAULT_LOG_LEVEL = logging.WARNING

    @classmethod
    def get_args(cls) -> List[argparse.ArgumentParser]:
        p = argparse.ArgumentParser(description="mem-write", add_help=False)
        g = p.add_argument_group("mem-write")
        g.add_argument("addr", type=parse_int_base_0,
                       help="Address (hex/dec/bin). Must be 4-byte aligned.")
        g.add_argument("words", nargs="+", type=parse_int_base_0,
                       help="One or more 32-bit values to write.")
        g.add_argument("-u", "--uid", "--probe", dest="unique_id")
        g.add_argument("-f", "--frequency", type=int, default=1_000_000)
        return [cls.CommonOptions.LOGGING, p]

    def invoke(self) -> int:
        try:
            with open_probe_swd(self._args.unique_id, self._args.frequency) as dap:
                addr = self._args.addr & 0xFFFFFFFF
                words = [w & 0xFFFFFFFF for w in self._args.words[:64]]
                status, actual = vcmd_mem_write(dap, addr, words)
                if status != 0:
                    LOG.error("mem-write failed after %d words: %s",
                              actual, status_name(status))
                    return 2
                print(f"  wrote {actual} word(s) starting at 0x{addr:08X}")
                return 0
        except Exception as e:
            LOG.error("error: %s", e)
            return 1
