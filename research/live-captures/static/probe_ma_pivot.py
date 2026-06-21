#!/usr/bin/env python3
"""
Pivot probe: the +0xFD7F30 audio ctor is the DYNAMICS SmoothAudioDataStream
(string "Dynamics Smooth DataStream"), NOT the Master Assistant feed.
So Strategy D's anchor is a misattribution.

This probe finds the real Master Assistant TRIGGER by:
  1. Taking the 18-func "Master Assistant" code-ref cluster.
  2. Finding which of them call the state-machine helpers
     (0xEAD2E0/0xEAD360/0xEAD3E0/0xEAD930) -- the assistant state path.
  3. Finding which has FEW callers (GUI entry) + MANY callees + sets state.
  4. Tracing backwards from each Master Assistant func to find a common root
     that the GUI "Play" handler would invoke.
  5. Re-scanning .text for phase-state strings via a BROADER RIP pattern (the
     table is referenced by something other than a 7-byte REX.W LEA; we now
     scan for ALL RIP-relative modrm forms incl. non-REX and MOVREG).

Offline, read-only on the DLL. No attach, no execution.
"""

from __future__ import annotations

import bisect
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

DLL = r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll"
IB = 0x180000000
OUT = Path(__file__).with_name("probe_ma_pivot_report.json")

STATE_HELPERS = {
    0x0EAD2E0: "state_helper_a",
    0x0EAD360: "state_helper_b",
    0x0EAD3E0: "state_poller",
    0x0EAD930: "apply_secondary",
    0x166CA90: "assistant_caller",
}

MA_CLUSTER = [
    0x1661430, 0x1669980, 0x166e520, 0x168a390, 0x168c5e0, 0x168d590,
    0x168dfc0, 0x168e470, 0x16968d0, 0x169bb20, 0xc6c0a0, 0xd53360,
    0xd54490, 0xd56600, 0xd58a20, 0xe02420, 0xe02f30, 0xe03740,
]

PHASE_STRINGS = [
    "PROCESSING_LISTENING",
    "LEARNING_EQ_AND_CLASSIFYING_GENRE",
    "PROCESSING_SETTING_SIGNAL_CHAIN",
]


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


def find_string_rvas(pe, data, needles):
    out = {}
    for needle in needles:
        nb = needle.encode("latin1")
        rvas, start = [], 0
        while True:
            i = data.find(nb, start)
            if i < 0:
                break
            r = off_to_rva(pe, i)
            if r is not None:
                rvas.append(r)
            start = i + 1
        out[needle] = rvas
    return out


def find_phase_table_and_leas(pe, data, funcs, begins, image_base, string_rvas):
    """Find pointer-table qwords for a phase string AND every RIP-relative insn
    (ANY length) that references each table qword.

    Broader than the 7-byte REX.W scan: also matches non-REX (8d/8b/89/3b..) and
    other RIP modrm opcodes by linear-sweep disassembling each .text function.
    """
    text = next((s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text"))
    code = data[text.PointerToRawData:text.PointerToRawData + text.SizeOfRawData]
    t0 = text.VirtualAddress

    # Build the set of pointer-table entry RVAs (qwords holding string VA).
    table_rvas = set()
    for rvas in string_rvas.values():
        for s_rva in rvas:
            needle = (image_base + s_rva).to_bytes(8, "little")
            start = 0
            while True:
                i = data.find(needle, start)
                if i < 0:
                    break
                pr = off_to_rva(pe, i)
                if pr is not None:
                    table_rvas.add(pr)
                start = i + 1
    # Also include the string RVAs themselves as targets.
    for rvas in string_rvas.values():
        table_rvas |= set(rvas)
    return table_rvas


def rip_refs_via_capstone(pe, data, funcs, begins, fb, target_set):
    """Disassemble function fb linearly; collect every RIP-relative effective
    address that lands in target_set. Returns set of target RVAs hit."""
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    from capstone.x86 import X86_OP_MEM, X86_REG_RIP
    f = function_of(funcs, begins, fb)
    if not f:
        return set()
    begin, end = f
    off0 = rva_to_off(pe, begin)
    if off0 is None:
        return set()
    code = data[off0:off0 + min(end - begin, 0x2000)]
    hits = set()
    for insn in md.disasm(code, IB + begin):
        for op in insn.operands:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                tgt = insn.address + insn.size + op.mem.disp
                trva = tgt - IB
                if trva in target_set:
                    hits.add(trva)
    return hits


def read_cstr(pe, data, rva, maxlen=80):
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


def disasm_annotated(pe, data, funcs, begins, fb, anchors, max_insns=80):
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = False
    f = function_of(funcs, begins, fb)
    if not f:
        return []
    begin, end = f
    off0 = rva_to_off(pe, begin)
    code = data[off0:off0 + min(end - begin, 0x1000)]
    rows = []
    for k, insn in enumerate(md.disasm(code, IB + begin)):
        if k >= max_insns:
            break
        rva = insn.address - IB
        op = insn.op_str
        note = ""
        if insn.mnemonic == "call" and op.startswith("0x"):
            try:
                tgt = int(op, 16) - IB
                if tgt in anchors:
                    note = f"  ; -> {anchors[tgt]}"
            except ValueError:
                pass
        if "rip" in op:
            for t in re.findall(r"0x[0-9a-f]+", op):
                try:
                    trva = int(t, 16) - IB
                    if trva in anchors:
                        note += f"  ; rip->{anchors[trva]}"
                    else:
                        s = read_cstr(pe, data, trva)
                        if s and len(s) >= 4 and s.isprintable() and " " in s:
                            note += f"  ; rip->\"{s}\""
                except ValueError:
                    pass
        rows.append({"rva": "0x%x" % rva, "mnemonic": insn.mnemonic,
                     "op_str": op, "note": note})
    return rows


def main():
    print(f"[*] reading {DLL}")
    data = Path(DLL).read_bytes()
    pe = pefile.PE(DLL, fast_load=True)
    pe.parse_data_directories()
    image_base = pe.OPTIONAL_HEADER.ImageBase
    funcs, begins = parse_functions(pe, data)
    callers, callees = build_call_graph(pe, data, funcs, begins)
    print(f"[*] {len(funcs)} fns; callgraph built")

    string_rvas = find_string_rvas(pe, data, PHASE_STRINGS)
    table_target_set = find_phase_table_and_leas(
        pe, data, funcs, begins, image_base, string_rvas)
    print(f"[*] phase string ptr-table + string target RVAs: {len(table_target_set)}")

    # --- Which Master Assistant cluster funcs reference phase strings/tables? ---
    ma_phase_users = []
    for fb in MA_CLUSTER:
        hits = rip_refs_via_capstone(pe, data, funcs, begins, fb, table_target_set)
        if hits:
            ma_phase_users.append((fb, hits))

    # Also scan a broader neighborhood: the 0x16xxxxx GUI/assistant range for
    # any function (not just the 18) that references a phase table/string.
    # That range is where the assistant subsystem lives.
    phase_funcs = defaultdict(set)  # fb -> {phase string names}
    # Build a name->targetset map.
    name_targets = {}
    for name, rvas in string_rvas.items():
        ts = set()
        for sr in rvas:
            ts.add(sr)
            needle = (image_base + sr).to_bytes(8, "little")
            st = 0
            while True:
                i = data.find(needle, st)
                if i < 0:
                    break
                pr = off_to_rva(pe, i)
                if pr is not None:
                    ts.add(pr)
                st = i + 1
        name_targets[name] = ts

    # Scan ALL functions whose begin is in the 0x16xxxxx range (GUI/assistant).
    guirange = [b for b in begins if 0x1600000 <= b < 0x16b0000]
    print(f"[*] scanning {len(guirange)} funcs in 0x1600000-0x16b0000 for phase refs")
    for fb in guirange:
        hits = rip_refs_via_capstone(pe, data, funcs, begins, fb, table_target_set)
        if not hits:
            continue
        names = set()
        for h in hits:
            for nm, ts in name_targets.items():
                if h in ts:
                    names.add(nm)
        if names:
            phase_funcs[fb] |= names

    # Also scan the 0x0E0000-0x0EF0000 range (the state-helper neighborhood).
    ehbrange = [b for b in begins if 0x0E00000 <= b < 0x0EF0000]
    print(f"[*] scanning {len(ehbrange)} funcs in 0x0E00000-0x0EF0000 for phase refs")
    for fb in ehbrange:
        hits = rip_refs_via_capstone(pe, data, funcs, begins, fb, table_target_set)
        if not hits:
            continue
        names = set()
        for h in hits:
            for nm, ts in name_targets.items():
                if h in ts:
                    names.add(nm)
        if names:
            phase_funcs[fb] |= names

    print(f"\n[*] functions referencing phase strings/tables: {len(phase_funcs)}")
    for fb, names in sorted(phase_funcs.items()):
        cs = callees.get(fb, set())
        cr = callers.get(fb, set())
        state_calls = cs & set(STATE_HELPERS.keys())
        print(f"   0x{fb:x}  phase={sorted(names)}  callers={len(cr)} "
              f"callees={len(cs)}  state-calls={sorted(STATE_HELPERS[s] for s in state_calls)}")

    # --- Score candidate triggers from the phase-referencing funcs + MA cluster ---
    candidate_set = set(phase_funcs.keys()) | set(MA_CLUSTER)
    # Add funcs that call a state helper (these touch the assistant state machine).
    for fb in list(candidate_set):
        candidate_set |= (callees.get(fb, set()) & set(STATE_HELPERS.keys()))
    # And funcs whose callee set includes a phase-referencing func.
    for pf in phase_funcs:
        for maybe_caller_fb, cs in callees.items():
            if pf in cs:
                candidate_set.add(maybe_caller_fb)

    rows = []
    for fb in candidate_set:
        f = function_of(funcs, begins, fb)
        if not f:
            continue
        cs = callees.get(fb, set())
        cr = callers.get(fb, set())
        reasons = []
        score = 0
        if fb in phase_funcs:
            score += 5
            reasons.append(f"refs-phase({sorted(phase_funcs[fb])})")
        if fb in MA_CLUSTER:
            score += 2
            reasons.append("master-assistant-cluster")
        state_calls = cs & set(STATE_HELPERS.keys())
        if state_calls:
            score += 3
            reasons.append(f"calls-state({sorted(STATE_HELPERS[s] for s in state_calls)})")
        # Trigger signature: few callers (GUI), many callees, references Master Assistant state.
        if len(cr) <= 3 and len(cs) >= 8:
            score += 3
            reasons.append(f"sparse-caller/rich-callee({len(cr)}/{len(cs)})")
        if not reasons:
            continue
        rows.append({
            "func_begin": "0x%x" % fb,
            "func_end": "0x%x" % f[1],
            "size": f[1] - f[0],
            "score": score,
            "reasons": reasons,
            "callers": ["0x%x" % c for c in sorted(cr)],
            "callers_count": len(cr),
            "callees_count": len(cs),
            "callees": ["0x%x" % c for c in sorted(cs)][:50],
            "calls_state_helpers": sorted(STATE_HELPERS[s] for s in state_calls),
        })
    rows.sort(key=lambda r: (-r["score"], -r["callees_count"], r["func_begin"]))

    report = {
        "dll": DLL,
        "phase_funcs": [{"func": "0x%x" % k, "phases": sorted(v)}
                        for k, v in sorted(phase_funcs.items())],
        "ma_cluster_phase_users": [{"func": "0x%x" % k, "hits": len(h)}
                                   for k, h in ma_phase_users],
        "candidates": rows[:25],
    }
    OUT.write_text(json.dumps(report, indent=1), encoding="utf-8")
    print(f"\n[+] report -> {OUT}")
    print(f"[+] top {min(15, len(rows))} candidates:")
    for r in rows[:15]:
        print(f"   score={r['score']:>2} fn={r['func_begin']} size={r['size']:>5} "
              f"callers={r['callers_count']} callees={r['callees_count']} "
              f"{r['reasons']}")

    # Disasm top 5 that reference phase strings.
    print("\n[+] disasm of top phase-referencing candidates:")
    shown = 0
    for r in rows:
        if shown >= 5:
            break
        if not any("refs-phase" in x for x in r["reasons"]):
            continue
        shown += 1
        fb = int(r["func_begin"], 16)
        print(f"\n=== {r['func_begin']}  (score {r['score']}, {r['reasons']}) ===")
        for row in disasm_annotated(pe, data, funcs, begins, fb, STATE_HELPERS, 50):
            print(f"    {row['rva']:>10}  {row['mnemonic']:<8} {row['op_str']}{row['note']}")


if __name__ == "__main__":
    sys.exit(main())
