#!/usr/bin/env python3
"""
Strategy C — Master Assistant function deep dive. OFFLINE static analysis only.

Confirmed: 0xd58a20 references 'Master Assistant' string and operates on an
'ElementChain' with 'Maximizer'/'Threshold'. 0xd572f0 is its vtable[0] entry.
Now fully decode:
  - 0xd572f0 (the vtable callback head) — its full body + what it passes to 0xd58a20
  - 0xd58a20 (the MA-string function) — full call list + string refs + key state writes
  - 0xe9fc30 (has 2 callers — branch point on the controller chain) — to see if it
    is the apply-vs-start fork
Also: identify ALL functions in the binary that reference 'Master Assistant'
string via RIP-rel, using a corrected detector that catches both direct LEA
and table-base LEA (lea r, [rip+disp] where disp lands anywhere in .rdata
within 0x40 of a Master Assistant string occurrence).
"""

from __future__ import annotations
import json, bisect, struct
from pathlib import Path

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP

DLL = r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll"
IMAGE_BASE = 0x180000000
OUT = Path(r"G:/More_Phi-vst3-plugin/tools/live_captures/static/probe_ma_out.json")


def section_of(pe, rva):
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + s.Misc_VirtualSize:
            return s
    return None


def rva_to_off(pe, rva):
    s = section_of(pe, rva)
    return (rva - s.VirtualAddress + s.PointerToRawData) if s else None


def read_cstr(data, pe, rva, maxlen=96):
    off = rva_to_off(pe, rva)
    if off is None:
        return None
    end = data.find(b"\x00", off, off + maxlen)
    if end == -1:
        end = off + maxlen
    raw = data[off:end]
    try:
        s = raw.decode("utf-8")
        if s and s.isprintable() and len(s) >= 3:
            return s
    except Exception:
        pass
    return None


def parse_functions(pe):
    pd = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".pdata")
    raw = pd.get_data()
    funcs = []
    for i in range(0, len(raw) - 8, 12):
        b = int.from_bytes(raw[i:i+4], "little")
        e = int.from_bytes(raw[i+4:i+8], "little")
        u = int.from_bytes(raw[i+8:i+12], "little")
        if b == 0 and e == 0 and u == 0:
            continue
        if 0x1000 <= b < 0x40000000 and e > b:
            funcs.append((b, e))
    funcs.sort()
    return funcs, [f[0] for f in funcs]


def function_of(funcs, begins, rva):
    i = bisect.bisect_right(begins, rva) - 1
    if 0 <= i < len(funcs):
        b, e = funcs[i]
        if b <= rva < e:
            return (b, e)
    return None


def full_disasm(pe, data, funcs, begins, fb):
    fp = function_of(funcs, begins, fb)
    if not fp:
        return
    b, e = fp
    off = rva_to_off(pe, b)
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    print("\n=== %s  size=%d  begin=%s end=%s ===" % (hex(b), e-b, hex(b), hex(e)))
    for ins in md.disasm(data[off:off+(e-b)], IMAGE_BASE + b):
        extra = ""
        if "rip" in ins.op_str:
            for op in ins.operands:
                if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                    tva = ins.address + ins.size + op.mem.disp
                    cs = read_cstr(data, pe, tva - IMAGE_BASE)
                    if cs:
                        extra = "  ; %r" % cs[:50]
        print("  0x%x: %s %s%s" % (ins.address - IMAGE_BASE, ins.mnemonic, ins.op_str, extra))


def main():
    print("[*] loading")
    with open(DLL, "rb") as f:
        data = f.read()
    pe = pefile.PE(data=data, fast_load=True)
    funcs, begins = parse_functions(pe)

    # Full disasm of the key nodes
    for fb in (0xd572f0, 0xd58a20, 0xe9fc30, 0xea1b20, 0xea4d50):
        full_disasm(pe, data, funcs, begins, fb)

    # Count callers of 0xe9fc30 (branch point) and list them
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    callers_9fc30 = []
    for s in pe.sections:
        if not (s.Characteristics & 0x20000000):
            continue
        base_rva = s.VirtualAddress
        base_off = s.PointerToRawData
        sec = data[base_off:base_off + s.Misc_VirtualSize]
        n = len(sec)
        i = 0
        while i < n - 5:
            b = sec[i]
            if b in (0xE8, 0xE9):
                rel = int.from_bytes(sec[i+1:i+5], "little", signed=True)
                tgt_rva = base_rva + i + 5 + rel
                if tgt_rva == 0xe9fc30:
                    sf = function_of(funcs, begins, base_rva + i)
                    callers_9fc30.append((base_rva + i, sf[0] if sf else None))
                i += 5
            else:
                i += 1
    print("\n[*] callers of 0xe9fc30 (controller-chain branch point):")
    for site, fb in callers_9fc30:
        print("    call site 0x%x in func %s" % (site, hex(fb) if fb else "?"))

    # Find ALL functions referencing 'Master Assistant' string via rip-rel LEA/MOV
    # landing within +/-0x40 of any Master Assistant occurrence in .rdata.
    ma_locs = []  # rvas
    i = data.find(b"Master Assistant")
    while i != -1:
        r = pe.get_rva_from_offset(i)
        if r is not None:
            ma_locs.append(r)
        i = data.find(b"Master Assistant", i + 1)
    print("\n[*] 'Master Assistant' string at %d rdata rvas" % len(ma_locs))
    ma_set = set()
    for r in ma_locs:
        for d in range(-0x40, 0x48, 1):
            ma_set.add(r + d)

    users = set()
    for s in pe.sections:
        if not (s.Characteristics & 0x20000000):
            continue
        base_rva = s.VirtualAddress
        base_off = s.PointerToRawData
        sec = data[base_off:base_off + s.Misc_VirtualSize]
        for ins in md.disasm(sec, IMAGE_BASE + base_rva):
            for op in ins.operands:
                if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                    tva = ins.address + ins.size + op.mem.disp
                    trva = tva - IMAGE_BASE
                    if trva in ma_set:
                        fb = function_of(funcs, begins, ins.address - IMAGE_BASE)
                        if fb:
                            users.add(fb[0])
    print("[*] functions with rip-rel ref near 'Master Assistant' (table-base indexers): %d" % len(users))
    for fb in sorted(users):
        # quick callee/caller counts
        print("    %s" % hex(fb))

    OUT.write_text(json.dumps({
        "callers_of_e9fc30": [hex(fb) if fb else None for _, fb in callers_9fc30],
        "master_assistant_users": [hex(x) for x in sorted(users)],
    }, indent=2))
    print("\n[*] wrote", OUT)


if __name__ == "__main__":
    main()
