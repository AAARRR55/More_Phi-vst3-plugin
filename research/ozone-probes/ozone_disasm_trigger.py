#!/usr/bin/env python3
"""Static disassembly of the Master Assistant trigger (thunk 0xD572F0 + body
0xD58A20) to see EXACTLY how rcx/rdx/r8 are consumed in the prologue, so we can
construct a valid r8 (or pick a better entry) for the binary-interception call.
Read-only: disassembles the on-disk iZOzonePro.dll only. No process touched."""
from __future__ import annotations
import sys
from pathlib import Path
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

DLL = r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll"
THUNK = 0xD572F0
BODY = 0xD58A20
PIPELINE = 0xE9FC30


def main():
    pe = pefile.PE(DLL, fast_load=True)
    img_base = pe.OPTIONAL_HEADER.ImageBase
    print(f"image_base = 0x{img_base:x}")
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True

    def disasm(rva, count, label):
        off = rva
        data = pe.get_data(rva, 0x600)
        print(f"\n=== {label} @ RVA 0x{rva:x} (VA 0x{img_base + rva:x}) ===")
        n = 0
        for ins in md.disasm(data, img_base + rva):
            n += 1
            # show operands referencing rcx/rdx/r8/r13 early
            regs = {str(op.reg) for op in ins.operands if op.type == 2}  # REG
            note = ""
            if any(r in ins.op_str for r in ("r8", "rdi", "rdx", "rcx", "r13")):
                note = "  <== ctx-use"
            print(f"  0x{ins.address:x}: {ins.mnemonic:8s} {ins.op_str}{note}")
            if n >= count:
                break

    disasm(THUNK, 10, "THUNK (dispatch surface)")
    disasm(BODY, 60, "BODY prologue (first 60 insns)")
    print(f"\n(pipeline root 0x{PIPELINE:x} not disassembled here — see recon.)")


if __name__ == "__main__":
    sys.exit(main())
