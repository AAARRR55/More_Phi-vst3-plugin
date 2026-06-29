#!/usr/bin/env python3
"""
Strategy C — finalize. OFFLINE static analysis only.

Resolve the remaining ambiguities:
  1. Decode the .rdata CALLBACK TABLE slots that register each 0-caller root.
     Each root's VA appears as a qword in .rdata. We scan the surrounding
     0x80-byte window for OTHER function-pointer VAs and for strings, to
     identify the vtable / callback-table that owns each root (this names
     the subsystem: "Button handler", "Assistant analyzer", etc.).
  2. Decode rip-rel LEA targets inside 0xfbd0b0 / 0x17ff6f0 / 0x1052380 /
     0x1056120 (the analysis-ingest chain) to see what string/object they
     load — looking for "Master Assistant", phase strings, or class names.
  3. Resolve whether 0xd572f0 (controller-chain head) is reachable from a
     shared ancestor with the ingest chain via INDIRECT edges by scanning
     .rdata for any function-VA that, if treated as a caller, would unify
     the chains.
"""

from __future__ import annotations
import json, bisect, struct
from pathlib import Path
from collections import defaultdict

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP

DLL = r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll"
IMAGE_BASE = 0x180000000
OUT = Path(r"G:/More_Phi-vst3-plugin/tools/live_captures/static/probe_finalize_out.json")


def section_of(pe, rva):
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + s.Misc_VirtualSize:
            return s
    return None


def rva_to_off(pe, rva):
    s = section_of(pe, rva)
    return (rva - s.VirtualAddress + s.PointerToRawData) if s else None


def off_to_rva(pe, off):
    for s in pe.sections:
        if s.PointerToRawData <= off < s.PointerToRawData + s.SizeOfRawData:
            return off - s.PointerToRawData + s.VirtualAddress
    return None


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


def decode_callback_slot(data, pe, funcs, begins, root_va):
    """Where root_va is stored as a qword in .rdata/.data. Decode the slot's
       neighbors (other function pointers + nearby strings)."""
    needle = struct.pack("<Q", root_va)
    out = []
    i = data.find(needle)
    while i != -1:
        try:
            slot_rva = pe.get_rva_from_offset(i)
        except Exception:
            slot_rva = None
        if slot_rva is None:
            i = data.find(needle, i + 1)
            continue
        sec = section_of(pe, slot_rva)
        secname = sec.Name.rstrip(b"\x00").decode("ascii", "replace") if sec else "?"
        # scan +-0x80 bytes for qword values that look like function VAs
        neighbors = []
        base_off = i - 0x80
        for dx in range(-0x80, 0x88, 8):
            oo = base_off + dx
            if oo < 0 or oo + 8 > len(data):
                continue
            q = struct.unpack("<Q", data[oo:oo+8])[0]
            if q == 0 or q == root_va:
                continue
            fr = q - IMAGE_BASE
            if function_of(funcs, begins, fr):
                neighbors.append((oo - i, hex(fr)))
        # nearby strings
        strs = []
        for delta in (-0xC0, -0x60, +0x10, +0x40, +0x80, +0xC0):
            sr = slot_rva + delta
            cs = read_cstr(data, pe, sr)
            if cs:
                strs.append((delta, cs))
        out.append({
            "slot_rva": hex(slot_rva), "section": secname,
            "neighbor_funcs": neighbors, "nearby_strings": strs,
        })
        i = data.find(needle, i + 1)
    return out


def disasm_rip_targets(pe, data, funcs, begins, fb, max_insns=400):
    fp = function_of(funcs, begins, fb)
    if not fp:
        return []
    b, e = fp
    off = rva_to_off(pe, b)
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    hits = []
    for ins in md.disasm(data[off:off+min(e-b, max_insns*16)], IMAGE_BASE + b):
        if ins.mnemonic in ("lea", "mov", "movups", "movaps"):
            for op in ins.operands:
                if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                    tva = ins.address + ins.size + op.mem.disp
                    trva = tva - IMAGE_BASE
                    cs = read_cstr(data, pe, trva)
                    hits.append((ins.address - IMAGE_BASE, hex(trva), cs))
    return hits


def main():
    print("[*] loading")
    with open(DLL, "rb") as f:
        data = f.read()
    pe = pefile.PE(data=data, fast_load=True)
    funcs, begins = parse_functions(pe)

    roots = {
        "controller_chain_head": 0xd572f0,
        "ingest_a_head":         0x1056120,
        "ingest_b_head":         0x10790f0,
        "disp_thunk_A":          0x163dfa0,
        "disp_thunk_B":          0xd4b830,
        "ingest_a_body":         0xfbd0b0,   # calls data_stream_ctor
        "ingest_a_mid":          0x17ff6f0,
        "ingest_a_low":          0x1052380,
    }

    print("\n[*] callback-slot decoding for each root (who registers it):")
    slot_out = {}
    for name, fb in roots.items():
        slots = decode_callback_slot(data, pe, funcs, begins, IMAGE_BASE + fb)
        slot_out[name] = slots
        print("\n  === %s (%s) ===" % (name, hex(fb)))
        for s in slots[:3]:
            print("    slot @ %s [%s]" % (s["slot_rva"], s["section"]))
            for d, fr in s["neighbor_funcs"][:8]:
                print("        %+d  func %s" % (d, fr))
            for d, cs in s["nearby_strings"][:6]:
                print("        %+d  str  %r" % (d, cs))

    print("\n[*] rip-rel string references inside analysis-ingest chain:")
    rip_out = {}
    for name, fb in [("0xfbd0b0", 0xfbd0b0), ("0x17ff6f0", 0x17ff6f0),
                     ("0x1052380", 0x1052380), ("0x1056120", 0x1056120),
                     ("0x10790f0", 0x10790f0), ("0xd572f0", 0xd572f0)]:
        hits = disasm_rip_targets(pe, data, funcs, begins, fb)
        rip_out[name] = hits
        print("\n  === %s ===" % name)
        # only print string-bearing targets, dedup
        seen = set()
        for rva, tgt, cs in hits:
            if cs and cs not in seen:
                seen.add(cs)
                print("    0x%-10s -> %s  %r" % (hex(rva), tgt, cs[:60]))

    # Cross-check: is 0xd572f0 (controller head) ever stored as a qword near
    # the same callback table as the ingest heads? (would indicate a shared
    # UI registry). Look for overlapping slot neighborhoods.
    print("\n[*] slot-overlap check (shared registries?):")
    def slot_rvas(va):
        needle = struct.pack("<Q", va)
        rs = []
        i = data.find(needle)
        while i != -1:
            r = pe.get_rva_from_offset(i)
            if r is not None:
                rs.append(r)
            i = data.find(needle, i + 1)
        return rs
    sets = {name: set(slot_rvas(IMAGE_BASE + fb)) for name, fb in roots.items()}
    for a in roots:
        for b in roots:
            if a >= b:
                continue
            inter = sets[a] & sets[b]
            if inter:
                print("    %s & %s share slots: %s" % (a, b, [hex(x) for x in inter]))

    OUT.write_text(json.dumps({
        "slot_decode": slot_out,
        "rip_string_refs": rip_out,
    }, indent=2))
    print("\n[*] wrote", OUT)


if __name__ == "__main__":
    main()
