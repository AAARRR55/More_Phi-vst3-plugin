#!/usr/bin/env python3
"""
State-machine pivot: the phase strings are unreferenced by linear-sweep (they
live in a struct/jump-table reached indirectly). But the assistant state
machine is driven by helpers 0xEAD2E0 (cmp r8,0x16) and 0xEAD360 (cmp r8,3),
which take a STATE INDEX in r8.

The TRIGGER is the function that:
  - calls a state helper to SET state = LISTENING (first phase), AND
  - is reachable from the GUI/Play-button handler (few callers), AND
  - kicks off analysis (rich callees).

This probe:
  1. Finds ALL callers of the state helpers (0xEAD2E0, 0xEAD360) and the
     poller/apply anchors (0xEAD3E0, 0xEAD930) in the WHOLE binary.
  2. Intersects with the Master Assistant cluster + scans for the function
     that calls a state helper with the LISTENING index (r8 = listening enum).
  3. Disassembles each state-helper caller to capture the r8 value passed --
     the one passing the LISTENING index right after a setup is the trigger.
  4. Also re-scans .text with FULL capstone linear-sweep to finally resolve
     the phase-string xrefs (catching any modrm form the byte-scan missed).

Offline, read-only. No attach, no execution.
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
from capstone.x86 import X86_OP_MEM, X86_OP_REG, X86_OP_IMM, X86_REG_RIP, X86_REG_R8, X86_REG_R8D

DLL = r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll"
IB = 0x180000000
OUT = Path(__file__).with_name("probe_trigger_report.json")

STATE_HELPERS = {
    0x0EAD2E0: "state_helper_a (cmp 0x16)",
    0x0EAD360: "state_helper_b (cmp 3)",
    0x0EAD3E0: "state_poller",
    0x0EAD930: "apply_secondary",
    0x166CA90: "assistant_caller",
}

MA_CLUSTER = set([
    0x1661430, 0x1669980, 0x166e520, 0x168a390, 0x168c5e0, 0x168d590,
    0x168dfc0, 0x168e470, 0x16968d0, 0x169bb20, 0xc6c0a0, 0xd53360,
    0xd54490, 0xd56600, 0xd58a20, 0xe02420, 0xe02f30, 0xe03740,
])

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
    # Also record call sites (src RVA -> tgt) so we can find the insn before.
    call_sites = defaultdict(list)  # tgt -> [src_rva,...]
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
        call_sites[tgt].append(src)
    return callers, callees, call_sites


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


def analyze_state_helper_calls(pe, data, funcs, begins, call_sites):
    """For every call site targeting a state helper, find the immediately
    preceding instruction that sets r8 (state index) and record its value.

    The assistant state machine: helper_a compares r8 vs 0x16 (22 states),
    helper_b compares r8 vs 3. The caller passes the desired state in r8.
    We want the caller that sets r8 to the LISTENING index.
    """
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    results = defaultdict(list)  # helper_rva -> list of {caller_fn, call_rva, r8_value}
    for helper in (0x0EAD2E0, 0x0EAD360):
        for site_rva in call_sites.get(helper, []):
            cf = function_of(funcs, begins, site_rva)
            if not cf:
                continue
            begin, end = cf
            # Disassemble the window before the call to find r8 set.
            off0 = rva_to_off(pe, begin)
            if off0 is None:
                continue
            fsize = min(end - begin, 0x3000)
            code = data[off0:off0 + fsize]
            insns = list(md.disasm(code, IB + begin))
            r8_val = None
            r8_set_rva = None
            # Walk forward; track last assignment to r8/r8d before the call site.
            site_addr = IB + site_rva
            for idx, ins in enumerate(insns):
                if ins.address >= site_addr:
                    break
                # Detect: mov r8, imm  /  mov r8d, imm  / lea r8, [imm]
                if ins.mnemonic == "mov" and len(ins.operands) == 2:
                    if (ins.operands[0].type == X86_OP_REG
                            and ins.operands[0].reg in (X86_REG_R8, X86_REG_R8D)
                            and ins.operands[1].type == X86_OP_IMM):
                        r8_val = ins.operands[1].imm
                        r8_set_rva = ins.address - IB
                # lea r8d, [r14+1] patterns => small arithmetic; capture op_str.
                if ins.mnemonic == "lea" and "r8" in ins.op_str.split(",")[0]:
                    r8_set_rva = ins.address - IB
                    # leave r8_val None; record the expr
            results[helper].append({
                "caller_fn": "0x%x" % begin,
                "call_site": "0x%x" % site_rva,
                "r8_value": (hex(r8_val) if r8_val is not None else None),
                "r8_set_rva": ("0x%x" % r8_set_rva) if r8_set_rva else None,
            })
    return results


def full_phase_scan(pe, data, funcs, begins, image_base, string_rvas):
    """FULL capstone linear-sweep of .text; record every RIP-relative target
    landing on a phase string or its pointer-table qword."""
    text = next((s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text"))
    code = data[text.PointerToRawData:text.PointerToRawData + text.SizeOfRawData]
    t0 = text.VirtualAddress

    target_set = set()
    name_targets = {}
    for name, rvas in string_rvas.items():
        ts = set(rvas)
        for sr in rvas:
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
        target_set |= ts

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    # Linear-sweep whole .text in chunks; capstone recovers from data islands
    # because we only care about RIP-rel operands (robust to occasional mis-sync).
    user_funcs = defaultdict(set)  # phase_name -> {func_begin}
    CHUNK = 0x100000
    total = len(code)
    pos = 0
    while pos < total:
        chunk = code[pos:pos + CHUNK]
        for ins in md.disasm(chunk, IB + t0 + pos):
            for op in ins.operands:
                if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                    tgt = ins.address + ins.size + op.mem.disp
                    trva = tgt - IB
                    if trva in target_set:
                        sf = function_of(funcs, begins, ins.address - IB)
                        if sf:
                            for nm, ts in name_targets.items():
                                if trva in ts:
                                    user_funcs[nm].add(sf[0])
        pos += CHUNK
    return user_funcs


def main():
    print(f"[*] reading {DLL}")
    data = Path(DLL).read_bytes()
    pe = pefile.PE(DLL, fast_load=True)
    pe.parse_data_directories()
    image_base = pe.OPTIONAL_HEADER.ImageBase
    funcs, begins = parse_functions(pe, data)
    callers, callees, call_sites = build_call_graph(pe, data, funcs, begins)
    print(f"[*] {len(funcs)} fns; callgraph + call sites built")

    string_rvas = find_string_rvas(pe, data, PHASE_STRINGS)

    # 1. FULL phase-string scan over entire .text.
    print("[*] full capstone linear-sweep of .text for phase refs ...")
    phase_users = full_phase_scan(pe, data, funcs, begins, image_base, string_rvas)
    print(f"[*] phase-string user funcs:")
    for nm, fns in phase_users.items():
        print(f"      {nm}: {len(fns)} funcs -> {sorted('0x%x'%f for f in fns)[:20]}")

    # 2. State-helper call analysis (captures r8 = state index).
    print("\n[*] analyzing state-helper call sites ...")
    sh_analysis = analyze_state_helper_calls(pe, data, funcs, begins, call_sites)
    for helper, sites in sh_analysis.items():
        print(f"   {STATE_HELPERS[helper]}: {len(sites)} call sites")
        # Tabulate r8 values.
        by_val = defaultdict(list)
        for s in sites:
            by_val[s["r8_value"]].append(s["caller_fn"])
        for val, cfs in sorted(by_val.items(), key=lambda x: (x[0] is None, x[0] or "")):
            uniq = sorted(set(cfs))
            print(f"        r8={val}: {len(uniq)} caller funcs -> "
                  f"{['0x'+format(int(u,16),'x') for u in uniq][:15]}")

    # 3. Rank trigger candidates.
    candidate_pool = set()
    for fns in phase_users.values():
        candidate_pool |= fns
    candidate_pool |= MA_CLUSTER
    # Callers of any state helper (these directly drive the state machine).
    for helper in (0x0EAD2E0, 0x0EAD360, 0x0EAD3E0, 0x0EAD930):
        candidate_pool |= callers.get(helper, set())

    rows = []
    for fb in candidate_pool:
        f = function_of(funcs, begins, fb)
        if not f:
            continue
        cs = callees.get(fb, set())
        cr = callers.get(fb, set())
        reasons = []
        score = 0
        phase_hits = []
        for nm, fns in phase_users.items():
            if fb in fns:
                phase_hits.append(nm)
        if phase_hits:
            score += 6
            reasons.append(f"refs-phase({phase_hits})")
        if fb in MA_CLUSTER:
            score += 2
            reasons.append("master-assistant-cluster")
        sh_calls = cs & set(STATE_HELPERS.keys())
        if sh_calls:
            score += 3
            reasons.append(f"calls-state({sorted(STATE_HELPERS[s] for s in sh_calls)})")
        # Does this fn call BOTH a state setter AND assistant_caller (orchestration)?
        if 0x166CA90 in cs:
            score += 3
            reasons.append("calls-assistant_caller(orchestrator)")
        # GUI-entry signature.
        if 0 < len(cr) <= 3 and len(cs) >= 6:
            score += 2
            reasons.append(f"sparse-caller/rich-callee({len(cr)}/{len(cs)})")
        if len(cr) == 0 and len(cs) >= 6:
            score += 2
            reasons.append(f"no-caller/root-rich-callee({len(cs)})")
        if not reasons:
            continue
        rows.append({
            "func_begin": "0x%x" % fb,
            "func_end": "0x%x" % f[1],
            "size": f[1] - f[0],
            "score": score,
            "reasons": reasons,
            "callers": ["0x%x" % c for c in sorted(cr)][:15],
            "callers_count": len(cr),
            "callees_count": len(cs),
            "callees": ["0x%x" % c for c in sorted(cs)][:50],
            "phase_refs": phase_hits,
            "state_helper_calls": sorted(STATE_HELPERS[s] for s in sh_calls),
        })
    rows.sort(key=lambda r: (-r["score"], -r["callees_count"], r["func_begin"]))

    report = {
        "dll": DLL,
        "phase_users": {nm: sorted("0x%x" % f for f in fns)
                        for nm, fns in phase_users.items()},
        "state_helper_call_sites": {("0x%x" % k): v
                                    for k, v in sh_analysis.items()},
        "candidates": rows[:30],
    }
    OUT.write_text(json.dumps(report, indent=1), encoding="utf-8")
    print(f"\n[+] report -> {OUT}")
    print(f"[+] top 18 trigger candidates:")
    for r in rows[:18]:
        print(f"   score={r['score']:>2} fn={r['func_begin']} size={r['size']:>5} "
              f"callers={r['callers_count']} callees={r['callees_count']} "
              f"{r['reasons']}")

    # 4. Disasm top candidates that look like a trigger (call state helper +
    #    few callers). Show how r8 is set before the state-helper call.
    print("\n[+] disasm of top orchestrator candidates (showing state-helper calls):")
    md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = False
    shown = 0
    for r in rows:
        if shown >= 6:
            break
        if "calls-state" not in str(r["reasons"]):
            continue
        shown += 1
        fb = int(r["func_begin"], 16)
        f = function_of(funcs, begins, fb)
        begin, end = f
        off0 = rva_to_off(pe, begin)
        code = data[off0:off0 + min(end - begin, 0x1800)]
        print(f"\n=== {r['func_begin']}  (score {r['score']}, {r['reasons']}) ===")
        insns = list(md.disasm(code, IB + begin))
        for ins in insns:
            rva = ins.address - IB
            op = ins.op_str
            note = ""
            if ins.mnemonic == "call" and op.startswith("0x"):
                try:
                    tgt = int(op, 16) - IB
                    if tgt in STATE_HELPERS:
                        note = f"  ; -> {STATE_HELPERS[tgt]}"
                except ValueError:
                    pass
            if "rip" in op:
                for t in re.findall(r"0x[0-9a-f]+", op):
                    try:
                        trva = int(t, 16) - IB
                        s = read_cstr(pe, data, trva)
                        if s and len(s) >= 5 and s.isprintable() and " " in s:
                            note += f"  ; rip->\"{s[:40]}\""
                    except ValueError:
                        pass
            print(f"    {'0x%x'%rva:>10}  {ins.mnemonic:<8} {op}{note}")


if __name__ == "__main__":
    sys.exit(main())
