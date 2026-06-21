#!/usr/bin/env py
# ADVERSARIAL REFUTATION PROBE #2 — uses RAW .pdata parsing (like ozone_static_recon.py)
# so we get the correct 91,822 function bounds. OFFLINE file-read only.
import struct, bisect
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

DLL = r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll"
IMAGE_BASE = 0x180000000
data_all = open(DLL, "rb").read()
pe = pefile.PE(DLL, fast_load=True); pe.parse_data_directories()

# Raw .pdata parse (matches recon tool)
pd = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".pdata")
funcs = []
base = pd.PointerToRawData
for off in range(base, base + pd.SizeOfRawData - 11, 12):
    b = int.from_bytes(data_all[off:off+4], "little")
    e = int.from_bytes(data_all[off+4:off+8], "little")
    if b == 0: continue
    funcs.append((b, e))
funcs.sort()
begins = sorted(b for b, _ in funcs)
begins_set = set(begins)
print(f"pdata functions parsed: {len(funcs)}  (recon claims 91822)")

def function_of(rva):
    i = bisect.bisect_right(begins, rva) - 1
    if i < 0: return None
    b, e = funcs[i]
    return (b, e) if b <= rva < e else None

def rva_to_off(rva):
    for s in pe.sections:
        span = max(s.Misc_VirtualSize, s.SizeOfRawData)
        if s.VirtualAddress <= rva < s.VirtualAddress + span:
            return s.PointerToRawData + (rva - s.VirtualAddress)
    return None

def read(rva, n):
    o = rva_to_off(rva)
    if o is None: return b""
    return data_all[o:o+n]

md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True

CAND = 0xD58A20
THUNK = 0xD572F0
STEPPER = 0xD50740
POLLER = 0xEAD3E0
APPLIER = 0xEAD930
ORCH = 0x166CA90
PIPELINE = 0xE9FC30

for name, rva in [("CAND 0xD58A20",CAND),("THUNK 0xD572F0",THUNK),
                  ("STEPPER 0xD50740",STEPPER),("POLLER 0xEAD3E0",POLLER),
                  ("APPLIER 0xEAD930",APPLIER),("ORCH 0x166CA90",ORCH),
                  ("PIPELINE 0xE9FC30",PIPELINE)]:
    f = function_of(rva)
    print(f"{name}: func bounds = {('#%x..#%x'%(f[0],f[1])) if f else 'NOT IN PDATA'}  "
          f"(is_begin={f[0]==rva if f else '-'})")

print("\n" + "="*70)
print("CLAIM 1 re-verify: prologue of 0xD58A20")
print("="*70)
for ins in md.disasm(read(CAND, 0x60), IMAGE_BASE + CAND):
    print(f"  0x{ins.address-IMAGE_BASE:06x}  {ins.mnemonic:7s} {ins.op_str}")

print("\n" + "="*70)
print("CLAIM 2: phase-immediate writes + stepper calls in 0xD58A20")
print("="*70)
fb, fe = function_of(CAND)
print(f"body 0x{fb:x}..0x{fe:x}  size={fe-fb}")
body = read(CAND, fe - CAND)
ins_list = list(md.disasm(body, IMAGE_BASE + CAND))

# all writes to [rbp+0x720]
w720 = [(i,ins) for i,ins in enumerate(ins_list)
        if ins.mnemonic=="mov" and "rbp + 0x720" in ins.op_str]
print(f"writes to [rbp+0x720]: {len(w720)}")
for i, ins in w720:
    print(f"  0x{ins.address-IMAGE_BASE:06x}: {ins.op_str}")
    for k in range(i+1, min(i+4, len(ins_list))):
        print(f"      {ins_list[k].mnemonic:7s} {ins_list[k].op_str}")

# all writes to ANY [rbp+disp] with small immediate, and any nearby call
print("\nALL small-immediate writes [rbp+N] in 0xD58A20 (N in 0x600..0x740):")
import re
seq = []
for i, ins in enumerate(ins_list):
    if ins.mnemonic == "mov":
        m = re.search(r"\[rbp \+ (0x[0-9a-f]+)\]", ins.op_str)
        if m:
            disp = int(m.group(1), 16)
            parts = ins.op_str.split(",")
            if len(parts)==2:
                try:
                    v = int(parts[1].strip(), 0)
                    if 0 <= v <= 0x40 and 0x600 <= disp <= 0x740:
                        seq.append((ins.address-IMAGE_BASE, disp, v, i))
                except ValueError: pass
print(f"  count={len(seq)}")
for a,d,v,i in seq[:30]:
    print(f"  0x{a:06x}: mov [rbp+{d:#x}],{v}")

# all calls in 0xD58A20 and their targets
calls = [(ins.address-IMAGE_BASE, ins.op_str) for ins in ins_list if ins.mnemonic=="call"]
print(f"\nALL call targets from 0xD58A20: {len(calls)}")
from collections import Counter
tgt_cnt = Counter()
for a, op in calls:
    if op.startswith("0x"):
        try:
            t = int(op,16) - IMAGE_BASE
            tgt_cnt[t]+=1
        except: pass
for t,c in sorted(tgt_cnt.items(), key=lambda x:-x[1])[:25]:
    print(f"  0x{t:x}: {c}x")

print(f"\ncalls to STEPPER 0xD50740: {tgt_cnt.get(STEPPER,0)}")
print(f"calls to PIPELINE 0xE9FC30: {tgt_cnt.get(PIPELINE,0)}")
print(f"calls to POLLER 0xEAD3E0: {tgt_cnt.get(POLLER,0)}")
print(f"calls to APPLIER 0xEAD930: {tgt_cnt.get(APPLIER,0)}")

print("\n" + "="*70)
print("CLAIM 3 (REFUTE): is 0xD50740 a TRIGGER-ONLY stepper?")
print("="*70)
print(f"POLLER 0xEAD3E0 is a caller of STEPPER 0xD50740? -> checking recon anchors...")
# disassemble POLLER, find if it calls STEPPER
pbody = read(POLLER, function_of(POLLER)[1]-POLLER)
for ins in md.disasm(pbody, IMAGE_BASE+POLLER):
    if ins.mnemonic=="call" and "0xd50740" in ins.op_str.lower():
        print(f"  POLLER 0xEAD3E0 @ 0x{ins.address-IMAGE_BASE:06x} CALLS STEPPER 0xD50740")
        break
else:
    print("  POLLER does NOT directly call 0xD50740 (check indirect/listing)")
# APPLIER
abody = read(APPLIER, function_of(APPLIER)[1]-APPLIER)
for ins in md.disasm(abody, IMAGE_BASE+APPLIER):
    if ins.mnemonic=="call" and "0xd50740" in ins.op_str.lower():
        print(f"  APPLIER 0xEAD930 @ 0x{ins.address-IMAGE_BASE:06x} CALLS STEPPER 0xD50740")
        break
else:
    print("  APPLIER does NOT directly call 0xD50740")

# Disassemble 0xD50740 to see what it actually is
print("\n0xD50740 first 0x60 bytes:")
for ins in md.disasm(read(STEPPER, 0x60), IMAGE_BASE+STEPPER):
    print(f"  0x{ins.address-IMAGE_BASE:06x}  {ins.mnemonic:7s} {ins.op_str}")

print("\n" + "="*70)
print("CLAIM 4: thunk 0xD572F0 — only caller of 0xD58A20?")
print("="*70)
for ins in md.disasm(read(THUNK, 0x40), IMAGE_BASE+THUNK):
    print(f"  0x{ins.address-IMAGE_BASE:06x}  {ins.mnemonic:7s} {ins.op_str}")

# Direct E8 callers of 0xD58A20 across whole .text
text = next(s for s in pe.sections if s.Name.rstrip(b"\x00")==b".text")
code = data_all[text.PointerToRawData:text.PointerToRawData+text.SizeOfRawData]
t0 = text.VirtualAddress
callers_cand = []
i = 0
while i < len(code)-5:
    if code[i]==0xE8:
        rel = struct.unpack_from("<i", code, i+1)[0]
        if (t0+i)+5+rel == CAND:
            callers_cand.append(t0+i)
    i += 1
print(f"\nDIRECT E8 callers of 0xD58A20: {len(callers_cand)} -> {[hex(c) for c in callers_cand]}")
# thunk callers
callers_thunk=[]
i=0
while i < len(code)-5:
    if code[i]==0xE8:
        rel=struct.unpack_from("<i",code,i+1)[0]
        if (t0+i)+5+rel == THUNK:
            callers_thunk.append(t0+i)
    i+=1
print(f"DIRECT E8 callers of THUNK 0xD572F0: {len(callers_thunk)} -> {[hex(c) for c in callers_thunk[:10]]}")
# E9 jmp-tail callers too (thunks often tail-jumped)
jmpers_thunk=[]
i=0
while i < len(code)-5:
    if code[i]==0xE9:
        rel=struct.unpack_from("<i",code,i+1)[0]
        if (t0+i)+5+rel == THUNK:
            jmpers_thunk.append(t0+i)
    i+=1
print(f"DIRECT E9 jmp to THUNK 0xD572F0: {len(jmpers_thunk)}")

# .rdata vtable slot check: does any qword in .rdata == THUNK+IMAGE_BASE?
rdata = next(s for s in pe.sections if s.Name.rstrip(b"\x00")==b".rdata")
rdata_va = rdata.VirtualAddress
rdata_bytes = data_all[rdata.PointerToRawData:rdata.PointerToRawData+rdata.SizeOfRawData]
target_va = THUNK + IMAGE_BASE
slots=[]
for off in range(0, len(rdata_bytes)-8, 8):
    if struct.unpack_from("<Q", rdata_bytes, off)[0]==target_va:
        slots.append(rdata_va+off)
print(f"\n.rdata qword slots == 0xD572F0 (vtable entries): {len(slots)} -> {[hex(s) for s in slots]}")

print("\n" + "="*70)
print("CLAIM 6 (REFUTE): does 0xD58A20 really xref 'Master Assistant' string?")
print("="*70)
needle = b"Master Assistant\x00"
ma_hits=[]
i=0
while True:
    j = rdata_bytes.find(needle, i)
    if j<0: break
    ma_hits.append(rdata_va+j); i=j+1
print(f"'Master Assistant' occurrences in .rdata: {len(ma_hits)}")
# scan whole .text for RIP-rel refs to any MA hit (like recon tool)
# REX.W + lea/mov + rip-modrm (7-byte)
rip_op = __import__("re").compile(rb"[\x48\x4c][\x8d\x8b\x89\x3b\x39\x85][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]")
ma_user_funcs=set()
ma_target_set=set(ma_hits)
for m in rip_op.finditer(code):
    i=m.start()
    if i+7>len(code): continue
    disp = struct.unpack_from("<i", code, i+3)[0]
    tgt = (t0+i)+7+disp
    if tgt in ma_target_set:
        f = function_of(t0+i)
        if f: ma_user_funcs.add(f[0])
print(f"funcs with RIP xref to 'Master Assistant': {len(ma_user_funcs)}")
print(f"is 0xD58A20 (func begin) among them? {CAND in ma_user_funcs}")
# What's the containing function of 0xD58A20?
fc = function_of(CAND)
print(f"containing func of 0xD58A20 = 0x{fc[0]:x}..0x{fc[1]:x}; is THIS in MA set? {fc[0] in ma_user_funcs}")
print("First 25 MA-xref funcs:", [hex(f) for f in sorted(ma_user_funcs)[:25]])

print("\nDONE")
