#!/usr/bin/env python3
"""Resolve the controller vtable (RVA 0x28b8878) and dump its slots, especially
+0xc0 (the virtual method the POLLER calls every tick) and +0x20 (the slot the
BODY reads via mov rcx,[rax+0x20]). Then disassemble those slot targets to see
if the poller's per-tick virtual call is the analysis advancer. Read-only."""
from __future__ import annotations
import sys
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

DLL = r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll"
VT = 0x28b8878   # live controller vtable RVA
SLOTS = (0x20, 0xc0)  # body's [rax+0x20]; poller's [rax+0xc0]


def main():
    pe = pefile.PE(DLL, fast_load=True)
    img_base = pe.OPTIONAL_HEADER.ImageBase
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True

    print(f"=== controller vtable @ RVA 0x{VT:x} (VA 0x{img_base+VT:x}) ===")
    # read 32 qword slots
    data = pe.get_data(VT, 32 * 8)
    import struct
    slots = struct.unpack("<32Q", data)
    for i, va in enumerate(slots):
        off = i * 8
        rva_t = va - img_base if va > img_base else None
        mark = "  <== poller calls this every tick" if off == 0xc0 else \
               ("  <== body reads this ([rax+0x20])" if off == 0x20 else "")
        print(f"  +0x{off:x} [slot {i}]: 0x{va:x}  (RVA {('0x%x' % rva_t) if rva_t else 'external'}){mark}")

    # disassemble the +0xc0 and +0x20 targets
    for off in SLOTS:
        va = slots[off // 8]
        rva_t = va - img_base
        if not (0 < rva_t < 0x4000000):
            print(f"\n+0x{off:x} target not in module (external/indirect)")
            continue
        d = pe.get_data(rva_t, 0x400)
        print(f"\n=== vtable+0x{off:x} target @ RVA 0x{rva_t:x} (first 40 insns) ===")
        n = 0
        for ins in md.disasm(d, va):
            n += 1
            note = ""
            if "[" in ins.op_str:
                note = "  <== mem"
            if ins.mnemonic == "call":
                note += " CALL"
            if ins.mnemonic.startswith("j"):
                note += " ->branch"
            print(f"  0x{ins.address:x}: {ins.mnemonic:8s} {ins.op_str}{note}")
            if n >= 40:
                break


if __name__ == "__main__":
    sys.exit(main())
