#!/usr/bin/env python3
"""
Static recon of iZOzonePro.dll to locate the Ozone Master Assistant *trigger*.

Why this exists
---------------
The documented anchors from docs/OZONE_PRIVATE_IPC_LIVE_FINDINGS_20260516.md
(+0xEAD3E0, +0xEAD930, +0x166CA90, +0xFD7F30) are READ-ONLY observation points
found by live Frida tracing. +0xEAD3E0 is hot during idle polling (548 calls/min,
return 0x1), i.e. a state *poller/predicate*, NOT the GUI "Play" trigger. The
actual trigger function that starts an Assistant analysis was never identified.

This tool finds it WITHOUT attaching to any iZotope/DAW process, by:
  1. Locating the Assistant phase-state strings (PROCESSING_LISTENING,
     LEARNING_EQ_AND_CLASSIFYING_GENRE, PROCESSING_SETTING_SIGNAL_CHAIN) and
     other anchors (Master Assistant, SmoothAudioDataStream, Ozone IPC 1).
  2. Finding the code that references each string (RIP-relative LEA/MOV).
  3. Reconstructing the function call graph from the x64 .pdata exception table,
     which lists the exact begin/end RVA of every function.

Authorized scope (OZONE_IPC_RESEARCH_METHODOLOGY.md sec 5.2): plain-string xrefs
and call-graph mapping of vendor code that More-Phi hosts itself. No license,
PACE/iLok, anti-tamper, or integrity-check logic is decoded; no process attached.

Usage
-----
    py tools/ozone_static_recon.py
    py tools/ozone_static_recon.py --dll <path> --out tools/live_captures/static/ozone_recon.json
    py tools/ozone_static_recon.py --self-check
"""

from __future__ import annotations

import argparse
import bisect
import json
import re
import sys
from pathlib import Path

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP

DEFAULT_DLL = r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll"

# Anchors (RVA) from the 2026-05-16 live findings.
DEFAULT_ANCHORS = {
    "ozone_master_assistant_state":     0x0EAD3E0,
    "ozone_state_helper_a":             0x0EAD2E0,
    "ozone_state_helper_b":             0x0EAD360,
    "ozone_master_assistant_secondary": 0x0EAD930,
    "ozone_master_assistant_caller":    0x166CA90,
    "ozone_data_stream_ctor":           0x0FD7F30,
}

# Phase-state strings = the assistant's internal state-machine labels.
DEFAULT_STRINGS = [
    "PROCESSING_SETTING_SIGNAL_CHAIN",
    "LEARNING_EQ_AND_CLASSIFYING_GENRE",
    "PROCESSING_LISTENING",
    "Master Assistant",
    "SmoothAudioDataStream",
    "Ozone IPC 1",
]

PHASE_STRINGS = {
    "PROCESSING_LISTENING",
    "LEARNING_EQ_AND_CLASSIFYING_GENRE",
    "PROCESSING_SETTING_SIGNAL_CHAIN",
}


def rva_to_off(pe: pefile.PE, rva: int) -> int | None:
    for s in pe.sections:
        span = max(s.Misc_VirtualSize, s.SizeOfRawData)
        if s.VirtualAddress <= rva < s.VirtualAddress + span:
            return s.PointerToRawData + (rva - s.VirtualAddress)
    return None


def off_to_rva(pe: pefile.PE, off: int) -> int | None:
    for s in pe.sections:
        if s.PointerToRawData <= off < s.PointerToRawData + s.SizeOfRawData:
            return s.VirtualAddress + (off - s.PointerToRawData)
    return None


def parse_functions(pe: pefile.PE, data: bytes):
    """x64 .pdata: array of RUNTIME_FUNCTION {u32 begin, u32 end, u32 unwind}.

    begin/end are RVAs. Returns sorted (begin,end) list and a begins list.
    """
    pd = next((s for s in pe.sections if s.Name.rstrip(b"\x00") == b".pdata"), None)
    funcs: list[tuple[int, int]] = []
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


def function_of(funcs, begins, rva: int):
    """Return (begin,end) of the function containing rva, or None."""
    if not begins:
        return None
    i = bisect.bisect_right(begins, rva) - 1
    if i < 0:
        return None
    b, e = funcs[i]
    return (b, e) if b <= rva < e else None


def find_string_rvas(pe: pefile.PE, data: bytes, needles: list[str]) -> dict[str, list[int]]:
    out: dict[str, list[int]] = {}
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


def resolve_targets(pe: pefile.PE, data: bytes, string_rvas, image_base: int):
    """Map each xref target RVA -> string name.

    Covers BOTH direct string RVAs and pointer-table entries: a qword elsewhere
    in the image holding the string's VA (how the phase-state string array is
    referenced). Returns {target_rva -> name} and per-string pointer counts.
    """
    target_to_string: dict[int, str] = {}
    pointer_counts: dict[str, int] = {}
    for name, rvas in string_rvas.items():
        pn = 0
        for r in rvas:
            target_to_string.setdefault(r, name)  # direct lea/mov of the string
            needle = (image_base + r).to_bytes(8, "little")
            start = 0
            while True:
                i = data.find(needle, start)
                if i < 0:
                    break
                pr = off_to_rva(pe, i)
                if pr is not None and pr not in target_to_string:
                    target_to_string[pr] = name  # pointer-table entry -> string
                    pn += 1
                start = i + 1
        pointer_counts[name] = pn
    return target_to_string, pointer_counts


def build_graph(pe: pefile.PE, data: bytes, funcs, begins, str_target_set: set[int]):
    """Fast, alignment-independent byte scan of .text -> function-granularity
    forward/reverse call graph + the functions that RIP-reference each string.

    Linear-sweep disasm of a .text full of data islands loses alignment and
    mis-parses real direct calls. Instead we scan raw bytes for the e8/e9
    direct-call/jmp encoding (validated against .pdata function starts to kill
    data-byte false positives) and the REX.W + lea/mov + RIP-modrm encoding for
    string references. Capstone is reserved for small neighborhoods later.
    """
    text = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text")
    code = data[text.PointerToRawData:text.PointerToRawData + text.SizeOfRawData]
    text_rva0 = text.VirtualAddress
    n = len(code)
    fof = function_of
    begins_set = set(begins)

    callers: dict[int, set[int]] = {}
    callees: dict[int, set[int]] = {}
    string_user_funcs: dict[int, set[int]] = {r: set() for r in str_target_set}
    stats = {"call_bytes": 0, "call_edges": 0, "rip_sites": 0, "string_hits": 0}

    # e8 rel32 / e9 rel32: keep edge only if target is a .pdata function start.
    for m in re.compile(rb"[\xe8\xe9]", re.DOTALL).finditer(code):
        i = m.start()
        if i + 5 > n:
            continue
        stats["call_bytes"] += 1
        rel = int.from_bytes(code[i + 1:i + 5], "little", signed=True)
        src_rva = text_rva0 + i
        tgt_rva = src_rva + 5 + rel
        if tgt_rva not in begins_set:
            continue
        sf = fof(funcs, begins, src_rva)
        if not sf:
            continue
        stats["call_edges"] += 1
        callees.setdefault(sf[0], set()).add(tgt_rva)
        callers.setdefault(tgt_rva, set()).add(sf[0])

    # REX.W + lea/mov/cmp + RIP-relative modrm (7-byte insn) -> data refs.
    rip_op = re.compile(rb"[\x48\x4c][\x8d\x8b\x89\x3b\x39\x85]"
                        rb"[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", re.DOTALL)
    for m in rip_op.finditer(code):
        i = m.start()
        if i + 7 > n:
            continue
        stats["rip_sites"] += 1
        disp = int.from_bytes(code[i + 3:i + 7], "little", signed=True)
        tgt_rva = (text_rva0 + i) + 7 + disp
        if tgt_rva in str_target_set:
            sf = fof(funcs, begins, text_rva0 + i)
            if sf:
                stats["string_hits"] += 1
                string_user_funcs[tgt_rva].add(sf[0])

    return callers, callees, string_user_funcs, stats


def listing(pe, data, funcs, begins, begin: int, end: int, max_insns: int = 70):
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = False
    off0 = rva_to_off(pe, begin)
    if off0 is None:
        return []
    size = min(max(end - begin, 0x40), 0x500)
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


def func_report(pe, data, funcs, begins, callers, callees, string_user_funcs,
                target_to_string, label, anchor_rva, func_begin):
    if func_begin is None and anchor_rva is not None:
        f = function_of(funcs, begins, anchor_rva)
        func_begin = f[0] if f else None
        func_end = f[1] if f else None
    else:
        f = function_of(funcs, begins, func_begin) if func_begin else None
        func_end = f[1] if f else None

    # Names of strings this function references (directly or via pointer table).
    referenced_names = sorted({
        target_to_string[t] for t, users in string_user_funcs.items()
        if func_begin in users and t in target_to_string
    })
    referenced = [{"string": n} for n in referenced_names]

    callee_keys = sorted(callees.get(func_begin, set()))
    callees_out = [{"rva": "0x%x" % k} for k in callee_keys[:50]]
    srcs = sorted(callers.get(func_begin, set()))

    return {
        "label": label,
        "anchor_rva": ("0x%x" % anchor_rva) if anchor_rva else None,
        "func_begin": ("0x%x" % func_begin) if func_begin is not None else None,
        "func_end": ("0x%x" % func_end) if func_end else None,
        "referenced_strings": referenced,
        "callees": callees_out,
        "callees_count": len(callee_keys),
        "callers": ["0x%x" % s for s in srcs[:50]],
        "callers_count": len(srcs),
        "listing": listing(pe, data, funcs, begins, func_begin, func_end) if func_begin else [],
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dll", default=DEFAULT_DLL)
    ap.add_argument("--out", default=None)
    ap.add_argument("--self-check", action="store_true")
    args = ap.parse_args()

    data = Path(args.dll).read_bytes()
    pe = pefile.PE(args.dll, fast_load=True)
    pe.parse_data_directories()
    image_base = pe.OPTIONAL_HEADER.ImageBase

    funcs, begins = parse_functions(pe, data)
    string_rvas = find_string_rvas(pe, data, DEFAULT_STRINGS)

    if args.self_check:
        ok = bool(funcs) and all(string_rvas.get(s) for s in PHASE_STRINGS)
        print(f"self_check ok={ok} functions={len(funcs)} "
              f"phase_found={[s for s in PHASE_STRINGS if string_rvas.get(s)]}")
        return 0 if ok else 1

    target_to_string, pointer_counts = resolve_targets(pe, data, string_rvas, image_base)
    str_target_set = set(target_to_string.keys())
    callers, callees, string_user_funcs, stats = build_graph(pe, data, funcs, begins, str_target_set)
    print(f"scan: call_bytes={stats['call_bytes']} call_edges={stats['call_edges']} "
          f"rip_sites={stats['rip_sites']} string_hits={stats['string_hits']}")

    # Aggregate referencing functions per string name (direct + pointer-table).
    users_by_name: dict[str, set[int]] = {name: set() for name in string_rvas}
    for tr, users in string_user_funcs.items():
        name = target_to_string.get(tr)
        if name:
            users_by_name[name].update(users)

    report = {
        "dll": str(args.dll),
        "image_base": "0x%x" % image_base,
        "functions_total": len(funcs),
        "strings": {},
        "anchors": {},
        "candidates": [],
    }

    for name in string_rvas:
        users = sorted(users_by_name.get(name, set()))
        report["strings"][name] = {
            "string_rvas": ["0x%x" % r for r in string_rvas[name]],
            "pointer_table_entries": pointer_counts.get(name, 0),
            "referenced_by_funcs": ["0x%x" % u for u in users[:40]],
            "referenced_by_count": len(users),
        }

    for label, rva in DEFAULT_ANCHORS.items():
        report["anchors"][label] = func_report(
            pe, data, funcs, begins, callers, callees, string_user_funcs,
            target_to_string, label, rva, None)

    # Candidate triggers = functions referencing a phase string + the anchors.
    cand_funcs: set[int] = set()
    for name in PHASE_STRINGS:
        cand_funcs |= users_by_name.get(name, set())
    for rva in DEFAULT_ANCHORS.values():
        f = function_of(funcs, begins, rva)
        if f:
            cand_funcs.add(f[0])

    for fb in sorted(cand_funcs):
        r = func_report(pe, data, funcs, begins, callers, callees, string_user_funcs,
                        target_to_string, "candidate", None, fb)
        r["phase_string_hits"] = [s["string"] for s in r["referenced_strings"]
                                  if s["string"] in PHASE_STRINGS]
        if not r["phase_string_hits"]:
            r["listing"] = []
        report["candidates"].append(r)

    out_path = Path(args.out) if args.out else Path("tools/live_captures/static/ozone_recon.json")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(report, indent=1), encoding="utf-8")

    print(f"functions={len(funcs)}")
    for name in DEFAULT_STRINGS:
        info = report["strings"][name]
        print(f"string {name!r}: rvas={len(info['string_rvas'])} "
              f"ptr_entries={info['pointer_table_entries']} ref_funcs={info['referenced_by_count']}")
    print("\nanchors:")
    for label, r in report["anchors"].items():
        print(f"  {label}: func={r['func_begin']} callers={r['callers_count']} "
              f"strings={[s['string'] for s in r['referenced_strings']]}")
    phase_cands = [c for c in report["candidates"] if c["phase_string_hits"]]
    print(f"\nphase-string candidate trigger funcs: {len(phase_cands)}")
    for c in sorted(phase_cands, key=lambda x: -len(x["phase_string_hits"])):
        print(f"  func={c['func_begin']} phase_hits={c['phase_string_hits']} "
              f"callers={c['callers_count']} callees={c['callees_count']}")
    print(f"\nfull report -> {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
