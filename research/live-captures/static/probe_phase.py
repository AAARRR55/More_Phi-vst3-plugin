#!/usr/bin/env python3
"""
Strategy A probe: phase-string pointer-TABLE base xref.

The phase-state strings (PROCESSING_LISTENING, LEARNING_EQ_AND_CLASSIFYING_GENRE,
PROCESSING_SETTING_SIGNAL_CHAIN) each have ~19 pointer-table entries but 0 direct
code refs. The code references the TABLE BASE (the start of the contiguous qword
array), then indexes it. This probe:

  1. Finds the contiguous pointer arrays (runs of qwords whose VAs are the
     phase-string RVAs) and records each array's BASE RVA.
  2. Scans .text for RIP-relative lea/mov resolving to a table-base RVA -> the
     state-machine function that selects/indexes a phase.
  3. For each candidate, captures: callers, callees, whether it touches the known
     state helpers (0xEAD2E0 / 0xEAD360) or writes a phase index, and a short
     capstone listing around the RIP-reference site.

OFFLINE ONLY: reads the on-disk DLL. No process attach, no execution.
Authorized scope: OZONE_IPC_RESEARCH_METHODOLOGY.md sec 5.2 (static string xref +
call-graph mapping of vendor code More-Phi hosts).
"""

from __future__ import annotations

import bisect
import json
import re
import struct
from pathlib import Path

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

DLL = r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll"
OUT = Path("tools/live_captures/static/probe_phase_report.json")

PHASE_STRINGS = [
    "PROCESSING_LISTENING",
    "LEARNING_EQ_AND_CLASSIFYING_GENRE",
    "PROCESSING_SETTING_SIGNAL_CHAIN",
]

# Known anchors from the live findings.
STATE_HELPER_A = 0x0EAD2E0
STATE_HELPER_B = 0x0EAD360
STATE_POLLER   = 0x0EAD3E0
STATE_APPLIER  = 0x0EAD930
CALLER_166CA90 = 0x166CA90
DATA_STREAM_CTOR = 0x0FD7F30
ANCHOR_FUNCS = {STATE_HELPER_A, STATE_HELPER_B, STATE_POLLER, STATE_APPLIER,
                CALLER_166CA90, DATA_STREAM_CTOR}

# The phase-index lookup tables discovered in state_helper listings:
#   state_helper_a @ 0xEAD311: lea rax,[rip+0x17d5238] -> 0xEAD318 + 0x17d5238 = 0x2682550
#   state_helper_b @ 0xEAD391: lea rax,[rip+0x17d5180] -> 0xEAD398 + 0x17d5180 = 0x2682518
PHASE_LOOKUP_TABLE_A = 0x2682550
PHASE_LOOKUP_TABLE_B = 0x2682518


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
        begin = int.from_bytes(data[off:off+4], "little")
        end = int.from_bytes(data[off+4:off+8], "little")
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


def find_string_vas(pe, data, needles):
    """Return {name: [rva,...]} of each needle in the image."""
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


def find_pointer_arrays(pe, data, image_base, string_vas):
    """Find contiguous qword arrays of phase-string pointers.

    For each phase string, collect every file offset where a qword equals
    image_base+rva for some rva of that string. Then group consecutive offsets
    (stride 8) into arrays; record base RVA and entries.
    Returns {name: [{base_rva, entries:[va,...], va_set}]}
    """
    out = {}
    for name, rvas in string_vas.items():
        va_set = {image_base + r for r in rvas}
        # Map: file offset -> VA stored there (only if it's one of our VAs)
        hit_offs = {}
        needle8 = set()
        for va in va_set:
            needle8.add(va.to_bytes(8, "little"))
        # Brute scan for each VA. Build offset->va map.
        for va in va_set:
            pat = va.to_bytes(8, "little")
            start = 0
            while True:
                i = data.find(pat, start)
                if i < 0:
                    break
                hit_offs[i] = va
                start = i + 1
        # Group consecutive offsets at stride 8 into runs.
        offs = sorted(hit_offs)
        runs = []
        cur = []
        for o in offs:
            if cur and o == cur[-1] + 8:
                cur.append(o)
            else:
                if len(cur) >= 2:
                    runs.append(cur)
                cur = [o]
        if len(cur) >= 2:
            runs.append(cur)
        arrays = []
        for run in runs:
            base_off = run[0]
            # Extend the array backwards/forwards to capture the FULL contiguous
            # table (entries may include other phase strings or related pointers).
            # Walk back while the qword 8 bytes earlier is also a plausible image VA.
            b = base_off
            while b - 8 >= 0:
                q = int.from_bytes(data[b-8:b], "little")
                if image_base < q < image_base + 0x4000000:
                    b -= 8
                else:
                    break
            e = run[-1] + 8
            while e + 8 <= len(data):
                q = int.from_bytes(data[e:e+8], "little")
                if image_base < q < image_base + 0x4000000:
                    e += 8
                else:
                    break
            entries = []
            for o in range(b, e, 8):
                q = int.from_bytes(data[o:o+8], "little")
                entries.append(q)
            base_rva = off_to_rva(pe, b)
            arrays.append({
                "base_off": b,
                "base_rva": base_rva,
                "count": len(entries),
                "entries": [hex(x) for x in entries],
                "phase_hits_in_array": sum(1 for o in range(b, e, 8)
                                           if int.from_bytes(data[o:o+8], "little") in va_set),
            })
        out[name] = {"va_set": va_set, "arrays": arrays}
    return out


# RIP-relative REX.W + opcode + modrm(rip) encodings. 7-byte form for
# 48/4C prefix + {8d,8b,89,3b,39,85} + {05,0d,15,1d,25,2d,35,3d}.
RIP_RE = re.compile(rb"[\x48\x4c][\x8d\x8b\x89\x3b\x39\x85]"
                    rb"[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", re.DOTALL)


def scan_text_rip_refs(pe, data, target_rvas):
    """Scan .text for RIP-relative refs resolving into target_rvas.

    Returns list of {src_rva, disp, tgt_rva}.
    """
    text = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text")
    code = data[text.PointerToRawData:text.PointerToRawData + text.SizeOfRawData]
    text_rva0 = text.VirtualAddress
    n = len(code)
    tgt = set(target_rvas)
    hits = []
    for m in RIP_RE.finditer(code):
        i = m.start()
        if i + 7 > n:
            continue
        disp = int.from_bytes(code[i+3:i+7], "little", signed=True)
        src_rva = text_rva0 + i
        tgt_rva = src_rva + 7 + disp
        if tgt_rva in tgt:
            hits.append({"src_rva": src_rva, "disp": disp, "tgt_rva": tgt_rva})
    return hits


# Direct call/jmp e8/e9 rel32
CALL_RE = re.compile(rb"[\xe8\xe9]", re.DOTALL)


def build_callgraph(pe, data, funcs, begins):
    """Forward+reverse direct-call graph at function granularity.

    Only keeps edges whose target is a .pdata function start.
    """
    text = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text")
    code = data[text.PointerToRawData:text.PointerToRawData + text.SizeOfRawData]
    text_rva0 = text.VirtualAddress
    n = len(code)
    begins_set = set(begins)
    callers, callees = {}, {}
    for m in CALL_RE.finditer(code):
        i = m.start()
        if i + 5 > n:
            continue
        rel = int.from_bytes(code[i+1:i+5], "little", signed=True)
        src_rva = text_rva0 + i
        tgt_rva = src_rva + 5 + rel
        if tgt_rva not in begins_set:
            continue
        sf = function_of(funcs, begins, src_rva)
        if not sf:
            continue
        callees.setdefault(sf[0], set()).add(tgt_rva)
        callers.setdefault(tgt_rva, set()).add(sf[0])
    return callers, callees


def disasm_at(pe, data, image_base, rva, before=0x20, after=0x80):
    """Disassemble a window around rva for context. Returns list of insn dicts."""
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = False
    # Start a bit before to catch the instruction containing/near rva.
    start_rva = max(0, rva - before)
    off0 = rva_to_off(pe, start_rva)
    if off0 is None:
        return []
    size = before + after
    code = data[off0:off0 + size]
    rows = []
    for insn in md.disasm(code, image_base + start_rva):
        rows.append({
            "rva": "0x%x" % (insn.address - image_base),
            "addr": "0x%x" % insn.address,
            "mnemonic": insn.mnemonic,
            "op_str": insn.op_str,
        })
        if insn.address - image_base > rva + after:
            break
    return rows


def disasm_func(pe, data, image_base, begin, end, max_insns=400):
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = False
    off0 = rva_to_off(pe, begin)
    if off0 is None:
        return []
    size = min(max(end - begin, 0x40), 0x1400)
    code = data[off0:off0 + size]
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


def analyze_func_for_trigger(pe, data, image_base, funcs, begins,
                             callers, callees, fb):
    """Score a function for trigger-likeness.

    Signals:
      - calls a state helper (0xEAD2E0 / 0xEAD360) with a phase index arg
      - writes an immediate phase index (mov dword [...], <small const>)
      - references a phase-table base
      - has a UI/entry-shaped caller chain (few callers, reachable)
      - NOT the poller itself (caller of 0xEAD3E0 only)
    """
    fe = None
    for b, e in funcs:
        if b == fb:
            fe = e
            break
    listing = disasm_func(pe, data, image_base, fb, fe or fb + 0x200)
    text = "\n".join(f"{r['rva']}: {r['mnemonic']} {r['op_str']}" for r in listing)

    cs = sorted(callees.get(fb, set()))
    calls_helper_a = STATE_HELPER_A in cs
    calls_helper_b = STATE_HELPER_B in cs
    calls_poller = STATE_POLLER in cs
    calls_applier = STATE_APPLIER in cs
    calls_data_stream = any(c == DATA_STREAM_CTOR for c in cs)

    # Immediate "mov dword [mem], imm32" with small positive constant -> phase id write.
    imm_writes = []
    for r in listing:
        s = r["op_str"]
        # mov dword ptr [rsp+...], 0xN  / mov dword ptr [rbp+...], N
        m = re.search(r"mov\s+dword ptr \[[^\]]+\], (0x[0-9a-f]+|\d+)$", s)
        if m:
            v = int(m.group(1), 0)
            if 0 < v <= 0x40:
                imm_writes.append((r["rva"], v))

    srcs = sorted(callers.get(fb, set()))
    # Walk callers up 2 hops for entry-path signal.
    up2 = set()
    for c in srcs:
        up2 |= callers.get(c, set())

    return {
        "func_begin": "0x%x" % fb,
        "func_end": "0x%x" % (fe or 0),
        "callees": ["0x%x" % c for c in cs[:60]],
        "callees_count": len(cs),
        "callers": ["0x%x" % c for c in srcs[:40]],
        "callers_count": len(srcs),
        "callers_2hop": ["0x%x" % c for c in sorted(up2)[:40]],
        "signals": {
            "calls_state_helper_a": calls_helper_a,
            "calls_state_helper_b": calls_helper_b,
            "calls_state_poller": calls_poller,
            "calls_state_applier": calls_applier,
            "calls_data_stream_ctor": calls_data_stream,
            "phase_imm_writes": [{"rva": r, "value": v} for r, v in imm_writes[:20]],
        },
        "is_anchor_func": fb in ANCHOR_FUNCS,
    }


def main():
    data = Path(DLL).read_bytes()
    pe = pefile.PE(DLL, fast_load=True)
    pe.parse_data_directories()
    image_base = pe.OPTIONAL_HEADER.ImageBase
    funcs, begins = parse_functions(pe, data)

    string_vas = find_string_vas(pe, data, PHASE_STRINGS)
    arrays_by_name = find_pointer_arrays(pe, data, image_base, string_vas)

    # Collect all candidate base RVAs (and also the known phase-index lookup tables).
    base_rvas = set()
    base_sources = {}  # rva -> [(name, count, phase_hits)]
    for name, info in arrays_by_name.items():
        for a in info["arrays"]:
            if a["base_rva"] is not None:
                base_rvas.add(a["base_rva"])
                base_sources.setdefault(a["base_rva"], []).append(
                    (name, a["count"], a["phase_hits_in_array"]))
    # Also include the phase-index lookup tables from state_helper listings.
    base_rvas.add(PHASE_LOOKUP_TABLE_A)
    base_rvas.add(PHASE_LOOKUP_TABLE_B)
    base_sources.setdefault(PHASE_LOOKUP_TABLE_A, []).append(
        ("PHASE_INDEX_TABLE_A", 0x16, 0))
    base_sources.setdefault(PHASE_LOOKUP_TABLE_B, []).append(
        ("PHASE_INDEX_TABLE_B", 0x16, 0))

    # Also scan for RIP refs to the individual phase-string VAs themselves (direct
    # lea of the string) in case any exist that the prior recon missed due to
    # encoding variants.
    direct_targets = set()
    for name, rvas in string_vas.items():
        for r in rvas:
            direct_targets.add(r)

    rip_refs = scan_text_rip_refs(pe, data, base_rvas | direct_targets)

    # Map each RIP ref to its containing function.
    refs_by_func = {}
    for h in rip_refs:
        sf = function_of(funcs, begins, h["src_rva"])
        if not sf:
            continue
        refs_by_func.setdefault(sf[0], []).append(h)

    callers, callees = build_callgraph(pe, data, funcs, begins)

    # Candidate set: any function that RIP-references a phase table base or a
    # phase-string VA directly.
    candidate_funcs = set(refs_by_func.keys())

    # ALSO include callers of the state helpers (they pass a phase index in).
    for anc in (STATE_HELPER_A, STATE_HELPER_B):
        candidate_funcs |= callers.get(anc, set())

    report = {
        "dll": DLL,
        "image_base": "0x%x" % image_base,
        "functions_total": len(funcs),
        "phase_string_vas": {k: [hex(x) for x in v] for k, v in string_vas.items()},
        "arrays": {},
        "rip_refs_count": len(rip_refs),
        "candidate_functions": [],
    }
    for name, info in arrays_by_name.items():
        report["arrays"][name] = {
            "array_count": len(info["arrays"]),
            "arrays": [
                {"base_rva": hex(a["base_rva"]) if a["base_rva"] is not None else None,
                 "count": a["count"],
                 "phase_hits": a["phase_hits_in_array"],
                 "first_entries": a["entries"][:8]}
                for a in info["arrays"]
            ],
        }

    for fb in sorted(candidate_funcs):
        info = analyze_func_for_trigger(pe, data, image_base, funcs, begins,
                                        callers, callees, fb)
        info["rip_refs_in_func"] = [
            {"src_rva": hex(h["src_rva"]), "tgt_rva": hex(h["tgt_rva"]),
             "target_role": "phase_table_base" if h["tgt_rva"] in base_rvas
                            else "phase_string_va"}
            for h in refs_by_func.get(fb, [])
        ]
        report["candidate_functions"].append(info)

    # Rank: prefer functions that (a) RIP-ref a table base, (b) call a state
    # helper, (c) write a phase immediate, (d) are reachable but not the poller.
    def score(c):
        s = c["signals"]
        n = 0
        if c["rip_refs_in_func"]:
            n += 4
        if s["calls_state_helper_a"] or s["calls_state_helper_b"]:
            n += 3
        if s["phase_imm_writes"]:
            n += 2
        if s["calls_data_stream_ctor"]:
            n += 2
        if s["calls_state_applier"]:
            n += 1
        if s["calls_state_poller"] and not (s["calls_state_helper_a"] or s["calls_state_helper_b"]):
            n -= 2  # poller caller is lower priority
        if c["func_begin"] in ("0x%x" % STATE_POLLER, "0x%x" % STATE_APPLIER):
            n -= 1
        return n
    report["candidate_functions"].sort(key=lambda c: -score(c))
    report["_scoring"] = "higher = more trigger-like (rip-ref table base, calls state helper, writes phase imm, calls data stream ctor)"

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(report, indent=1), encoding="utf-8")

    print(f"image_base=0x{image_base:x} functions={len(funcs)}")
    for name in PHASE_STRINGS:
        a = report["arrays"][name]
        print(f"\n[{name}] string_vas={len(string_vas[name])} arrays={a['array_count']}")
        for arr in a["arrays"]:
            print(f"   base={arr['base_rva']} count={arr['count']} phase_hits={arr['phase_hits']}")
    print(f"\nRIP refs to phase tables/strings: {len(rip_refs)}")
    print(f"Candidate functions: {len(report['candidate_functions'])}")
    print("\nTop 15 candidates:")
    for c in report["candidate_functions"][:15]:
        s = c["signals"]
        flags = []
        if c["rip_refs_in_func"]: flags.append("RIPREF")
        if s["calls_state_helper_a"] or s["calls_state_helper_b"]: flags.append("HELPER")
        if s["calls_state_poller"]: flags.append("POLL")
        if s["calls_state_applier"]: flags.append("APPLY")
        if s["calls_data_stream_ctor"]: flags.append("DATASTREAM")
        if s["phase_imm_writes"]: flags.append("PHASEIMM")
        print(f"  {c['func_begin']:>12} callers={c['callers_count']:>3} "
              f"callees={c['callees_count']:>3} [{'/'.join(flags) or '-'}]")
    print(f"\nfull report -> {OUT}")


if __name__ == "__main__":
    main()
