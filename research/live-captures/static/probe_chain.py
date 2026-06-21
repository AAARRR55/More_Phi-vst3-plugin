#!/usr/bin/env python3
"""
Strategy C probe: upward caller-chain analysis from the Ozone Assistant anchors.

OFFLINE / static-only. Reads the on-disk iZOzonePro.dll. No process attach.

Goal
----
Identify the Master Assistant TRIGGER (the function the GUI "Play" button calls
to START an analysis), by walking 2-3 hops UP from the known observation anchors
  +0xEAD3E0 (poller), +0xEAD930 (applier), +0x166CA90 (assistant_caller/dispatcher),
  +0xFD7F30 (SmoothAudioDataStream ctor).

Distinguishing heuristic for TRIGGER vs POLLER vs DISPATCHER:
  - POLLER   : called very frequently from few sites (idle tick). Here poller
               +0xEAD3E0 has callers {0xEABDB0, 0x166CA90}.
  - DISPATCHER: calls DOWN into both poller + applier + (often) data_stream_ctor;
               0x166CA90 does exactly this -> a per-frame assistant controller.
  - TRIGGER  : called from UI/dispatch code (often few callers, sometimes 1),
               itself CALLS the dispatcher/controller (0x166CA90) and/or the
               poller+applier, and is NOT called every idle tick. Usually sits
               above the dispatcher, references "Master Assistant" string, and
               may set up the analysis / push an event.

This probe:
  1. Rebuilds the .pdata function table + call graph (same logic as
     tools/ozone_static_recon.py).
  2. For each anchor, walks callers upward 3 hops (BFS), recording the chain.
  3. Highlights nodes that call BOTH the poller and applier (controller shape),
     nodes whose callees include the data_stream_ctor (audio ingestion =>
     analysis start), and nodes referencing the "Master Assistant" string.
  4. Disassembles the top candidate(s) and prints callees/callers.

Output: printed to stdout + dumped to tools/live_captures/static/probe_chain_out.json
"""

from __future__ import annotations
import json
import sys
from pathlib import Path
from collections import deque, defaultdict

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP

DLL = r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll"
IMAGE_BASE = 0x180000000
OUT_JSON = Path(r"G:/More_Phi-vst3-plugin/tools/live_captures/static/probe_chain_out.json")

ANCHORS = {
    "poller":            0x0EAD3E0,
    "applier":           0x0EAD930,
    "dispatcher":        0x166CA90,  # ozone_master_assistant_caller
    "data_stream_ctor":  0x0FD7F30,
    "state_helper_a":    0x0EAD2E0,
    "state_helper_b":    0x0EAD360,
    "unknown_shared":    0x0EABDB0,  # <-- unexplored: shared caller of poller+applier
}

PHASE_STRINGS = [
    "PROCESSING_LISTENING",
    "LEARNING_EQ_AND_CLASSIFYING_GENRE",
    "PROCESSING_SETTING_SIGNAL_CHAIN",
]
MASTER_STRING = "Master Assistant"
DATASTREAM_STRING = "SmoothAudioDataStream"


def rva_to_off(pe, rva):
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + s.Misc_VirtualSize:
            return rva - s.VirtualAddress + s.PointerToRawData
    return None


def parse_functions(pe):
    pd = next((s for s in pe.sections if s.Name.rstrip(b"\x00") == b".pdata"), None)
    if pd is None:
        raise SystemExit("no .pdata")
    raw = pd.get_data()
    funcs = []
    for i in range(0, len(raw) - 8, 12):
        begin = int.from_bytes(raw[i:i+4], "little")
        end = int.from_bytes(raw[i+4:i+8], "little")
        unwd = int.from_bytes(raw[i+8:i+12], "little")
        if begin == 0 and end == 0 and unwd == 0:
            continue
        if 0x1000 <= begin < 0x40000000 and end > begin:
            funcs.append((begin, end))
    funcs.sort()
    begins = [f[0] for f in funcs]
    return funcs, begins


def function_of(funcs, begins, rva):
    import bisect
    i = bisect.bisect_right(begins, rva) - 1
    if 0 <= i < len(funcs):
        b, e = funcs[i]
        if b <= rva < e:
            return (b, e)
    return None


def find_string_rvas(data, needles):
    out = {}
    for n in needles:
        nb = n.encode("utf-8")
        locs = []
        i = data.find(nb)
        while i != -1:
            locs.append(i)
            i = data.find(nb, i + 1)
        out[n] = locs
    return out


def build_graph(pe, data, funcs, begins, image_base, str_rva_to_name):
    """callers/callees by function begin RVA. Also string xref map."""
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    callers = defaultdict(set)
    callees = defaultdict(set)
    string_users = defaultdict(set)  # func_begin -> set of string names

    code_sections = [s for s in pe.sections if s.Characteristics & 0x20000000]
    for s in code_sections:
        base_rva = s.VirtualAddress
        base_off = s.PointerToRawData
        size = s.Misc_VirtualSize
        code = data[base_off:base_off + size]
        insns = list(md.disasm(code, image_base + base_rva))
        for ins in insns:
            addr_va = ins.address
            addr_rva = addr_va - image_base
            if ins.mnemonic in ("call", "jmp") and len(ins.operands) == 1:
                op = ins.operands[0]
                if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                    # indirect call through [rip+disp] -> jump table / vtable; skip for graph
                    pass
                continue
            # detect RIP-relative lea/mov referencing a string
            if ins.mnemonic in ("lea", "mov", "movups", "movaps") and len(ins.operands) >= 1:
                for op in ins.operands:
                    if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                        tgt_va = ins.address + ins.size + op.mem.disp
                        tgt_rva = tgt_va - image_base
                        if tgt_rva in str_rva_to_name:
                            fb = function_of(funcs, begins, addr_rva)
                            if fb:
                                string_users[fb[0]].add(str_rva_to_name[tgt_rva])

        # second pass for direct call edges (e8/e9 rel32) — robust scan
        # We re-scan the raw bytes for e8/e9 to catch edges the detailed walker
        # may have split across instruction boundaries.
        sec_code = data[base_off:base_off + size]
        n = len(sec_code)
        i = 0
        while i < n - 5:
            b = sec_code[i]
            if b in (0xE8, 0xE9):
                rel = int.from_bytes(sec_code[i+1:i+5], "little", signed=True)
                src_rva = base_rva + i
                tgt_va = image_base + src_rva + 5 + rel
                tgt_rva = tgt_va - image_base
                # only keep if target is a real function start
                idx = bisect.bisect_left(begins, tgt_rva)
                if idx < len(begins) and begins[idx] == tgt_rva:
                    sf = function_of(funcs, begins, src_rva)
                    if sf:
                        callers[tgt_rva].add(sf[0])
                        callees[sf[0]].add(tgt_rva)
                i += 5
            else:
                i += 1
    return callers, callees, string_users


def fmt(rva):
    return "0x%x" % rva


def bfs_up(start, callers, hops=3):
    """Return list of paths (each a list of rvas) from start upward, up to `hops` deep."""
    paths = []
    q = deque([(start, [start])])
    seen_at_level = {start: 0}
    while q:
        node, path = q.popleft()
        if len(path) - 1 >= hops:
            continue
        for p in sorted(callers.get(node, ())):
            # avoid cycles
            if p in path:
                continue
            newpath = path + [p]
            paths.append(newpath)
            q.append((p, newpath))
    return paths


def main():
    print("[*] loading", DLL)
    with open(DLL, "rb") as f:
        data = f.read()
    pe = pefile.PE(data=data, fast_load=True)
    print("[*] parsing .pdata function table")
    funcs, begins = parse_functions(pe)
    print("    functions:", len(funcs))

    # locate string rvas
    print("[*] locating strings")
    needle_rvas = {}
    for n in PHASE_STRINGS + [MASTER_STRING, DATASTREAM_STRING]:
        i = data.find(n.encode("utf-8"))
        cnt = 0
        while i != -1:
            # rva of this byte offset
            rva = pe.get_rva_from_offset(i) if pe.get_rva_from_offset(i) else None
            if rva is not None:
                needle_rvas[rva] = n
                cnt += 1
            i = data.find(n.encode("utf-8"), i + 1)
        print(f"    {n}: {cnt} byte hits")

    print("[*] building call graph")
    callers, callees, string_users = build_graph(pe, data, funcs, begins, IMAGE_BASE, needle_rvas)
    print("    edges:", sum(len(v) for v in callees.values()))

    # Report: which functions reference the Master Assistant string
    ma_users = sorted(string_users.keys())
    print("\n[*] functions referencing 'Master Assistant' string (count=%d):" % len(ma_users))
    for fb in ma_users:
        print("    %s  callers=%d callees=%d" % (
            fmt(fb), len(callers.get(fb, ())), len(callees.get(fb, ()))))

    # BFS upward from each anchor
    print("\n[*] upward caller chains (up to 3 hops):")
    chains_out = {}
    for name, rva in ANCHORS.items():
        paths = bfs_up(rva, callers, hops=3)
        chains_out[name] = {"anchor": fmt(rva), "paths": [[fmt(x) for x in p] for p in paths]}
        print("\n  === %s (%s) ===" % (name, fmt(rva)))
        # dedupe + cap
        uniq = {}
        for p in paths:
            uniq[tuple(p)] = p
        shown = 0
        for p in sorted(uniq.values(), key=lambda pp: (len(pp), [-x for x in pp])):
            # print as chain
            print("    " + " -> ".join(fmt(x) for x in p))
            shown += 1
            if shown >= 40:
                print("    ... (%d more)" % (len(uniq) - shown))
                break

    # Identify controller-shape functions: those that call BOTH poller and applier
    poller = ANCHORS["poller"]
    applier = ANCHORS["applier"]
    callers_of_poller = set(callers.get(poller, ()))
    callers_of_applier = set(callers.get(applier, ()))
    both = callers_of_poller & callers_of_applier
    print("\n[*] functions that call BOTH poller(%s) and applier(%s):" % (fmt(poller), fmt(applier)))
    for fb in sorted(both):
        calls_ds = ANCHORS["data_stream_ctor"] in callees.get(fb, set())
        uses_ma = MASTER_STRING in string_users.get(fb, set())
        uses_phase = bool(string_users.get(fb, set()) & set(PHASE_STRINGS))
        print("    %s  callers=%d callees=%d  data_stream_ctor=%s  MA_string=%s  phase_string=%s" % (
            fmt(fb), len(callers.get(fb, ())), len(callees.get(fb, ())),
            calls_ds, uses_ma, uses_phase))

    # Identify functions that call the data_stream_ctor (audio ingestion => analysis)
    ds = ANCHORS["data_stream_ctor"]
    ds_callers = sorted(callers.get(ds, ()))
    print("\n[*] direct callers of data_stream_ctor(%s) (analysis ingestion):" % fmt(ds))
    for fb in ds_callers:
        calls_both = fb in both
        uses_ma = MASTER_STRING in string_users.get(fb, set())
        print("    %s  callers=%d callees=%d  calls_poller+applier=%s  MA_string=%s" % (
            fmt(fb), len(callers.get(fb, ())), len(callees.get(fb, ())), calls_both, uses_ma))

    # TRIGGER heuristic scoring.
    # A trigger is likely:
    #   - reachable within 1-2 hops above the poller/applier/dispatcher
    #   - calls the dispatcher (0x166CA90) OR directly poller+applier
    #   - ideally references "Master Assistant"
    #   - has few callers (UI entry, not a per-tick path) -> prefer callers_count small
    #   - does NOT appear as a caller of the poller ALONE with high fan-in
    dispatcher = ANCHORS["dispatcher"]
    shared = ANCHORS["unknown_shared"]
    # Candidate pool = union of: callers of dispatcher, callers of poller, callers of applier,
    # callers of shared, callers of data_stream_ctor, and MA-string users.
    pool = set()
    pool |= set(callers.get(dispatcher, ()))
    pool |= callers_of_poller
    pool |= callers_of_applier
    pool |= set(callers.get(shared, ()))
    pool |= set(callers.get(ds, ()))
    pool |= set(ma_users)
    # also second-hop: callers of the dispatcher's callers
    for c in list(callers.get(dispatcher, ())):
        pool |= set(callers.get(c, ()))

    print("\n[*] trigger-candidate scoring (pool=%d):" % len(pool))
    scored = []
    for fb in pool:
        score = 0
        reasons = []
        callees_set = callees.get(fb, set())
        callers_cnt = len(callers.get(fb, ()))
        callees_cnt = len(callees_set)
        if dispatcher in callees_set:
            score += 3; reasons.append("calls_dispatcher(0x166CA90)")
        if poller in callees_set and applier in callees_set:
            score += 3; reasons.append("calls_poller+applier")
        elif poller in callees_set or applier in callees_set:
            score += 1; reasons.append("calls_poller|applier")
        if ds in callees_set:
            score += 4; reasons.append("calls_data_stream_ctor(analysis ingest)")
        if shared in callees_set:
            score += 2; reasons.append("calls_shared_controller(0xEABDB0)")
        if MASTER_STRING in string_users.get(fb, set()):
            score += 3; reasons.append("refs_'Master Assistant'")
        if string_users.get(fb, set()) & set(PHASE_STRINGS):
            score += 2; reasons.append("refs_phase_string")
        # UI/entry bias: prefer few callers (1-3)
        if callers_cnt == 1:
            score += 2; reasons.append("1_caller(UI-entry?)")
        elif callers_cnt <= 3:
            score += 1; reasons.append("few_callers")
        # avoid pure pollers (called a lot) -- demote high-fan-in
        if callers_cnt >= 8:
            score -= 2; reasons.append("high_fan_in(poller?)")
        # need at least one positive anchor signal
        if any("calls_" in r or "refs_" in r for r in reasons):
            scored.append((score, fb, callers_cnt, callees_cnt, reasons))

    scored.sort(key=lambda t: (-t[0], t[1]))
    print("    %-12s %-6s %-6s %-6s %s" % ("rva", "score", "callrs", "callees", "reasons"))
    for score, fb, cc, cl, reasons in scored[:25]:
        print("    %-12s %-6d %-6d %-6d %s" % (fmt(fb), score, cc, cl, "; ".join(reasons)))

    # Disassemble the top candidate for sanity (first 40 insns).
    if scored:
        top_fb = scored[0][1]
        fb_pair = function_of(funcs, begins, top_fb)
        if fb_pair:
            b, e = fb_pair
            off = rva_to_off(pe, b)
            size = min(e - b, 0x400)
            md = Cs(CS_ARCH_X86, CS_MODE_64)
            print("\n[*] top candidate %s listing (first insns):" % fmt(top_fb))
            cnt = 0
            for ins in md.disasm(data[off:off+size], IMAGE_BASE + b):
                print("    0x%x: %s %s" % (ins.address - IMAGE_BASE, ins.mnemonic, ins.op_str))
                cnt += 1
                if cnt >= 40:
                    break

    # dump json
    summary = {
        "anchors": {k: fmt(v) for k, v in ANCHORS.items()},
        "master_assistant_users": [fmt(x) for x in ma_users],
        "callers_of_dispatcher": [fmt(x) for x in sorted(callers.get(dispatcher, ()))],
        "callers_of_shared": [fmt(x) for x in sorted(callers.get(shared, ()))],
        "callers_of_data_stream_ctor": [fmt(x) for x in ds_callers],
        "functions_calling_both_poller_applier": [fmt(x) for x in sorted(both)],
        "top_candidates": [
            {"rva": fmt(fb), "score": s, "callers": cc, "callees": cl, "reasons": reasons}
            for s, fb, cc, cl, reasons in scored[:15]
        ],
        "upward_chains": chains_out,
    }
    OUT_JSON.write_text(json.dumps(summary, indent=2))
    print("\n[*] wrote", OUT_JSON)


if __name__ == "__main__":
    import bisect  # noqa
    main()
