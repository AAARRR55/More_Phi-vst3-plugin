#!/usr/bin/env python3
"""
Strategy B probe: reconstruct the "Master Assistant" subsystem cluster and
find the ORCHESTRATOR (trigger candidate).

OFFLINE / READ-ONLY: reads the on-disk iZOzonePro.dll file only. No process
attach. Authorized scope: OZONE_IPC_RESEARCH_METHODOLOGY.md sec 5.2 (plain
string xrefs + call-graph mapping of vendor code More-Phi hosts).

Method
------
1. Parse .pdata -> exact function begin/end RVAs (91,822 funcs).
2. Resolve the 20 code xrefs of the literal "Master Assistant" string (from the
   curated recon JSON, so we don't depend on re-scanning).
3. Build a *local* forward+reverse call graph (e8/e9 direct-call byte scan,
   target validated against .pdata function starts) across the union of:
       - the 20 "Master Assistant" funcs
       - their direct callees and callers (1-hop)
4. Score each cluster function as an ORCHESTRATOR / trigger:
       - few EXTERNAL callers (callers outside the cluster)   -> top of a tree
       - many INTERNAL callees (callees inside the cluster)
       - bonus: calls a known anchor (state helpers +0xEAD2E0 / +0xEAD360,
         secondary +0xEAD930, data_stream_ctor +0xFD7F30)
       - bonus: references a phase-state string table region
5. Emit capstone prologue (first ~40 insns) for the top candidates so a human
   can judge the calling convention (thiscall / context-struct arg).
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
RECON = Path("tools/live_captures/static/ozone_recon.json")

# Known observation anchors (RVA) from the live findings.
STATE_POLLER    = 0x0EAD3E0   # poller (NOT trigger)
STATE_HELPER_A  = 0x0EAD2E0
STATE_HELPER_B  = 0x0EAD360
SECONDARY       = 0x0EAD930   # apply path
CALLER          = 0x166CA90   # calls the poller (NOT trigger)
DATA_STREAM_CTOR = 0x0FD7F30  # SmoothAudioDataStream ctor (audio ingestion)
ANCHORS = {STATE_POLLER, STATE_HELPER_A, STATE_HELPER_B, SECONDARY, CALLER, DATA_STREAM_CTOR}


def rva_to_off(pe, rva):
    for s in pe.sections:
        span = max(s.Misc_VirtualSize, s.SizeOfRawData)
        if s.VirtualAddress <= rva < s.VirtualAddress + span:
            return s.PointerToRawData + (rva - s.VirtualAddress)
    return None


def off_to_rva(pe, off):
    for s in pe.sections:
        if s.PointerToRawData <= off < s.PointerToRawData + s.SizeOfRawData:
            return s.VirtualAddress + (off - s.PointerToRawData)
    return None


def parse_functions(pe, data):
    pd = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".pdata")
    funcs = []
    base = pd.PointerToRawData
    for off in range(base, base + pd.SizeOfRawData - 11, 12):
        begin = int.from_bytes(data[off:off + 4], "little")
        end = int.from_bytes(data[off + 4:off + 8], "little")
        if begin == 0:
            continue
        funcs.append((begin, end))
    funcs.sort()
    return funcs, [b for b, _ in funcs]


def function_of(funcs, begins, rva):
    if not begins:
        return None
    i = bisect.bisect_right(begins, rva) - 1
    if i < 0:
        return None
    b, e = funcs[i]
    return (b, e) if b <= rva < e else None


def scan_call_graph(text, text_rva0, begins_set):
    """Return (callers, callees): dict[func_begin] -> set[func_begin].
    Only e8/e9 direct calls whose target is a .pdata function start."""
    callers, callees = {}, {}
    n = len(text)
    for m in re.compile(rb"[\xe8\xe9]", re.DOTALL).finditer(text):
        i = m.start()
        if i + 5 > n:
            continue
        rel = int.from_bytes(text[i + 1:i + 5], "little", signed=True)
        src_rva = text_rva0 + i
        tgt_rva = src_rva + 5 + rel
        if tgt_rva not in begins_set:
            continue
        # Resolve src func lazily (caller of this helper passes begins).
        yield src_rva, tgt_rva


def listing(pe, data, funcs, begins, begin, end, max_insns=44):
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    off0 = rva_to_off(pe, begin)
    if off0 is None:
        return []
    size = min(max(end - begin, 0x40), 0x300)
    code = data[off0:off0 + size]
    image_base = pe.OPTIONAL_HEADER.ImageBase
    rows = []
    for n, insn in enumerate(md.disasm(code, image_base + begin)):
        if n >= max_insns:
            break
        rows.append({
            "rva": "0x%x" % (insn.address - image_base),
            "mnemonic": insn.mnemonic,
            "op_str": insn.op_str,
        })
    return rows


def find_string_rvas(pe, data, needle):
    nb = needle.encode("latin1")
    rvas, start = [], 0
    while True:
        i = data.find(nb, start)
        if i < 0:
            break
        rva = off_to_rva(pe, i)
        if rva is not None:
            rvas.append(rva)
        start = i + 1
    return rvas


def find_string_xref_funcs(pe, data, funcs, begins, string_rvas, image_base):
    """Find funcs that RIP-reference the string (direct) or its VA pointer table.
    Returns set of func_begin RVAs."""
    target_set = set(string_rvas)
    # pointer-table entries: qword VAs equal to image_base + string_rva
    for r in string_rvas:
        needle = (image_base + r).to_bytes(8, "little")
        start = 0
        while True:
            i = data.find(needle, start)
            if i < 0:
                break
            pr = off_to_rva(pe, i)
            if pr is not None:
                target_set.add(pr)
            start = i + 1

    text = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text")
    code = data[text.PointerToRawData:text.PointerToRawData + text.SizeOfRawData]
    text_rva0 = text.VirtualAddress
    n = len(code)
    users = set()
    rip_op = re.compile(rb"[\x48\x4c][\x8d\x8b\x89\x3b\x39\x85]"
                        rb"[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", re.DOTALL)
    for m in rip_op.finditer(code):
        i = m.start()
        if i + 7 > n:
            continue
        disp = int.from_bytes(code[i + 3:i + 7], "little", signed=True)
        tgt_rva = (text_rva0 + i) + 7 + disp
        if tgt_rva in target_set:
            sf = function_of(funcs, begins, text_rva0 + i)
            if sf:
                users.add(sf[0])
    return users, target_set


def main():
    recon = json.loads(RECON.read_text(encoding="utf-8"))
    ma_funcs = set()
    for s in recon["strings"]["Master Assistant"]["referenced_by_funcs"]:
        ma_funcs.add(int(s, 16))
    print(f"[i] 'Master Assistant' ref funcs (from recon): {len(ma_funcs)}")

    data = Path(DLL).read_bytes()
    pe = pefile.PE(DLL, fast_load=True)
    pe.parse_data_directories()
    image_base = pe.OPTIONAL_HEADER.ImageBase
    funcs, begins = parse_functions(pe, data)
    begins_set = set(begins)
    print(f"[i] functions total: {len(funcs)}  image_base=0x{image_base:x}")

    # Re-derive "Master Assistant" string RVAs + xref funcs fresh (sanity).
    ma_str_rvas = find_string_rvas(pe, data, "Master Assistant")
    ma_funcs_fresh, ma_target_set = find_string_xref_funcs(
        pe, data, funcs, begins, ma_str_rvas, image_base)
    print(f"[i] 'Master Assistant' string occurrences: {len(ma_str_rvas)}  "
          f"xref funcs (fresh): {len(ma_funcs_fresh)}")
    # Trust the recon list as the canonical cluster seed, but union with fresh.
    ma_funcs |= ma_funcs_fresh

    # Build full call graph once (byte scan of .text). ~91k funcs but scan is
    # linear over .text bytes; same approach the recon tool uses.
    text = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text")
    text_bytes = data[text.PointerToRawData:text.PointerToRawData + text.SizeOfRawData]
    text_rva0 = text.VirtualAddress
    callers, callees = {}, {}
    edges = 0
    for src_rva, tgt_rva in scan_call_graph(text_bytes, text_rva0, begins_set):
        sf = function_of(funcs, begins, src_rva)
        if not sf:
            continue
        callees.setdefault(sf[0], set()).add(tgt_rva)
        callers.setdefault(tgt_rva, set()).add(sf[0])
        edges += 1
    print(f"[i] call-graph edges: {edges}")

    # 1-hop neighborhood of the Master Assistant cluster.
    cluster = set(ma_funcs)
    onehop = set(ma_funcs)
    for f in ma_funcs:
        onehop |= callees.get(f, set())
        onehop |= callers.get(f, set())
    print(f"[i] cluster funcs: {len(cluster)}   1-hop neighborhood: {len(onehop)}")

    # Score orchestrator candidates within the cluster.
    def func_bounds(fb):
        i = bisect.bisect_left(begins, fb)
        if i < len(funcs) and funcs[i][0] == fb:
            return funcs[i][1]
        return None

    scores = []
    for f in cluster:
        callees_f = callees.get(f, set())
        callers_f = callers.get(f, set())
        ext_callers = callers_f - cluster          # callers OUTSIDE the cluster
        int_callees = callees_f & onehop            # callees in the neighborhood
        hits_anchors = callees_f & ANCHORS
        # Points:
        #  +3 per anchor callee (esp. data_stream_ctor / secondary / helpers)
        #  +2 per int callee (orchestrator fans out)
        #  -1 per external caller (a true trigger is near the root -> few ext callers)
        #  small bonus for being larger than average (orchestrators are bigger)
        end = func_bounds(f) or f
        size = end - f
        score = 0
        score += 3 * len(hits_anchors)
        score += 2 * len(int_callees)
        score -= 1 * len(ext_callers)
        score += min(size // 0x200, 6)  # cap size bonus
        scores.append({
            "func": f,
            "end": "0x%x" % end,
            "size": size,
            "callers_total": len(callers_f),
            "ext_callers": len(ext_callers),
            "callees_total": len(callees_f),
            "int_callees": len(int_callees),
            "anchor_callees": sorted(hits_anchors),
            "score": score,
        })
    scores.sort(key=lambda x: (-x["score"], -x["int_callees"], -x["size"]))

    print("\n=== CLUSTER ORCHESTRATOR RANKING (by score) ===")
    print(f"{'func':>10} {'sz':>7} {'call':>5} {'ext':>4} {'intcl':>6} {'anchors':>7} {'score':>6}  anchor_callees")
    for s in scores[:20]:
        anc = ",".join("0x%x" % a for a in s["anchor_callees"])
        print(f"0x{s['func']:08x} {s['size']:>7} {s['callers_total']:>5} {s['ext_callers']:>4} "
              f"{s['int_callees']:>6} {len(s['anchor_callees']):>7} {s['score']:>6}  {anc}")

    # For the top 5, also show which cluster funcs they call and which call them.
    print("\n=== TOP 5 candidate detail ===")
    for s in scores[:5]:
        f = s["func"]
        cl_callees = sorted(callees.get(f, set()) & cluster)
        cl_callers = sorted(callers.get(f, set()) & cluster)
        print(f"\n--- 0x{f:x} (score {s['score']}) ---")
        print(f"  cluster callees: {[hex(x) for x in cl_callees]}")
        print(f"  cluster callers: {[hex(x) for x in cl_callers]}")
        print(f"  ALL callers:     {[hex(x) for x in sorted(callers.get(f,set()))]}")
        print(f"  ALL callees:     {[hex(x) for x in sorted(callees.get(f,set()))[:25]]}")

    # Capstone prologues for the top 6 candidates.
    print("\n=== CAPSTONE PROLOGUES (top 6) ===")
    for s in scores[:6]:
        f = s["func"]
        end = int(s["end"], 16)
        rows = listing(pe, data, funcs, begins, f, end, max_insns=40)
        print(f"\n;;; func 0x{f:x} .. 0x{end:x}  (size {s['size']}, "
              f"callers={s['callers_total']} ext={s['ext_callers']} "
              f"int_callees={s['int_callees']} anchors={len(s['anchor_callees'])})")
        for r in rows:
            # Annotate calls into anchors / cluster.
            note = ""
            op = r["op_str"]
            if r["mnemonic"] in ("call", "jmp") and op.startswith("0x"):
                tgt = int(op, 16) - image_base
                if tgt in ANCHORS:
                    note = "  ;-> ANCHOR"
                elif tgt in cluster:
                    note = "  ;-> cluster"
            print(f"  {r['rva']:>10}  {r['mnemonic']:<6} {r['op_str']}{note}")

    # Save the ranking for the parent agent.
    out = {
        "dll": DLL,
        "image_base": "0x%x" % image_base,
        "master_assistant_ref_funcs": [hex(f) for f in sorted(ma_funcs)],
        "cluster_size": len(cluster),
        "ranking": [
            {**s, "func": "0x%x" % s["func"],
             "anchor_callees": ["0x%x" % a for a in s["anchor_callees"]]}
            for s in scores[:12]
        ],
    }
    Path("tools/live_captures/static/probe_master_ranking.json").write_text(
        json.dumps(out, indent=1), encoding="utf-8")
    print("\n[+] ranking -> tools/live_captures/static/probe_master_ranking.json")
    return 0


if __name__ == "__main__":
    sys.exit(main())
