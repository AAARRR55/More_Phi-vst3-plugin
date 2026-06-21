#!/usr/bin/env python3
"""
Climb the caller chain upward from the HUB +0xEABDB0 to find the assistant's
TRIGGER entry point. Also builds the complete reverse-call-tree around both
known common callers (HUB +0xEABDB0 and documented CALLER +0x166CA90) so we can
see which root is reachable from the GUI/IPC layer.

OFFLINE, file-read only. OZONE_IPC_RESEARCH_METHODOLOGY.md sec 5.2 scope.
"""
from __future__ import annotations
import bisect, json, re, sys
from pathlib import Path
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

DLL = r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll"
RECON = Path("tools/live_captures/static/ozone_recon.json")

STATE_POLLER   = 0x0EAD3E0
SECONDARY      = 0x0EAD930
DATA_STREAM    = 0x0FD7F30
CALLER         = 0x166CA90
HUB            = 0x0EABDB0
DS_CALLER_A    = 0x0FBD0B0
DS_CALLER_B    = 0x1072860
EAB020         = 0x0EAB020


def rva_to_off(pe, rva):
    for s in pe.sections:
        span = max(s.Misc_VirtualSize, s.SizeOfRawData)
        if s.VirtualAddress <= rva < s.VirtualAddress + span:
            return s.PointerToRawData + (rva - s.VirtualAddress)
    return None


def parse_functions(pe, data):
    pd = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".pdata")
    funcs = []
    base = pd.PointerToRawData
    for off in range(base, base + pd.SizeOfRawData - 11, 12):
        b = int.from_bytes(data[off:off+4],"little"); e=int.from_bytes(data[off+4:off+8],"little")
        if b: funcs.append((b,e))
    funcs.sort(); return funcs, [b for b,_ in funcs]


def main():
    recon = json.loads(RECON.read_text(encoding="utf-8"))
    ma_funcs = set(int(s,16) for s in recon["strings"]["Master Assistant"]["referenced_by_funcs"])

    data = Path(DLL).read_bytes()
    pe = pefile.PE(DLL, fast_load=True); pe.parse_data_directories()
    image_base = pe.OPTIONAL_HEADER.ImageBase
    funcs, begins = parse_functions(pe, data)
    begins_set = set(begins)

    text = next(s for s in pe.sections if s.Name.rstrip(b"\x00")==b".text")
    tb = data[text.PointerToRawData:text.PointerToRawData+text.SizeOfRawData]
    t0 = text.VirtualAddress
    callers, callees = {}, {}
    for m in re.compile(rb"[\xe8\xe9]", re.DOTALL).finditer(tb):
        i=m.start()
        if i+5>len(tb): continue
        rel=int.from_bytes(tb[i+1:i+5],"little",signed=True)
        s=t0+i; t=s+5+rel
        if t not in begins_set: continue
        # resolve src func
        ii = bisect.bisect_right(begins, s) - 1
        if ii<0: continue
        b,e=funcs[ii]
        if not (b<=s<e): continue
        callees.setdefault(b,set()).add(t)
        callers.setdefault(t,set()).add(b)

    def endof(fb):
        for b,e in funcs:
            if b==fb: return e
        return None

    def climb(root, depth=6):
        print(f"\n### reverse chain from 0x{root:x}")
        cur = root
        for d in range(depth):
            cs = sorted(callers.get(cur, set()))
            if not cs:
                print(f"  {'  '*d}0x{cur:x}  <-- (NO callers / root)")
                break
            print(f"  {'  '*d}0x{cur:x}  callers={len(cs)}: {[hex(x) for x in cs]}")
            if len(cs) == 1:
                cur = cs[0]
            else:
                for c in cs:
                    gc = callers.get(c, set())
                    print(f"  {'  '*(d+1)}0x{c:x} (size {endof(c)-c if endof(c) else 0}, "
                          f"callers={len(gc)}: {[hex(x) for x in sorted(gc)]})")
                break

    climb(HUB)
    climb(CALLER)
    climb(EAB020)

    for fb in (EAB020, HUB):
        cl = callees.get(fb, set())
        print(f"\n=== callees of 0x{fb:x} ({len(cl)}) ===")
        for c in sorted(cl):
            tag=""
            if c==STATE_POLLER: tag="; POLLER"
            elif c==SECONDARY: tag="; SECONDARY"
            elif c==DATA_STREAM: tag="; data_stream_ctor"
            elif c==DS_CALLER_A: tag="; ->ds_caller_a (audio ingest)"
            elif c==DS_CALLER_B: tag="; ->ds_caller_b (audio ingest)"
            elif c in ma_funcs: tag="; MA-cluster"
            print(f"   0x{c:x}  {tag}")

    audio_tree = {DATA_STREAM, DS_CALLER_A, DS_CALLER_B}
    for c in list(audio_tree):
        audio_tree |= callers.get(c, set())
    control_tree = {HUB, EAB020, STATE_POLLER, SECONDARY, CALLER}
    print(f"\naudio subtree (data_stream + 1-hop callers): {[hex(x) for x in sorted(audio_tree)]}")

    bridges = []
    for f, cl in callees.items():
        if (cl & control_tree) and (cl & audio_tree):
            bridges.append((f, sorted(cl & control_tree), sorted(cl & audio_tree)))
    print(f"\n=== BRIDGE funcs (call into BOTH control + audio subtrees): {len(bridges)} ===")
    bridges.sort(key=lambda x: (len(callers.get(x[0],set())), endof(x[0])-x[0] if endof(x[0]) else 0))
    for f, ctl, aud in bridges[:25]:
        print(f"   0x{f:x}  size={endof(f)-f if endof(f) else 0}  "
              f"callers={len(callers.get(f,set()))}  ctl={[hex(x) for x in ctl]}  audio={[hex(x) for x in aud]}")

    md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail=True
    def prologue(fb, n=30):
        end = endof(fb)
        if not end: return
        off0 = rva_to_off(pe, fb)
        code = data[off0:off0+min(end-fb, 0x180)]
        print(f"\n  ;;; prologue 0x{fb:x} (size {end-fb})")
        for k,insn in enumerate(md.disasm(code, image_base+fb)):
            if k>=n: break
            note=""
            if insn.mnemonic in ("call","jmp") and insn.op_str.startswith("0x"):
                t=int(insn.op_str,16)-image_base
                if t==HUB: note="  ;->HUB"
                elif t==EAB020: note="  ;->EAB020"
                elif t==STATE_POLLER: note="  ;->POLLER"
                elif t==SECONDARY: note="  ;->SECONDARY"
                elif t==DATA_STREAM: note="  ;->data_stream_ctor"
                elif t in audio_tree: note="  ;->audio"
                elif t in ma_funcs: note="  ;->MA-cluster"
            print(f"    0x{insn.address-image_base:x}  {insn.mnemonic:<7} {insn.op_str}{note}")

    print("\n=== CAPSTONE prologues ===")
    prologue(EAB020, 34)
    for f,_,_ in bridges[:3]:
        prologue(f, 30)

    return 0

if __name__=="__main__":
    sys.exit(main())
