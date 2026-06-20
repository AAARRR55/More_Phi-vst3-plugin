#!/usr/bin/env python3
"""
Strategy C — controller-chain deep dive. OFFLINE static analysis only.

The Dynamics-module hypothesis for the data-stream chain is confirmed (strings
'Enable Multiband', 'Band %1', 'Sidechain HPF', '.Dynamics Band N ...'). So the
Master Assistant TRIGGER lives on the CONTROLLER chain, not the ingest chain.

Controller chain (poller+applier driver side):
  0xeabdb0 (calls poller+applier) <- 0xeab020 <- 0xe9f4f0 <- 0xea4fd0
     <- 0xea4d50 <- 0xea1b20 <- 0xe9fc30 <- 0xd58a20 <- 0xd572f0 (head, 0 callers)

We now:
  1. Decode ALL rip-rel targets in every controller-chain node (0xeabdb0 up to
     0xd572f0) and print strings -> identify the module that owns this chain.
  2. Decode the .rdata callback-slot neighborhood of 0xd572f0 fully (it sits in
     a vtable; the other vtable entries name the class).
  3. Look for any function that calls the controller head 0xd572f0 via INDIRECT
     call/jmp (call qword ptr [reg+disp]) by scanning code for the pattern where
     the displacement matches the slot offset.
"""

from __future__ import annotations
import json, bisect, struct
from pathlib import Path

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP

DLL = r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll"
IMAGE_BASE = 0x180000000
OUT = Path(r"G:/More_Phi-vst3-plugin/tools/live_captures/static/probe_controller_out.json")

CHAIN = [0xeabdb0, 0xeab020, 0xe9f4f0, 0xea4fd0, 0xea4d50,
         0xea1b20, 0xe9fc30, 0xd58a20, 0xd572f0]


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


def all_rip_targets(pe, data, funcs, begins, fb):
    fp = function_of(funcs, begins, fb)
    if not fp:
        return []
    b, e = fp
    off = rva_to_off(pe, b)
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    hits = []
    for ins in md.disasm(data[off:off+(e-b)], IMAGE_BASE + b):
        for op in ins.operands:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                tva = ins.address + ins.size + op.mem.disp
                trva = tva - IMAGE_BASE
                hits.append((ins.address - IMAGE_BASE, ins.mnemonic, trva))
    return hits


def main():
    print("[*] loading")
    with open(DLL, "rb") as f:
        data = f.read()
    pe = pefile.PE(data=data, fast_load=True)
    funcs, begins = parse_functions(pe)

    # 1. rip-rel targets per chain node
    print("\n[*] rip-rel string refs per controller-chain node:")
    per_node = {}
    for fb in CHAIN:
        hits = all_rip_targets(pe, data, funcs, begins, fb)
        per_node[hex(fb)] = hits
        # decode strings, dedup
        seen = {}
        for rva, mn, trva in hits:
            cs = read_cstr(data, pe, trva)
            if cs and cs not in seen:
                seen[cs] = rva
        print("\n  === %s (unique strings=%d) ===" % (hex(fb), len(seen)))
        for cs, rva in list(seen.items())[:40]:
            print("    @0x%-9x %r" % (rva, cs[:64]))

    # 2. Full vtable decode around 0xd572f0's slot. Find the slot, then scan
    #    +/- 0x200 in 8-byte steps collecting function pointers; this is the
    #    owning class's vtable.
    print("\n[*] vtable around 0xd572f0 slot:")
    needle = struct.pack("<Q", IMAGE_BASE + 0xd572f0)
    i = data.find(needle)
    if i != -1:
        slot_off = i
        # walk backward to find vtable start (heuristic: first qword at lower
        # addresses that is also a function VA), then forward.
        start = slot_off
        for back in range(slot_off, slot_off - 0x400, -8):
            q = struct.unpack("<Q", data[back:back+8])[0]
            fr = q - IMAGE_BASE
            if function_of(funcs, begins, fr) or q == 0:
                start = back
            else:
                break
        # now scan forward from `start` printing every function VA slot
        print("    vtable start (approx): file_off=0x%x" % start)
        entries = []
        for off in range(start, start + 0x400, 8):
            q = struct.unpack("<Q", data[off:off+8])[0]
            if q == 0:
                entries.append((off - start, None, "0"))
                continue
            fr = q - IMAGE_BASE
            fp = function_of(funcs, begins, fr)
            if fp:
                # try to find a string near this entry (RTTI-style: vtable -8 has type descriptor)
                entries.append((off - start, fr, "func"))
            else:
                # maybe a string ptr in .rdata
                cs = read_cstr(data, pe, fr)
                if cs:
                    entries.append((off - start, fr, "str:%r" % cs[:40]))
                else:
                    entries.append((off - start, fr, "data"))
        for delta, fr, kind in entries:
            if kind == "0":
                # compact zero entries
                continue
            mark = "  <-- 0xd572f0 SLOT" if (delta == (slot_off - start)) else ""
            print("    +%4d  %s  %s%s" % (delta, kind, hex(fr) if fr else "", mark))

    # 3. RTTI: vtable - 8 usually points at a Complete Object Locator whose +12
    #    field is a type descriptor pointer -> type name string. Try to read it.
    if i != -1:
        col_off = i - 8
        if col_off >= 0:
            col_va = struct.unpack("<Q", data[col_off:col_off+8])[0]
            print("\n[*] RTTI COL @ file 0x%x -> va 0x%x" % (col_off, col_va))
            col_rva = col_va - IMAGE_BASE if col_va else None
            if col_rva:
                coff = rva_to_off(pe, col_rva)
                if coff:
                    # COL layout: sig(4) offset(4) cdOffset(4) typeDescRVA(4) ...
                    td_rva_field = struct.unpack("<I", data[coff+12:coff+16])[0]
                    td_off = rva_to_off(pe, td_rva_field)
                    if td_off:
                        # TypeDescriptor: vtable ptr(8) spare(8) name(follows)
                        name_off = td_off + 16
                        end = data.find(b"\x00", name_off, name_off + 128)
                        name = data[name_off:end if end != -1 else name_off+128]
                        print("    TypeDescriptor name: %r" % name.decode("utf-8", "replace"))

    # 4. Look at 0xeabdb0 / 0xeab020 specifically — do they load "Master Assistant"
    #    or assistant-related state? Also their first calls.
    print("\n[*] full call sequence of 0xeabdb0 (controller) and 0xeab020 (its caller):")
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    for fb in (0xeabdb0, 0xeab020, 0xe9f4f0, 0xea4fd0, 0xd572f0):
        fp = function_of(funcs, begins, fb)
        if not fp:
            continue
        b, e = fp
        off = rva_to_off(pe, b)
        print("\n  === %s ===" % hex(fb))
        cnt = 0
        for ins in md.disasm(data[off:off+(e-b)], IMAGE_BASE + b):
            show = ins.mnemonic in ("call", "jmp")
            if ins.mnemonic in ("lea", "mov") and "rip" in ins.op_str:
                for op in ins.operands:
                    if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                        tva = ins.address + ins.size + op.mem.disp
                        cs = read_cstr(data, pe, tva - IMAGE_BASE)
                        if cs:
                            show = True
            if show:
                extra = ""
                if ins.mnemonic in ("lea",) and "rip" in ins.op_str:
                    for op in ins.operands:
                        if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                            cs = read_cstr(data, pe, ins.address + ins.size + op.mem.disp - IMAGE_BASE)
                            if cs:
                                extra = "  ; %r" % cs[:40]
                print("    0x%x: %s %s%s" % (ins.address - IMAGE_BASE, ins.mnemonic, ins.op_str, extra))
            cnt += 1
            if cnt > 80:
                break

    OUT.write_text(json.dumps({
        "chain": [hex(x) for x in CHAIN],
        "rip_targets_per_node": {k: [(a, m, t) for (a, m, t) in v] for k, v in per_node.items()},
    }, indent=2))
    print("\n[*] wrote", OUT)


if __name__ == "__main__":
    main()
