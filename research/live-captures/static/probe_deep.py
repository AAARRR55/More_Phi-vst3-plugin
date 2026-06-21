#!/usr/bin/env python3
"""
Strategy C — deep probe. OFFLINE static analysis of iZOzonePro.dll only.

Focus:
  0xeabdb0  controller (calls poller+applier, 1 caller 0xeab020)
  0xeab020  sole caller of controller -> strong trigger candidate
  0xfbd0b0  caller of data_stream_ctor (analysis ingest), 1 caller
  0x1072860 caller of data_stream_ctor, 1 caller
  0x163dfa0 caller of dispatcher 0x166ca90, 0 callers (= entry-ish)
  0xd4b830  caller of dispatcher 0x166ca90, 0 callers (= entry-ish)

We:
  - disassemble each, list their callees + callers
  - detect what string TABLE BASE each references (RIP-rel lea to a rdata addr,
    then later indexed) to link phase-string tables even without direct LEA
  - walk upward from 0xeab020 and from the data_stream_ctor callers to find a
    common ancestor (= the true UI trigger entry).
"""

from __future__ import annotations
import json, bisect
from pathlib import Path
from collections import deque, defaultdict

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_OP_IMM, X86_REG_RIP

DLL = r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll"
IMAGE_BASE = 0x180000000
OUT = Path(r"G:/More_Phi-vst3-plugin/tools/live_captures/static/probe_deep_out.json")

TARGETS = [0xeabdb0, 0xeab020, 0xfbd0b0, 0x1072860, 0x163dfa0, 0xd4b830,
           0xe9f4f0, 0xea4fd0, 0x17ff6f0, 0x1052380, 0x10790f0]

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
    return rva - s.VirtualAddress + s.PointerToRawData if s else None


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


def build_graph(pe, data, funcs, begins):
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    callers = defaultdict(set)
    callees = defaultdict(set)
    rip_targets = defaultdict(set)  # func_begin -> set of rva it references via rip-rel
    for s in pe.sections:
        if not (s.Characteristics & 0x20000000):
            continue
        base_rva = s.VirtualAddress
        base_off = s.PointerToRawData
        sec = data[base_off:base_off + s.Misc_VirtualSize]
        n = len(sec)
        # edge scan e8/e9
        i = 0
        while i < n - 5:
            b = sec[i]
            if b in (0xE8, 0xE9):
                rel = int.from_bytes(sec[i+1:i+5], "little", signed=True)
                src_rva = base_rva + i
                tgt_rva = src_rva + 5 + rel
                idx = bisect.bisect_left(begins, tgt_rva)
                if idx < len(begins) and begins[idx] == tgt_rva:
                    sf = function_of(funcs, begins, src_rva)
                    if sf:
                        callers[tgt_rva].add(sf[0])
                        callees[sf[0]].add(tgt_rva)
                i += 5
            else:
                i += 1
        # rip-rel ref scan via capstone
        for ins in md.disasm(sec, IMAGE_BASE + base_rva):
            for op in ins.operands:
                if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                    tgt_va = ins.address + ins.size + op.mem.disp
                    tgt_rva = tgt_va - IMAGE_BASE
                    sf = function_of(funcs, begins, ins.address - IMAGE_BASE)
                    if sf:
                        rip_targets[sf[0]].add(tgt_rva)
    return callers, callees, rip_targets


def read_cstr(data, pe, rva, maxlen=64):
    off = rva_to_off(pe, rva)
    if off is None:
        return None
    end = data.find(b"\x00", off, off + maxlen)
    if end == -1:
        end = off + maxlen
    try:
        return data[off:end].decode("utf-8", errors="replace")
    except Exception:
        return None


def disasm_func(pe, data, funcs, begins, fb, label, max_insns=80):
    fb_pair = function_of(funcs, begins, fb)
    if not fb_pair:
        print("  (no function for %s)" % hex(fb))
        return []
    b, e = fb_pair
    off = rva_to_off(pe, b)
    size = min(e - b, 0xa00)
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    print("\n=== %s  %s  (size %d, end %s) ===" % (label, hex(b), e - b, hex(e)))
    rows = []
    for ins in md.disasm(data[off:off+size], IMAGE_BASE + b):
        rows.append((ins.address - IMAGE_BASE, ins.mnemonic, ins.op_str,
                     [(op.type, getattr(op, 'mem', None)) for op in ins.operands]))
    # print compact: just calls and rip-rel lea/mov
    cnt = 0
    for rva, mn, ops, _ in rows:
        show = (mn in ("call", "jmp") and not ops.startswith("qword ptr [r"))
        show = show or (mn == "lea" and "rip" in ops)
        show = show or (mn == "cmp" and ("0x17" in ops or "0x16" in ops or "0x18" in ops))
        if show:
            print("  0x%x: %s %s" % (rva, mn, ops))
        cnt += 1
        if cnt >= max_insns:
            break
    return rows


def main():
    print("[*] loading")
    with open(DLL, "rb") as f:
        data = f.read()
    pe = pefile.PE(data=data, fast_load=True)
    funcs, begins = parse_functions(pe)
    callers, callees, rip_targets = build_graph(pe, data, funcs, begins)

    # string rva -> name map (for resolving rip-rel refs)
    strmap = {}
    for name in PHASE_STRINGS + [MASTER_STRING]:
        i = data.find(name.encode())
        while i != -1:
            rva = pe.get_rva_from_offset(i)
            if rva is not None:
                strmap[rva] = name
            i = data.find(name.encode(), i + 1)

    # .rdata section range (for spotting table-base refs)
    rdata = next((s for s in pe.sections if s.Name.rstrip(b"\x00") == b".rdata"), None)

    print("\n[*] function summary for each target:")
    for fb in TARGETS:
        fp = function_of(funcs, begins, fb)
        if not fp:
            print("  %-12s NOT A FUNCTION START" % hex(fb))
            continue
        cls = sorted(callees.get(fb, ()))
        cas = sorted(callers.get(fb, ()))
        # rip-rel refs into .rdata, try to decode as cstrings
        rdata_strs = []
        for r in sorted(rip_targets.get(fb, ())):
            if rdata and rdata.VirtualAddress <= r < rdata.VirtualAddress + rdata.Misc_VirtualSize:
                cs = read_cstr(data, pe, r)
                if cs and (len(cs) >= 4) and cs.isprintable():
                    rdata_strs.append((r, cs))
        print("  %-12s callers=%d callees=%d  rip_refs=%d" % (
            hex(fb), len(cas), len(cls), len(rip_targets.get(fb, ()))))
        print("      callers: " + ", ".join(hex(x) for x in cas))
        print("      callees: " + ", ".join(hex(x) for x in cls))
        # show decoded rdata strings (unique, capped)
        seen = set()
        for r, cs in rdata_strs:
            if cs in seen:
                continue
            seen.add(cs)
            print("      rdata@%s: %r" % (hex(r), cs[:60]))
            if len(seen) >= 12:
                break

    # Disassemble each of the key targets (compact: calls + rip lea)
    for fb, label in [
        (0xeabdb0, "controller (poller+applier caller)"),
        (0xeab020, "controller's sole caller (TRIGGER?)"),
        (0xfbd0b0, "data_stream_ctor caller #1 (analysis)"),
        (0x1072860, "data_stream_ctor caller #2 (analysis)"),
        (0xe9f4f0, "0xeab020's caller"),
        (0xea4fd0, "0xe9f4f0's caller"),
        (0x163dfa0, "dispatcher caller A"),
        (0xd4b830, "dispatcher caller B"),
    ]:
        disasm_func(pe, data, funcs, begins, fb, label, max_insns=60)

    # Find common ancestor: BFS upward from 0xeab020 (controller-entry) and from
    # 0xfbd0b0 (data-stream caller). The intersection is likely the TRIGGER root.
    def bfs(start, hops=6):
        seen = {start: 0}
        q = deque([start])
        while q:
            n = q.popleft()
            if seen[n] >= hops:
                continue
            for p in callers.get(n, ()):
                if p not in seen:
                    seen[p] = seen[n] + 1
                    q.append(p)
        return seen

    up_ctrl = bfs(0xeab020, 8)
    up_ds1 = bfs(0xfbd0b0, 8)
    up_ds2 = bfs(0x1072860, 8)
    up_disp_a = bfs(0x163dfa0, 8)
    up_disp_b = bfs(0xd4b830, 8)

    common_cd = set(up_ctrl) & set(up_ds1)
    common_cd2 = set(up_ctrl) & set(up_ds2)
    common_all = set(up_ctrl) & set(up_ds1) & set(up_ds2)
    common_disp = (set(up_disp_a) | set(up_disp_b)) & (set(up_ds1) | set(up_ds2))

    print("\n[*] BFS-upward set sizes: ctrl=%d ds1=%d ds2=%d dispA=%d dispB=%d" % (
        len(up_ctrl), len(up_ds1), len(up_ds2), len(up_disp_a), len(up_disp_b)))
    print("[*] common(0xeab020-up  &  0xfbd0b0-up):")
    for x in sorted(common_cd):
        print("    %s  (ctrl_dist=%d ds1_dist=%d)" % (hex(x), up_ctrl[x], up_ds1[x]))
    print("[*] common(0xeab020-up  &  0x1072860-up):")
    for x in sorted(common_cd2):
        print("    %s  (ctrl_dist=%d ds2_dist=%d)" % (hex(x), up_ctrl[x], up_ds2[x]))
    print("[*] common(ctrl & ds1 & ds2):")
    for x in sorted(common_all):
        print("    %s  ctrl=%d ds1=%d ds2=%d" % (hex(x), up_ctrl[x], up_ds1[x], up_ds2[x]))
    print("[*] common(disp-callers-up & ds-callers-up):")
    for x in sorted(common_disp):
        print("    %s  dispA=%s dispB=%s ds1=%s ds2=%s" % (
            hex(x),
            up_disp_a.get(x), up_disp_b.get(x), up_ds1.get(x), up_ds2.get(x)))

    OUT.write_text(json.dumps({
        "targets": [hex(t) for t in TARGETS],
        "upward_from_eab020": {hex(k): v for k, v in up_ctrl.items()},
        "upward_from_fbd0b0": {hex(k): v for k, v in up_ds1.items()},
        "upward_from_1072860": {hex(k): v for k, v in up_ds2.items()},
        "common_ctrl_ds1": [hex(x) for x in sorted(common_cd)],
        "common_ctrl_ds2": [hex(x) for x in sorted(common_cd2)],
        "common_all": [hex(x) for x in sorted(common_all)],
        "common_disp_ds": [hex(x) for x in sorted(common_disp)],
    }, indent=2))
    print("\n[*] wrote", OUT)


if __name__ == "__main__":
    main()
