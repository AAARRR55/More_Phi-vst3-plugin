#!/usr/bin/env python3
"""
Strategy D follow-up: disassemble the audio-feed ancestry chain and the L3 root
to identify which function is the actual TRIGGER (GUI "Play" entry).

Offline, read-only on the DLL. No process attach, no execution.

Chain discovered by probe_audio.py:
    0x1052380 (L3 root, single caller-context)
      -> 0x17ff6f0 / 0x10790f0 (L2)
        -> 0xfbd0b0 / 0x1072860 (L1, call data_stream_ctor 0xfd7f30)
"""

from __future__ import annotations

import bisect
import json
import re
import sys
from pathlib import Path

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

DLL = r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll"
IB = 0x180000000
OUT = Path(__file__).with_name("probe_chain2_report.json")

CHAIN = {
    "L3_root":      [0x1052380],
    "L2":           [0x17ff6f0, 0x10790f0],
    "L1_data_feed": [0xfbd0b0, 0x1072860],
    "data_stream_ctor": [0xfd7f30],
}

# Anchors to cross-reference in disasm.
ANCHORS = {
    0x0EAD3E0: "state_poller",
    0x0EAD2E0: "state_helper_a",
    0x0EAD360: "state_helper_b",
    0x0EAD930: "apply_secondary",
    0x166CA90: "assistant_caller",
    0x0FD7F30: "data_stream_ctor",
}


def rva_to_off(pe, rva):
    for s in pe.sections:
        span = max(s.Misc_VirtualSize, s.SizeOfRawData)
        if s.VirtualAddress <= rva < s.VirtualAddress + span:
            return s.PointerToRawData + (rva - s.VirtualAddress)
    return None


def parse_functions(pe, data):
    pd = next((s for s in pe.sections if s.Name.rstrip(b"\x00") == b".pdata"), None)
    funcs = []
    base = pd.PointerToRawData
    for off in range(base, base + pd.SizeOfRawData - 11, 12):
        b = int.from_bytes(data[off:off+4], "little")
        e = int.from_bytes(data[off+4:off+8], "little")
        if b:
            funcs.append((b, e))
    funcs.sort()
    return funcs, [b for b, _ in funcs]


def function_of(funcs, begins, rva):
    i = bisect.bisect_right(begins, rva) - 1
    if i < 0:
        return None
    b, e = funcs[i]
    return (b, e) if b <= rva < e else None


def build_call_graph(pe, data, funcs, begins):
    text = next((s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text"))
    code = data[text.PointerToRawData:text.PointerToRawData + text.SizeOfRawData]
    t0 = text.VirtualAddress
    n = len(code)
    bs = set(begins)
    callers, callees = {}, {}
    for m in re.compile(rb"[\xe8\xe9]").finditer(code):
        i = m.start()
        if i + 5 > n:
            continue
        rel = int.from_bytes(code[i+1:i+5], "little", signed=True)
        src = t0 + i
        tgt = src + 5 + rel
        if tgt not in bs:
            continue
        sf = function_of(funcs, begins, src)
        if not sf:
            continue
        callees.setdefault(sf[0], set()).add(tgt)
        callers.setdefault(tgt, set()).add(sf[0])
    return callers, callees


def rip_refs_in_func(pe, data, funcs, begins, fb):
    """Return set of target RVAs reached by 7-byte RIP-relative LEA/MOV in fn fb."""
    text = next((s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text"))
    f = function_of(funcs, begins, fb)
    if not f:
        return set()
    begin, end = f
    off0 = rva_to_off(pe, begin)
    if off0 is None:
        return set()
    code = data[off0:off0 + (end - begin)]
    t0 = begin
    refs = set()
    rip_op = re.compile(rb"[\x48\x4c][\x8d\x8b\x89\x3b\x39\x85]"
                        rb"[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]")
    for m in rip_op.finditer(code):
        i = m.start()
        if i + 7 > len(code):
            continue
        disp = int.from_bytes(code[i+3:i+7], "little", signed=True)
        refs.add(t0 + i + 7 + disp)
    return refs


def read_cstr_at_rva(pe, data, rva, maxlen=64):
    off = rva_to_off(pe, rva)
    if off is None:
        return None
    end = data.find(b"\x00", off, off + maxlen)
    if end < 0:
        end = off + maxlen
    try:
        return data[off:end].decode("latin1")
    except Exception:
        return None


def disasm(pe, data, funcs, begins, fb, max_insns=120):
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = False
    f = function_of(funcs, begins, fb)
    if not f:
        return []
    begin, end = f
    off0 = rva_to_off(pe, begin)
    code = data[off0:off0 + min(end - begin, 0x1200)]
    rows = []
    for k, insn in enumerate(md.disasm(code, IB + begin)):
        if k >= max_insns:
            break
        rva = insn.address - IB
        op = insn.op_str
        # Annotate calls/leas against anchors and strings.
        note = ""
        # Detect direct call to a known anchor.
        if insn.mnemonic == "call" and op.startswith("0x"):
            try:
                tgt = int(op, 16) - IB
                if tgt in ANCHORS:
                    note = f"  ; -> {ANCHORS[tgt]}"
            except ValueError:
                pass
        # RIP-relative operand annotation.
        if "rip" in op:
            # find 0x... address
            mtoks = re.findall(r"0x[0-9a-f]+", op)
            for t in mtoks:
                try:
                    addr = int(t, 16)
                    trva = addr - IB
                    if trva in ANCHORS:
                        note += f"  ; rip->{ANCHORS[trva]}"
                    else:
                        s = read_cstr_at_rva(pe, data, trva)
                        if s and len(s) >= 4 and s.isprintable():
                            note += f"  ; rip->\"{s}\""
                except ValueError:
                    pass
        rows.append({"rva": "0x%x" % rva, "mnemonic": insn.mnemonic,
                     "op_str": op, "note": note})
    return rows


def main():
    data = Path(DLL).read_bytes()
    pe = pefile.PE(DLL, fast_load=True)
    pe.parse_data_directories()
    funcs, begins = parse_functions(pe, data)
    callers, callees = build_call_graph(pe, data, funcs, begins)

    # Precompute all RIP string refs in .rdata for quick note lookup.
    # (Already done lazily in disasm.)

    report = {"chain": {}, "summary": []}
    for level, fns in CHAIN.items():
        report["chain"][level] = {}
        for fb in fns:
            f = function_of(funcs, begins, fb)
            fsize = (f[1] - f[0]) if f else 0
            cs = callees.get(fb, set())
            cr = callers.get(fb, set())
            rip_refs = rip_refs_in_func(pe, data, funcs, begins, fb)
            # Which rip refs land on a known string?
            string_refs = []
            for r in rip_refs:
                s = read_cstr_at_rva(pe, data, r)
                if s and len(s) >= 4 and s.isprintable():
                    string_refs.append({"rva": "0x%x" % r, "str": s})
            anchor_calls = {a: ANCHORS[a] for a in cs if a in ANCHORS}
            entry = {
                "func_begin": "0x%x" % fb,
                "func_end": ("0x%x" % f[1]) if f else None,
                "size": fsize,
                "callers": ["0x%x" % c for c in sorted(cr)],
                "callers_count": len(cr),
                "callees": ["0x%x" % c for c in sorted(cs)][:60],
                "callees_count": len(cs),
                "anchor_calls": [{"rva": "0x%x" % k, "name": v}
                                 for k, v in anchor_calls.items()],
                "rip_string_refs": string_refs[:20],
                "listing": disasm(pe, data, funcs, begins, fb, max_insns=120),
            }
            report["chain"][level]["0x%x" % fb] = entry

            print(f"\n{'='*70}\n[{level}] 0x{fb:x}  size={fsize}  "
                  f"callers={len(cr)} callees={len(cs)}")
            if cr:
                print(f"  CALLERS: {['0x%x'%c for c in sorted(cr)]}")
            if anchor_calls:
                print(f"  ANCHOR CALLS: {list(anchor_calls.values())}")
            if string_refs:
                print(f"  STRING REFS ({len(string_refs)}):")
                for sr in string_refs[:12]:
                    print(f"     {sr['rva']}  \"{sr['str']}\"")

    OUT.write_text(json.dumps(report, indent=1), encoding="utf-8")
    print(f"\n[+] report -> {OUT}")


if __name__ == "__main__":
    sys.exit(main())
