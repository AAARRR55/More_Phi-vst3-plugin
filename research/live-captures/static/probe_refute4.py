#!/usr/bin/env py
# REFUTATION PROBE #4 — (a) verify the controller-deref crash hazards,
# (b) check whether 0xD50740's table @rdi could be the phase-string table.
# OFFLINE file-read only.
import struct, bisect
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

DLL = r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll"
IMAGE_BASE = 0x180000000
data_all=open(DLL,"rb").read()
pe=pefile.PE(DLL,fast_load=True); pe.parse_data_directories()
pd=next(s for s in pe.sections if s.Name.rstrip(b"\x00")==b".pdata")
funcs=[]; base=pd.PointerToRawData
for off in range(base,base+pd.SizeOfRawData-11,12):
    b=int.from_bytes(data_all[off:off+4],"little"); e=int.from_bytes(data_all[off+4:off+8],"little")
    if b: funcs.append((b,e))
funcs.sort(); begins=sorted(b for b,_ in funcs)
def function_of(rva):
    i=bisect.bisect_right(begins,rva)-1
    if i<0: return None
    b,e=funcs[i]; return (b,e) if b<=rva<e else None
def rva_to_off(rva):
    for s in pe.sections:
        span=max(s.Misc_VirtualSize,s.SizeOfRawData)
        if s.VirtualAddress<=rva<s.VirtualAddress+span:
            return s.PointerToRawData+(rva-s.VirtualAddress)
    return None
def read(rva,n):
    o=rva_to_off(rva); return data_all[o:o+n] if o is not None else b""
def read_cstr(rva,n=80):
    d=read(rva,n); i=d.find(b"\x00")
    try: return d[:i].decode("ascii","replace") if i>=0 else None
    except: return None
md=Cs(CS_ARCH_X86,CS_MODE_64); md.detail=True
CAND=0xD58A20; STEPPER=0xD50740

# === A. Enumerate all `this`-pointer derefs in 0xD58A20 to map crash hazards. ===
# The candidate claims: reads [vtable_obj+0x90], writes [+0x98]/[+0xA0].
# Verify by scanning the body for [rbx+...] (rbx = [rcx] = vtable/subobject).
fb,fe=function_of(CAND); body=read(CAND,fe-CAND)
print(f"=== this-deref map in 0xD58A20 (rbx = [rcx]) ===")
ins_list=list(md.disasm(body,IMAGE_BASE+CAND))
rbx_derefs={}
for ins in ins_list:
    op=ins.op_str
    # find [rbx + 0xNN] or [rbx]
    import re
    for m in re.finditer(r"\[rbx(?: \+ (0x[0-9a-fA-F]+))?\]", op):
        disp=m.group(1)
        disp=int(disp,16) if disp else 0
        rbx_derefs.setdefault(disp,[]).append((ins.address-IMAGE_BASE,ins.mnemonic,op))
print(f"distinct [rbx+disp] offsets touched: {len(rbx_derefs)}")
for d in sorted(rbx_derefs)[:30]:
    sites=rbx_derefs[d]
    print(f"  +0x{d:x}: {len(sites)} site(s); first: 0x{sites[0][0]:06x} {sites[0][1]} {sites[0][2]}")

# Also: any [rcx+...] before rbx is loaded? (raw this derefs)
print("\n=== Raw [rcx+disp] derefs (before rcx is clobbered) ===")
for i,ins in enumerate(ins_list[:40]):
    if "rcx" in ins.op_str and "[" in ins.op_str and "rsp" not in ins.op_str:
        print(f"  0x{ins.address-IMAGE_BASE:06x}  {ins.mnemonic} {ins.op_str}")

# === B. Resolve 0xD50740's table @rdi. rdi = lea rdi,[rip+0x1fe55f8]. ===
# insn at VA 0x180d50751, 7 bytes, ends at 0x180d50758. target VA = end+disp.
rdi_va = 0x180d50758 + 0x1fe55f8
rdi_rva = rdi_va - IMAGE_BASE
print(f"\n=== 0xD50740 table @ rdi: VA 0x{rdi_va:x}  RVA 0x{rdi_rva:x} ===")
# read ~16 qwords from there
print("First 16 qwords (dereferenced as [rdi+idx*8]):")
for k in range(16):
    q=struct.unpack_from("<Q", read(rdi_rva+k*8, 8))[0]
    s=""
    if q>IMAGE_BASE:
        s=read_cstr(q-IMAGE_BASE,48)
    print(f"  [{k}] = 0x{q:x}  {s!r}")

# === C. Where are the phase-state strings relative to that table? ===
for nm in (b"PROCESSING_LISTENING\x00",b"LEARNING_EQ_AND_CLASSIFYING_GENRE\x00",
           b"PROCESSING_SETTING_SIGNAL_CHAIN\x00"):
    # search whole file
    i=data_all.find(nm)
    if i>=0:
        # rva
        for s in pe.sections:
            if s.PointerToRawData<=i<s.PointerToRawData+s.SizeOfRawData:
                rva=s.VirtualAddress+(i-s.PointerToRawData)
                print(f"  {nm[:30]!r}: file off 0x{i:x}  RVA 0x{rva:x}  VA 0x{rva+IMAGE_BASE:x}")
                break

print("\n=== Sanity: does 0xD58A20 ever CALL the poller/applier directly or via 0xE9FC30 chain? ===")
# 0xD58A20 -> 0xE9FC30 (2x). Does 0xE9FC30 reach poller/applier?
seen=set(); frontier={0xE9FC30}; found_poller=found_applier=False
# build callees_by_func quickly for the relevant region only
callees={}
text=next(s for s in pe.sections if s.Name.rstrip(b"\x00")==b".text")
code=data_all[text.PointerToRawData:text.PointerToRawData+text.SizeOfRawData]
t0=text.VirtualAddress; begins_set=set(begins)
i=0
while i<len(code)-5:
    if code[i] in (0xE8,0xE9):
        rel=struct.unpack_from("<i",code,i+1)[0]
        tgt=(t0+i)+5+rel
        if tgt in begins_set:
            sf=function_of(t0+i)
            if sf: callees.setdefault(sf[0],set()).add(tgt)
    i+= 1
depth=0
while frontier and depth<8:
    nxt=set()
    for f in frontier:
        for c in callees.get(f,()):
            if c==0xEAD3E0: found_poller=True
            if c==0xEAD930: found_applier=True
            if c not in seen:
                seen.add(c); nxt.add(c)
    frontier=nxt; depth+=1
    if found_poller and found_applier: break
print(f"  0xE9FC30 transitive reach (<=8 deep): POLLER reached={found_poller}  APPLIER reached={found_applier}")
print(f"  (candidate claims a 6-stage chain 0xD58A20->0xE9FC30->...->POLLER+APPLIER)")
print("\nDONE")
