#!/usr/bin/env python3
"""
Strategy C — final probe. OFFLINE static analysis only.

The upward closures of the controller-chain and analysis-ingest-chain are tiny
and disjoint. Each terminates at a function with 0 DIRECT callers (reachable
only via indirect call: vtable / registered callback / event dispatch).
We need to:
  1. Print the full upward closure of every chain so we see the roots.
  2. For each 0-caller root, scan .rdata for its RVA as a pointer in a table
     (i.e. find where the code is registered as a callback) and decode nearby
     strings to identify the subsystem (e.g. "Button", "Play", "Analyze").
  3. Disassemble the most trigger-shaped root fully (calls + key state writes).
  4. Cross-check: does the root reference the "Master Assistant" string-table
     base (RIP-rel lea to an address inside the 0x2678000..0x2795000 cluster
     where the phase strings live)?
"""

from __future__ import annotations
import json, bisect, struct
from pathlib import Path
from collections import deque, defaultdict

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_OP_IMM, X86_REG_RIP

DLL = r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll"
IMAGE_BASE = 0x180000000
OUT = Path(r"G:/More_Phi-vst3-plugin/tools/live_captures/static/probe_roots_out.json")

PHASE_STRINGS = ["PROCESSING_LISTENING",
                 "LEARNING_EQ_AND_CLASSIFYING_GENRE",
                 "PROCESSING_SETTING_SIGNAL_CHAIN"]
MASTER_STRING = "Master Assistant"


def section_of(pe, rva):
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + s.Misc_VirtualSize:
            return s
    return None


def rva_to_off(pe, rva):
    s = section_of(pe, rva)
    return (rva - s.VirtualAddress + s.PointerToRawData) if s else None


def read_cstr(data, pe, rva, maxlen=80):
    off = rva_to_off(pe, rva)
    if off is None:
        return None
    end = data.find(b"\x00", off, off + maxlen)
    if end == -1:
        end = off + maxlen
    raw = data[off:end]
    try:
        s = raw.decode("utf-8")
        if s and s.isprintable():
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


def build_callers(pe, data, funcs, begins):
    callers = defaultdict(set)
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
                src_rva = base_rva + i
                tgt_rva = src_rva + 5 + rel
                idx = bisect.bisect_left(begins, tgt_rva)
                if idx < len(begins) and begins[idx] == tgt_rva:
                    callers[tgt_rva].add(src_rva)
                i += 5
            else:
                i += 1
    return callers


def find_rdata_refs_to(data, pe, target_va):
    """Find qword pointers in .rdata/.data whose value == target_va.
       Returns list of (file_offset, section_name)."""
    hits = []
    needle = struct.pack("<Q", target_va)
    i = data.find(needle)
    while i != -1:
        try:
            rva = pe.get_rva_from_offset(i)
        except Exception:
            rva = None
        sec = section_of(pe, rva) if rva is not None else None
        if sec is not None:
            hits.append((i, sec.Name.rstrip(b"\x00").decode("ascii", "replace")))
        i = data.find(needle, i + 1)
    return hits


def main():
    print("[*] loading")
    with open(DLL, "rb") as f:
        data = f.read()
    pe = pefile.PE(data=data, fast_load=True)
    funcs, begins = parse_functions(pe)
    callers = build_callers(pe, data, funcs, begins)
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True

    starts = {
        "controller_chain_root": 0xeab020,
        "analysis_ingest_a":     0xfbd0b0,
        "analysis_ingest_b":     0x1072860,
        "dispatcher_caller_A":   0x163dfa0,
        "dispatcher_caller_B":   0xd4b830,
    }

    def bfs(start, hops=10):
        seen = {start: 0}
        q = deque([start])
        while q:
            n = q.popleft()
            if seen[n] >= hops:
                continue
            # callers keyed by func begin; convert src site -> func begin
            for site in callers.get(n, ()):
                fb = function_of(funcs, begins, site)
                if fb and fb[0] not in seen:
                    seen[fb[0]] = seen[n] + 1
                    q.append(fb[0])
        return seen

    print("\n[*] full upward closures:")
    closures = {}
    for name, st in starts.items():
        up = bfs(st, 12)
        closures[name] = up
        print("\n  === %s (%s) ===" % (name, hex(st)))
        # sort by hop distance
        for fb, d in sorted(up.items(), key=lambda kv: (kv[1], kv[0])):
            ncallers = len(callers.get(fb, ()))
            print("    d=%d  %s  direct_callers=%d" % (d, hex(fb), ncallers))

    # Roots = nodes in closure with 0 direct callers (entry via indirect).
    print("\n[*] entry-shaped roots (0 direct callers) across all closures:")
    roots = set()
    for up in closures.values():
        for fb in up:
            if len(callers.get(fb, ())) == 0:
                roots.add(fb)
    for fb in sorted(roots):
        n = sum(1 for up in closures.values() if fb in up)
        # find .rdata pointer references to this function VA (= callback registration)
        refs = find_rdata_refs_to(data, pe, IMAGE_BASE + fb)
        print("    %s  closures=%d  rdata_ptr_refs=%d" % (hex(fb), n, len(refs)))
        for off, sec in refs[:6]:
            rva = pe.get_rva_from_offset(off)
            # look at nearby strings (before and after the pointer slot)
            ctx_before = read_cstr(data, pe, rva - 0x40) or ""
            ctx_after = read_cstr(data, pe, rva + 8) or ""
            ctx_after2 = read_cstr(data, pe, rva + 0x18) or ""
            ctx_after3 = read_cstr(data, pe, rva + 0x28) or ""
            print("        ref@%s(%s): before=%r after=%r / %r / %r" % (
                hex(rva), sec,
                ctx_before[:48], ctx_after[:48], ctx_after2[:48], ctx_after3[:48]))

    # Deep disasm of the two dispatcher callers (they directly invoke the per-frame
    # assistant controller 0x166ca90 and have 0 direct callers = callback entry).
    print("\n[*] deep disasm of dispatcher callers (callback-shaped triggers):")
    for fb in (0x163dfa0, 0xd4b830):
        fp = function_of(funcs, begins, fb)
        if not fp:
            continue
        b, e = fp
        off = rva_to_off(pe, b)
        size = e - b
        print("\n  === %s  size=%d  (0 direct callers) ===" % (hex(b), size))
        for ins in md.disasm(data[off:off+size], IMAGE_BASE + b):
            extra = ""
            if ins.mnemonic in ("lea", "mov") and "rip" in ins.op_str:
                # resolve rip-rel target
                for op in ins.operands:
                    if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                        tva = ins.address + ins.size + op.mem.disp
                        trva = tva - IMAGE_BASE
                        cs = read_cstr(data, pe, trva)
                        if cs:
                            extra = "  ; -> %r" % cs[:50]
            print("    0x%x: %s %s%s" % (ins.address - IMAGE_BASE, ins.mnemonic, ins.op_str, extra))

    # Also dump the analysis-ingest roots' first ~50 insns to see if they set
    # the phase state (e.g. write PROCESSING_LISTENING enum).
    print("\n[*] first insns of analysis-ingest entry 0xfbd0b0 (only calls + key writes):")
    fp = function_of(funcs, begins, 0xfbd0b0)
    if fp:
        b, e = fp
        off = rva_to_off(pe, b)
        cnt = 0
        for ins in md.disasm(data[off:off+min(e-b, 0x1200)], IMAGE_BASE + b):
            m = ins.mnemonic
            show = (m in ("call", "jmp") and not ins.op_str.startswith("qword ptr [r"))
            if m == "mov" and ("0x17" in ins.op_str or "= 0x1" in ins.op_str or "0x16" in ins.op_str):
                show = True
            if m == "lea" and "rip" in ins.op_str:
                show = True
            if show:
                print("    0x%x: %s %s" % (ins.address - IMAGE_BASE, m, ins.op_str))
            cnt += 1
            if cnt >= 200:
                break

    OUT.write_text(json.dumps({
        "closures": {name: {hex(k): v for k, v in up.items()} for name, up in closures.items()},
        "roots": [hex(x) for x in sorted(roots)],
    }, indent=2))
    print("\n[*] wrote", OUT)


if __name__ == "__main__":
    main()
