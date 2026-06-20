#!/usr/bin/env python3
"""
Final consolidation (Strategy D audio-feed subagent): resolve the assistant
state-name tables, confirm the orchestrator 0x166CA90, and trace its callers
to identify the GUI "Play" entry. Offline, read-only on the DLL.
"""
from __future__ import annotations
import bisect, json, re, sys
from collections import defaultdict
from pathlib import Path
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

DLL = r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll"
IB = 0x180000000
OUT = Path(__file__).with_name("probe_stratD_consolidate_report.json")


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


def main():
    data = Path(DLL).read_bytes()
    pe = pefile.PE(DLL, fast_load=True)
    pe.parse_data_directories()
    funcs, begins = parse_functions(pe, data)

    # The LEA at 0xead9d9 'lea r13,[rip - 0xead9e0]' sets r13 = image base.
    # Tables: [r13 + idx*4 + 0x2682550] and [.. + 0x2682518]. These hold
    # 32-bit enum IDs (mov eax,[...]) used as state values, NOT pointers.
    print("[*] state-name/enum tables referenced by apply_secondary 0xead930:")
    for tname, trva in [("A(0x2682550)", 0x2682550), ("B(0x2682518)", 0x2682518)]:
        off = rva_to_off(pe, trva)
        vals = []
        if off is not None:
            for i in range(24):
                vals.append(int.from_bytes(data[off+i*4:off+i*4+4], "little"))
        print(f"   {tname}: {vals[:22]}")

    # Find the contiguous 19-entry PROCESSING_LISTENING pointer array.
    pl_vas = set()
    nb = b"PROCESSING_LISTENING"
    st = 0
    while True:
        i = data.find(nb, st)
        if i < 0:
            break
        r = off_to_rva(pe, i)
        if r is not None:
            pl_vas.add(IB + r)
        st = i + 1
    best = (0, None)
    for off in range(0, len(data) - 8*20, 8):
        run = 0
        for k in range(20):
            q = int.from_bytes(data[off+k*8:off+k*8+8], "little")
            if q in pl_vas:
                run += 1
            else:
                break
        if run > best[0]:
            best = (run, off)
    print(f"\n[*] largest contiguous PROCESSING_LISTENING ptr-array: {best[0]} entries")
    if best[1] is not None:
        r = off_to_rva(pe, best[1])
        print(f"    @ rva 0x{r:x}")

    # Build caller graph.
    text = next((s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text"))
    tcode = data[text.PointerToRawData:text.PointerToRawData + text.SizeOfRawData]
    t0 = text.VirtualAddress
    bs = set(begins)
    callers = defaultdict(set)
    for m in re.compile(rb"[\xe8\xe9]").finditer(tcode):
        i = m.start()
        if i + 5 > len(tcode):
            continue
        rel = int.from_bytes(tcode[i+1:i+5], "little", signed=True)
        src = t0 + i
        tgt = src + 5 + rel
        if tgt not in bs:
            continue
        sf = function_of(funcs, begins, src)
        if sf:
            callers[tgt].add(sf[0])

    # Orchestrator ancestry.
    ORCH = 0x166ca90
    print(f"\n[*] ancestry of orchestrator 0x{ORCH:x}:")
    cur = {ORCH}; seen = {ORCH}
    for lvl in range(1, 5):
        nxt = set()
        for fn in cur:
            for c in callers.get(fn, set()):
                if c not in seen:
                    nxt.add(c); seen.add(c)
        if not nxt:
            print(f"   L{lvl}: (none -- reached root)")
            break
        print(f"   L{lvl}: {sorted('0x%x'%c for c in nxt)}")
        cur = nxt

    # Disasm orchestrator callers + their own callers (the GUI entry).
    md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = False
    anchors = {0x166CA90: "ORCH", 0x0EAD3E0: "poller", 0x0EAD930: "apply",
               0x0EAD2E0: "helper_a", 0x0EAD360: "helper_b"}
    targets = sorted(callers.get(ORCH, set()))
    print(f"\n[*] disasm of orchestrator callers {['0x%x'%t for t in targets]}:")
    for fb in targets:
        f = function_of(funcs, begins, fb)
        if not f:
            continue
        begin, end = f
        off0 = rva_to_off(pe, begin)
        code = data[off0:off0 + min(end - begin, 0x500)]
        print(f"\n=== 0x{fb:x} size={end-begin} callers={sorted('0x%x'%c for c in callers.get(fb,set()))} ===")
        for ins in md.disasm(code, IB + begin):
            rva = ins.address - IB
            op = ins.op_str
            note = ""
            if ins.mnemonic == "call" and op.startswith("0x"):
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
                        s = read_cstr(pe, data, trva)
                        if s and len(s) >= 5 and s.isprintable():
                            note += f"  ; rip->\"{s[:45]}\""
                    except ValueError:
                        pass
            print(f"    {'0x%x'%rva:>10}  {ins.mnemonic:<8} {op}{note}")

    OUT.write_text(json.dumps({"pl_array_max_run": best[0],
                               "pl_array_rva": ("0x%x" % off_to_rva(pe, best[1])) if best[1] else None},
                              indent=1), encoding="utf-8")
    print(f"\n[+] {OUT}")


if __name__ == "__main__":
    sys.exit(main())
