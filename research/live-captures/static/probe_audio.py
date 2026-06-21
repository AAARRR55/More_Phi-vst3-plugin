#!/usr/bin/env python3
"""
Strategy D probe: Audio-feed entry into the Ozone Master Assistant.

Authorized scope (OZONE_IPC_RESEARCH_METHODOLOGY.md sec 5.2):
  - Static analysis of the on-disk iZOzonePro.dll only (string xrefs, .pdata
    call graph, capstone listings). No process attach. No license/PACE/iLok/
    anti-tamper decode. No execution of any function.

What this probe does (OFFLINE, read-only on the DLL file):
  1. Parse the .pdata function table (91,822 fns) for begin/end RVAs.
  2. Locate the phase-state string pointer tables (19 contiguous qwords each for
     PROCESSING_LISTENING / LEARNING_EQ_AND_CLASSIFYING_GENRE /
     PROCESSING_SETTING_SIGNAL_CHAIN) and the "Master Assistant" string.
  3. Find RIP-relative LEA/MOV instructions anywhere in .text that compute the
     TABLE BASE address of each phase string array (the crux: the code does
     `lea r, [table_base]` then indexes it, so a single LEA per array is the
     real reference).
  4. Trace the call graph backwards from the SmoothAudioDataStream ctor
     (+0xFD7F30) two levels up, and forward 2 levels from each candidate
     trigger, to find convergence.
  5. Cross-check candidates against the "Master Assistant" code-ref cluster
     (20 funcs) and the assistant poller/apply anchors.
  6. Emit a ranked candidate trigger report (JSON + human summary).

Output: tools/live_captures/static/probe_audio_report.json
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
IMAGE_BASE_DEFAULT = 0x180000000
OUT_JSON = Path(__file__).with_name("probe_audio_report.json")

# Anchors (RVA) from docs/OZONE_PRIVATE_IPC_LIVE_FINDINGS_20260516.md.
ANCHORS = {
    "state_poller":              0x0EAD3E0,
    "state_helper_a":            0x0EAD2E0,
    "state_helper_b":            0x0EAD360,
    "apply_secondary":           0x0EAD930,
    "assistant_caller":          0x166CA90,
    "data_stream_ctor":          0x0FD7F30,
}

PHASE_STRINGS = [
    "PROCESSING_LISTENING",
    "LEARNING_EQ_AND_CLASSIFYING_GENRE",
    "PROCESSING_SETTING_SIGNAL_CHAIN",
]
EXTRA_STRINGS = ["Master Assistant", "SmoothAudioDataStream", "Ozone IPC 1"]


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
    pd = next((s for s in pe.sections if s.Name.rstrip(b"\x00") == b".pdata"), None)
    funcs = []
    if pd is None:
        return funcs, []
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


def find_string_rvas(pe, data, needles):
    out = {}
    for needle in needles:
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
        out[needle] = rvas
    return out


def find_pointer_table(image_base, data, pe, string_rva):
    """Return all qword RVAs anywhere in the image that hold VA(image_base+string_rva).

    A contiguous run of these qwords is the pointer table.
    """
    needle = (image_base + string_rva).to_bytes(8, "little")
    hits = []
    start = 0
    while True:
        i = data.find(needle, start)
        if i < 0:
            break
        r = off_to_rva(pe, i)
        if r is not None:
            hits.append(r)
        start = i + 1
    return hits


def find_table_base_leas(pe, data, funcs, begins, image_base, table_rvas):
    """Scan .text for RIP-relative LEA/MOV that compute any RVA in table_rvas.

    Returns {target_table_rva: set(func_begin)}.
    """
    text = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text")
    code = data[text.PointerToRawData:text.PointerToRawData + text.SizeOfRawData]
    text_rva0 = text.VirtualAddress
    n = len(code)
    targets = set(table_rvas)
    result = {t: set() for t in targets}
    # REX.W lea/mov/cmp + RIP-relative modrm (7-byte insn).
    rip_op = re.compile(rb"[\x48\x4c][\x8d\x8b\x89\x3b\x39\x85]"
                        rb"[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", re.DOTALL)
    for m in rip_op.finditer(code):
        i = m.start()
        if i + 7 > n:
            continue
        disp = int.from_bytes(code[i + 3:i + 7], "little", signed=True)
        tgt_rva = (text_rva0 + i) + 7 + disp
        if tgt_rva in targets:
            sf = function_of(funcs, begins, text_rva0 + i)
            if sf:
                result[tgt_rva].add(sf[0])
    return result


def build_call_graph(pe, data, funcs, begins):
    """Byte-scan .text for e8/e9 rel32 calls validated against .pdata fn starts."""
    text = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text")
    code = data[text.PointerToRawData:text.PointerToRawData + text.SizeOfRawData]
    text_rva0 = text.VirtualAddress
    n = len(code)
    begins_set = set(begins)
    fof = function_of
    callers, callees = {}, {}
    for m in re.compile(rb"[\xe8\xe9]", re.DOTALL).finditer(code):
        i = m.start()
        if i + 5 > n:
            continue
        rel = int.from_bytes(code[i + 1:i + 5], "little", signed=True)
        src_rva = text_rva0 + i
        tgt_rva = src_rva + 5 + rel
        if tgt_rva not in begins_set:
            continue
        sf = fof(funcs, begins, src_rva)
        if not sf:
            continue
        callees.setdefault(sf[0], set()).add(tgt_rva)
        callers.setdefault(tgt_rva, set()).add(sf[0])
    return callers, callees


def find_string_ref_funcs(pe, data, funcs, begins, image_base, string_rvas):
    """Functions that RIP-reference a string RVA directly (7-byte RIP insn)."""
    text = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text")
    code = data[text.PointerToRawData:text.PointerToRawData + text.SizeOfRawData]
    text_rva0 = text.VirtualAddress
    n = len(code)
    targets = set(string_rvas)
    result = {t: set() for t in targets}
    rip_op = re.compile(rb"[\x48\x4c][\x8d\x8b\x89\x3b\x39\x85]"
                        rb"[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", re.DOTALL)
    for m in rip_op.finditer(code):
        i = m.start()
        if i + 7 > n:
            continue
        disp = int.from_bytes(code[i + 3:i + 7], "little", signed=True)
        tgt_rva = (text_rva0 + i) + 7 + disp
        if tgt_rva in targets:
            sf = function_of(funcs, begins, text_rva0 + i)
            if sf:
                result[tgt_rva].add(sf[0])
    out = set()
    for users in result.values():
        out |= users
    return out


def disasm_func(pe, data, funcs, begins, func_begin, max_insns=60):
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = False
    f = function_of(funcs, begins, func_begin)
    if not f:
        return []
    begin, end = f
    off0 = rva_to_off(pe, begin)
    if off0 is None:
        return []
    size = min(max(end - begin, 0x40), 0x600)
    code = data[off0:off0 + size]
    rows = []
    for k, insn in enumerate(md.disasm(code, IMAGE_BASE_DEFAULT + begin)):
        if k >= max_insns:
            break
        rows.append({"rva": "0x%x" % (insn.address - IMAGE_BASE_DEFAULT),
                     "mnemonic": insn.mnemonic, "op_str": insn.op_str})
    return rows


def hexset(s):
    return sorted("0x%x" % v for v in s)


def main():
    print(f"[*] reading DLL: {DLL}")
    data = Path(DLL).read_bytes()
    pe = pefile.PE(DLL, fast_load=True)
    pe.parse_data_directories()
    image_base = pe.OPTIONAL_HEADER.ImageBase
    print(f"[*] image_base=0x{image_base:x}")

    funcs, begins = parse_functions(pe, data)
    print(f"[*] parsed {len(funcs)} functions from .pdata")

    callers, callees = build_call_graph(pe, data, funcs, begins)

    string_rvas = find_string_rvas(pe, data, PHASE_STRINGS + EXTRA_STRINGS)

    report = {
        "dll": DLL,
        "image_base": "0x%x" % image_base,
        "functions_total": len(funcs),
        "anchors": {},
        "phase_tables": {},
        "audio_feed_trace": {},
        "master_assistant_cluster": [],
        "candidates": [],
    }

    # --- Anchor function summaries (callers/callees) ---
    for label, rva in ANCHORS.items():
        f = function_of(funcs, begins, rva)
        fb = f[0] if f else None
        report["anchors"][label] = {
            "anchor_rva": "0x%x" % rva,
            "func_begin": ("0x%x" % fb) if fb else None,
            "func_end": ("0x%x" % f[1]) if f else None,
            "callers": hexset(callers.get(fb, set())) if fb else [],
            "callers_count": len(callers.get(fb, set())) if fb else 0,
            "callees": hexset(callees.get(fb, set()))[:60] if fb else [],
            "callees_count": len(callees.get(fb, set())) if fb else 0,
        }

    # --- Phase-string pointer tables + the LEA of each table base ---
    for name in PHASE_STRINGS:
        srvas = string_rvas[name]
        # Collect pointer-table entries for every occurrence of the string.
        all_table_rvas = []
        per_string = []
        for s in srvas:
            entries = find_pointer_table(image_base, data, pe, s)
            per_string.append({"string_rva": "0x%x" % s,
                               "ptr_entries": ["0x%x" % e for e in entries]})
            all_table_rvas.extend(entries)
        # Identify contiguous runs (the actual table arrays).
        uniq = sorted(set(all_table_rvas))
        runs = []
        if uniq:
            cur = [uniq[0]]
            for v in uniq[1:]:
                if v - cur[-1] == 8:
                    cur.append(v)
                else:
                    runs.append(cur)
                    cur = [v]
            runs.append(cur)
        # Find code that LEAs any of these table RVAs.
        lea_users = find_table_base_leas(pe, data, funcs, begins, image_base, uniq)
        user_funcs = set()
        for u in lea_users.values():
            user_funcs |= u
        report["phase_tables"][name] = {
            "string_rvas": ["0x%x" % r for r in srvas],
            "ptr_entry_total": len(uniq),
            "contiguous_runs": [["0x%x" % x for x in run] for run in runs],
            "largest_run_len": max((len(r) for r in runs), default=0),
            "table_base_lea_user_funcs": hexset(user_funcs),
            "table_base_lea_user_count": len(user_funcs),
            "per_string_entries": per_string,
        }
        print(f"[phase] {name}: ptr_entries={len(uniq)} runs={len(runs)} "
              f"largest_run={max((len(r) for r in runs), default=0)} "
              f"table_base_lea_users={len(user_funcs)}")

    # --- Audio-feed trace: callers of data_stream_ctor, 3 levels up ---
    target = ANCHORS["data_stream_ctor"]
    f = function_of(funcs, begins, target)
    ctor_begin = f[0] if f else target
    levels = {}
    current = {ctor_begin}
    seen = set()
    for lvl in range(1, 4):
        nxt = set()
        for fn in current:
            for c in callers.get(fn, set()):
                if c not in seen:
                    nxt.add(c)
                    seen.add(c)
        levels[f"level{lvl}_callers"] = hexset(nxt)
        levels[f"level{lvl}_count"] = len(nxt)
        current = nxt
        if not nxt:
            break
    report["audio_feed_trace"] = {
        "ctor_func_begin": "0x%x" % ctor_begin,
        "direct_callers": hexset(callers.get(ctor_begin, set())),
        "direct_callers_count": len(callers.get(ctor_begin, set())),
        "ancestor_trace": levels,
    }

    # --- "Master Assistant" code-ref cluster (20 funcs from recon) ---
    ma_string_rvas = string_rvas["Master Assistant"]
    ma_funcs = find_string_ref_funcs(pe, data, funcs, begins, image_base, ma_string_rvas)
    report["master_assistant_cluster"] = hexset(ma_funcs)
    report["master_assistant_cluster_count"] = len(ma_funcs)
    print(f"[cluster] Master Assistant code-ref funcs: {len(ma_funcs)}")

    # --- Convergence analysis ---
    # Collect candidate trigger funcs:
    #  (A) funcs that LEA a phase-string table base
    #  (B) funcs in the Master Assistant cluster
    #  (C) direct + L2 callers of the data_stream_ctor
    phase_lea_funcs = set()
    for name in PHASE_STRINGS:
        for u in report["phase_tables"][name]["table_base_lea_user_funcs"]:
            phase_lea_funcs.add(int(u, 16))

    audio_callers = set(callers.get(ctor_begin, set()))
    l2 = set()
    for c in audio_callers:
        l2 |= callers.get(c, set())

    cand_set = set()
    cand_set |= phase_lea_funcs
    cand_set |= ma_funcs
    cand_set |= audio_callers
    cand_set |= l2
    # Also include the known anchors themselves for ranking context.
    for rva in ANCHORS.values():
        f = function_of(funcs, begins, rva)
        if f:
            cand_set.add(f[0])

    # Rank candidates.
    score_rows = []
    for fb in cand_set:
        score = 0
        reasons = []
        if fb in phase_lea_funcs:
            score += 3
            reasons.append("lea-phase-table")
        if fb in ma_funcs:
            score += 2
            reasons.append("master-assistant-ref")
        if fb in audio_callers:
            score += 3
            reasons.append("direct-calls-datastream-ctor")
        if fb in l2:
            score += 2
            reasons.append("l2-ancestor-of-datastream-ctor")
        # Bonus: calls BOTH the poller/apply anchor AND constructs/feeds audio.
        callees_fb = callees.get(fb, set())
        calls_state = bool(callees_fb & {ANCHORS["state_poller"], ANCHORS["apply_secondary"]})
        calls_audio = bool(callees_fb & {ANCHORS["data_stream_ctor"]})
        calls_assistant_caller = ANCHORS["assistant_caller"] in callees_fb
        if calls_state:
            score += 2
            reasons.append("calls-assistant-state-anchor")
        if calls_audio:
            score += 4
            reasons.append("calls-datastream-ctor-directly")
        if calls_assistant_caller:
            score += 1
            reasons.append("calls-assistant-caller-anchor")
        # Heuristic: a trigger is called by FEW things (GUI handler) and calls MANY.
        n_callers = len(callers.get(fb, set()))
        n_callees = len(callees_fb)
        if n_callers <= 4 and n_callees >= 6:
            score += 2
            reasons.append(f"sparse-caller/rich-callee({n_callers}/{n_callees})")
        if not reasons:
            continue
        f = function_of(funcs, begins, fb)
        fsize = (f[1] - f[0]) if f else 0
        score_rows.append({
            "func_begin": "0x%x" % fb,
            "func_end": ("0x%x" % f[1]) if f else None,
            "func_size": fsize,
            "score": score,
            "reasons": reasons,
            "callers_count": n_callers,
            "callees_count": n_callees,
            "callers": hexset(callers.get(fb, set()))[:20],
            "callees": hexset(callees_fb)[:40],
        })
    score_rows.sort(key=lambda r: (-r["score"], -r["callees_count"], r["func_begin"]))
    report["candidates"] = score_rows

    # --- Emit ---
    OUT_JSON.write_text(json.dumps(report, indent=1), encoding="utf-8")
    print(f"\n[+] report -> {OUT_JSON}")
    print(f"[+] {len(score_rows)} ranked candidates. Top 12:")
    for r in score_rows[:12]:
        print(f"    score={r['score']:>2} fn={r['func_begin']} "
              f"size={r['func_size']:>5} callers={r['callers_count']} "
              f"callees={r['callees_count']} reasons={r['reasons']}")

    # --- Detailed disasm of top 6 ---
    print("\n[+] disasm of top candidates (first 30 insns):")
    for r in score_rows[:6]:
        fb = int(r["func_begin"], 16)
        print(f"\n=== {r['func_begin']}  (score {r['score']}, {r['reasons']}) ===")
        for row in disasm_func(pe, data, funcs, begins, fb, max_insns=30):
            print(f"    {row['rva']:>10}  {row['mnemonic']:<8} {row['op_str']}")


if __name__ == "__main__":
    sys.exit(main())
