#!/usr/bin/env python3
"""Disassemble the Master Assistant POLLER 0xEAD3E0 + HUB 0xEABDB0 to find the
state-bytes / flags they READ (the 'arm-flag' hypothesis): if the poller reads a
controller byte and conditionally advances into the analysis pipeline, we can
write that byte and let Ozone's own poller drive the analysis on its own thread
(no faulting body call). Read-only: disassembles on-disk iZOzonePro.dll only."""
from __future__ import annotations
import sys
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

DLL = r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll"
POLLER = 0xEAD3E0
HUB = 0xEABDB0
PIPELINE = 0xE9FC30
APPLIER = 0xEAD930   # FORBIDDEN to call; we only scan for who calls it


def main():
    pe = pefile.PE(DLL, fast_load=True)
    img_base = pe.OPTIONAL_HEADER.ImageBase
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True

    def disasm(rva, count, label, flags=None):
        data = pe.get_data(rva, 0x900)
        print(f"\n=== {label} @ RVA 0x{rva:x} (VA 0x{img_base + rva:x}) ===")
        n = 0
        for ins in md.disasm(data, img_base + rva):
            n += 1
            note = ""
            # flag byte reads through [reg+imm] or [reg] (state-byte probes)
            if ins.mnemonic in ("cmp", "mov", "movzx", "movsx", "test") and "[" in ins.op_str:
                note = "  <== mem"
            if "byte" in ins.op_str:
                note += " .byte"
            if any(c in ins.op_str for c in ("0x90", "0x98", "0xa0", "0xa8", "0xb0", "0xb8", "0xc0", "0xc8")):
                note += " *flag-offset?"
            if ins.mnemonic == "call":
                note += "  CALL"
            if ins.mnemonic.startswith("j"):
                note += "  ->branch"
            print(f"  0x{ins.address:x}: {ins.mnemonic:8s} {ins.op_str}{note}")
            if n >= count:
                break

    disasm(POLLER, 80, "POLLER 0xEAD3E0 (idle; reads controller state)")
    disasm(HUB, 70, "HUB 0xEABDB0 (calls POLLER + APPLIER)")
    print(f"\n(FORBIDDEN applier 0x{APPLIER:x} never called by us; pipeline root 0x{PIPELINE:x} is the success signal.)")


if __name__ == "__main__":
    sys.exit(main())
