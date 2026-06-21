#!/usr/bin/env python3
"""
Final triage on the candidate TRIGGER entry.

Finding so far: a single-parent pipeline bottoms out at the POLLER+SECONDARY:
  0xd58a20 (MA-cluster) -> 0xe9fc30 -> 0xea1b20 -> 0xea4d50 -> 0xea4fd0
      -> 0xe9f4f0 -> 0xeab020 -> 0xeabdb0(HUB) -> POLLER + SECONDARY

0xd58a20 references the "Master Assistant" string and is the SOLE caller of the
pipeline root 0xe9fc30. This probe confirms 0xd58a20's signature and who calls
IT (the GUI/IPC surface), and double-checks 0xe9fc30 as the pipeline root.
"""
from __future__ import annotations
import bisect, json, re, sys
from pathlib import Path
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

DLL = r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll"
RECON = Path("tools/live_captures/static/ozone_recon.json")

STATE_POLLER=0x0EAD3E0; SECONDARY=0x0EAD930; DATA_STREAM=0x0FD7F30
HUB=0x0EABDB0; EAB020=0x0EAB020
PIPELINE=[0xE9FC30,0xEA1B20,0xEA4D50,0xEA4FD0,0xE9F4F0,0xEAB020,0xEABDB0]
TRIGGER_CANDIDATES=[0xD58A20, 0xE9FC30, 0xEA1B20, 0xEA4D50]


def rva_to_off(pe,rva):
    for s in pe.sections:
        span=max(s.Misc_VirtualSize,s.SizeOfRawData)
        if s.VirtualAddress<=rva<s.VirtualAddress+span:
            return s.PointerToRawData+(rva-s.VirtualAddress)
    return None


def parse_functions(pe,data):
    pd=next(s for s in pe.sections if s.Name.rstrip(b"\x00")==b".pdata")
    funcs=[]; base=pd.PointerToRawData
    for off in range(base,base+pd.SizeOfRawData-11,12):
        b=int.from_bytes(data[off:off+4],"little"); e=int.from_bytes(data[off+4:off+8],"little")
        if b: funcs.append((b,e))
    funcs.sort(); return funcs,[b for b,_ in funcs]


def main():
    recon=json.loads(RECON.read_text(encoding="utf-8"))
    ma_funcs=set(int(s,16) for s in recon["strings"]["Master Assistant"]["referenced_by_funcs"])

    data=Path(DLL).read_bytes()
    pe=pefile.PE(DLL,fast_load=True); pe.parse_data_directories()
    image_base=pe.OPTIONAL_HEADER.ImageBase
    funcs,begins=parse_functions(pe,data); begins_set=set(begins)

    text=next(s for s in pe.sections if s.Name.rstrip(b"\x00")==b".text")
    tb=data[text.PointerToRawData:text.PointerToRawData+text.SizeOfRawData]; t0=text.VirtualAddress
    callers,callees={},{}
    for m in re.compile(rb"[\xe8\xe9]",re.DOTALL).finditer(tb):
        i=m.start()
        if i+5>len(tb): continue
        rel=int.from_bytes(tb[i+1:i+5],"little",signed=True); s=t0+i; t=s+5+rel
        if t not in begins_set: continue
        ii=bisect.bisect_right(begins,s)-1
        if ii<0: continue
        b,e=funcs[ii]
        if not (b<=s<e): continue
        callees.setdefault(b,set()).add(t); callers.setdefault(t,set()).add(b)

    def endof(fb):
        for b,e in funcs:
            if b==fb: return e
        return None

    def annotate(c):
        tag=""
        if c==STATE_POLLER: tag="POLLER"
        elif c==SECONDARY: tag="SECONDARY(apply)"
        elif c==DATA_STREAM: tag="data_stream_ctor"
        elif c==HUB: tag="HUB"
        elif c in PIPELINE: tag="pipeline"
        elif c in ma_funcs: tag="MA-cluster"
        return tag

    # Verify the pipeline chain is real (each calls the next).
    print("=== PIPELINE verification (each func should call the next) ===")
    for a,b in zip(PIPELINE, PIPELINE[1:]):
        cl=callees.get(a,set())
        print(f"  0x{a:x} -> 0x{b:x} ? {'YES' if b in cl else 'NO'}  "
              f"(0x{a:x} callees: {[hex(x)+('('+annotate(x)+')' if annotate(x) else '') for x in sorted(cl)]})")

    # For each trigger candidate: full report.
    for fb in TRIGGER_CANDIDATES:
        end=endof(fb)
        cl=callees.get(fb,set()); cr=callers.get(fb,set())
        print(f"\n=== 0x{fb:x}  size={end-fb if end else 0}  callers={len(cr)}  callees={len(cl)} ===")
        print(f"   callers: {[hex(x) for x in sorted(cr)]}")
        print(f"   callees: {[(hex(x), annotate(x)) for x in sorted(cl)]}")
        # pipeline reachability
        reach = cl & set(PIPELINE)
        reach_anchors = cl & {STATE_POLLER,SECONDARY,DATA_STREAM,HUB}
        print(f"   calls-pipeline-member: {[hex(x) for x in sorted(reach)]}")
        print(f"   calls-anchor: {[hex(x) for x in sorted(reach_anchors)]}")

    # Climb above 0xd58a20 to the GUI/IPC entry.
    print("\n=== reverse chain from 0xd58a20 (toward GUI entry) ===")
    cur=0xD58A20
    for d in range(5):
        cs=sorted(callers.get(cur,set()))
        if not cs:
            print(f"  {'  '*d}0x{cur:x}  <-- (root)"); break
        print(f"  {'  '*d}0x{cur:x}  callers={len(cs)}: {[hex(x) for x in cs]}")
        if len(cs)==1: cur=cs[0]
        else:
            for c in cs:
                gc=callers.get(c,set())
                print(f"  {'  '*(d+1)}0x{c:x} size={endof(c)-c if endof(c) else 0} callers={len(gc)}: {[hex(x) for x in sorted(gc)]}")
            break

    # Capstone the two strongest candidates: 0xd58a20 (MA-string ref, pipeline root caller)
    # and 0xe9fc30 (pipeline root).
    md=Cs(CS_ARCH_X86,CS_MODE_64); md.detail=True
    def prologue(fb,n=36):
        end=endof(fb)
        if not end: return
        off0=rva_to_off(pe,fb)
        code=data[off0:off0+min(end-fb,0x200)]
        print(f"\n  ;;; 0x{fb:x} (size {end-fb})")
        for k,insn in enumerate(md.disasm(code,image_base+fb)):
            if k>=n: break
            note=""
            if insn.mnemonic in ("call","jmp") and insn.op_str.startswith("0x"):
                t=int(insn.op_str,16)-image_base
                a=annotate(t)
                if a: note=f"  ;->{a}"
            print(f"    0x{insn.address-image_base:x}  {insn.mnemonic:<7} {insn.op_str}{note}")

    print("\n=== CAPSTONE prologues of trigger candidates ===")
    prologue(0xD58A20, 40)
    prologue(0xE9FC30, 36)
    prologue(0xEA1B20, 30)

    return 0

if __name__=="__main__":
    sys.exit(main())
