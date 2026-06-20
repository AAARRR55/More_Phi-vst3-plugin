#!/usr/bin/env py
# REFUTATION PROBE #3 — attack the "trigger not dispatcher/poller" claim.
# OFFLINE file-read only.
import struct, bisect
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

DLL = r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll"
IMAGE_BASE = 0x180000000
data_all = open(DLL,"rb").read()
pe = pefile.PE(DLL, fast_load=True); pe.parse_data_directories()
pd = next(s for s in pe.sections if s.Name.rstrip(b"\x00")==b".pdata")
funcs=[]; base=pd.PointerToRawData
for off in range(base, base+pd.SizeOfRawData-11, 12):
    b=int.from_bytes(data_all[off:off+4],"little"); e=int.from_bytes(data_all[off+4:off+8],"little")
    if b: funcs.append((b,e))
funcs.sort(); begins=sorted(b for b,_ in funcs); begins_set=set(begins)
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
    o=rva_to_off(rva)
    return data_all[o:o+n] if o is not None else b""
def read_cstr(rva,n=80):
    d=read(rva,n); i=d.find(b"\x00")
    return d[:i].decode("ascii","replace") if i>=0 else None
md=Cs(CS_ARCH_X86,CS_MODE_64); md.detail=True

CAND=0xD58A20; STEPPER=0xD50740; PIPELINE=0xE9FC30
POLLER=0xEAD3E0; APPLIER=0xEAD930; ORCH=0x166CA90; THUNK=0xD572F0

# === A. What are the 5 strings loaded right after each phase write? ===
# Pattern at each site: mov [rbp+0x720],N; lea rcx,[rbp+0x720]; call 0xD50740; lea rcx,[rip+disp]
# Resolve the lea rcx,[rip+disp] target -> read string.
print("=== Strings staged after each phase-enum write (the per-stage label) ===")
fb,fe=function_of(CAND); body=read(CAND,fe-CAND)
ins_list=list(md.disasm(body,IMAGE_BASE+CAND))
phase_strings={}
for i,ins in enumerate(ins_list):
    if ins.mnemonic=="mov" and "rbp + 0x720" in ins.op_str and "," in ins.op_str:
        try:
            v=int(ins.op_str.split(",")[1].strip(),0)
        except: continue
        if v not in (5,6,7,8,9): continue
        # find next 'lea rcx,[rip+...]' within 4 ins
        for k in range(i+1,min(i+6,len(ins_list))):
            x=ins_list[k]
            if x.mnemonic=="lea" and "rcx" in x.op_str and "rip" in x.op_str:
                disp_str=x.op_str.split("[rip")[1].split("]")[0].replace(" ","")
                disp=int(disp_str,16)
                tgt=(x.address+x.size+disp)-IMAGE_BASE
                s=read_cstr(tgt)
                phase_strings[v]=(tgt,s)
                break
for v in sorted(phase_strings):
    tgt,s=phase_strings[v]
    print(f"  phase {v}: -> 0x{tgt:x}  {s!r}")

# === B. The candidate claims 0xD50740 indexes a phase-string TABLE. Disprove/confirm. ===
# Disasm more of 0xD50740
print("\n=== 0xD50740 fuller body (0xc0 bytes) ===")
for ins in md.disasm(read(STEPPER,0xc0),IMAGE_BASE+STEPPER):
    print(f"  0x{ins.address-IMAGE_BASE:06x}  {ins.mnemonic:7s} {ins.op_str}")

# === C. CRITICAL: is 0xD58A20 reachable from a periodic/tick hub? ===
# Check if 0xD58A20 or the thunk 0xD572F0 or vtable slot 0x28a9458 appear inside
# any function that's called by the per-frame tick 0xEABDB0 or idle hub 0xEA4FD0.
# Strategy: do reverse-BFS up to 3 levels from {0xD58A20, 0xD572F0} over the
# E8/E9 graph, collecting the set of ancestor functions; check if any tick hub
# is in that ancestor set.
print("\n=== Reverse reachability: who reaches 0xD58A20? ===")
text=next(s for s in pe.sections if s.Name.rstrip(b"\x00")==b".text")
code=data_all[text.PointerToRawData:text.PointerToRawData+text.SizeOfRawData]
t0=text.VirtualAddress
# build edge list caller_func -> set(callee targets) ONLY for targets that are
# .pdata function begins. (E8/E9)
callees_by_func={}
callers_by_func={}
i=0
while i<len(code)-5:
    if code[i] in (0xE8,0xE9):
        rel=struct.unpack_from("<i",code,i+1)[0]
        tgt=(t0+i)+5+rel
        if tgt in begins_set:
            sf=function_of(t0+i)
            if sf:
                callees_by_func.setdefault(sf[0],set()).add(tgt)
                callers_by_func.setdefault(tgt,set()).add(sf[0])
    i+=1
# also: the vtable slot 0x28a9458 holds 0xD572F0; the slot is consumed by
# INDIRECT call [reg+N]. We can't resolve those statically. But any function
# that calls 0xD572F0 directly (E8) is in callers_by_func[THUNK].
print(f"callers_by_func[0xD58A20] (E8/E9): {callers_by_func.get(CAND,set())}")
print(f"callers_by_func[THUNK 0xD572F0] (E8/E9): {callers_by_func.get(THUNK,set())}")
print(f"  (vtable slot 0x28a9458 is the indirect-dispatch path; E8 count is 0 expected)")

# Reverse BFS from {CAND, THUNK} up to depth 4 over E8/E9 edges
seed={CAND,THUNK}; seen=set(seed); frontier=seed
for depth in range(1,5):
    nxt=set()
    for f in frontier:
        for c in callers_by_func.get(f,set()):
            if c not in seen:
                seen.add(c); nxt.add(c)
    print(f"  depth {depth}: +{len(nxt)} ancestors; sample: {[hex(x) for x in list(nxt)[:12]]}")
    frontier=nxt
    if not frontier: break

# Are the tick hubs in the ancestor set?
TICKS={0xEABDB0,0xEA4FD0,0xEAD3E0,0x166CA90}
print(f"\nTick/poller hubs in E8/E9-ancestor set of {{0xD58A20,0xD572F0}}?: "
      f"{ {hex(t): (t in seen) for t in TICKS} }")
print("(If False everywhere: 0xD58A20 is NOT reachable via direct calls from periodic hubs —")
print(" it is only reached via the vtable slot = UI/dispatcher-initiated shape.)")

# === D. Does 0xD58A20's body contain a tight loop or repeated re-entry pattern? ===
# A trigger should run once-through; a poller/dispatcher has loops. Check for
# backward jumps / loop structure inside 0xD58A20.
print("\n=== Backward jumps inside 0xD58A20 (loop structure)? ===")
back=0
for ins in ins_list:
    if ins.mnemonic.startswith("j") and "0x" in ins.op_str:
        try:
            tgt=int(ins.op_str.split("0x")[-1],16)
            if tgt < ins.address: back+=1
        except: pass
print(f"  backward conditional/uncond jumps: {back}")
# rets
rets=sum(1 for ins in ins_list if ins.mnemonic=="ret")
print(f"  ret instructions in body: {rets}  (a single-entry/single-exit trigger usually has 1 main ret)")

# === E. Does 0xD58A20 allocate heap / call new (would indicate it builds work items)? ===
# 0x1A3FB3C / 0x1A3FB34 were the top callees — likely operator new / alloc.
print("\n=== Top callees: identify allocator-like 0x1A3FB3x ===")
for tgt in (0x1a3fb3c,0x1a3fb34,0x1a6a6cc):
    print(f"  0x{tgt:x} first bytes:")
    for ins in md.disasm(read(tgt,0x20),IMAGE_BASE+tgt):
        print(f"    {ins.mnemonic:7s} {ins.op_str}")

print("\nDONE")
