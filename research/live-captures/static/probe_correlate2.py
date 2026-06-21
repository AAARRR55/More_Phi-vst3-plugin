#!/usr/bin/env py
# UNIQUE probe #2: confirm 0xD572F0 is a vtable slot; verify 0xD50740 is the
# state stepper that writes phase indices; check 0xD58A20 string refs by
# scanning .rdata for the actual "Master Assistant" pointer.
# OFFLINE file-read only.
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

DLL = r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll"
IMAGE_BASE = 0x180000000
pe = pefile.PE(DLL)

runtime = []
for e in pe.DIRECTORY_ENTRY_EXCEPTION:
    runtime.append((e.struct.BeginAddress, e.struct.EndAddress))

def func_end(rva):
    for b, ed in runtime:
        if b == rva:
            return ed
    return None

# Section helpers
sections = {}
for s in pe.sections:
    name = s.Name.rstrip(b"\x00").decode("ascii", "replace")
    sections[name] = (s.VirtualAddress, s.get_data(), s.Misc_VirtualSize)

def read_sec(rva, n, sec=".text"):
    if sec not in sections:
        return b""
    va, data, vsz = sections[sec]
    off = rva - va
    if off < 0 or off + n > len(data):
        return b""
    return data[off:off + n]

def read_cstr_any(rva, maxlen=80):
    for name, (va, data, vsz) in sections.items():
        if va <= rva < va + len(data):
            off = rva - va
            d = data[off:off+maxlen]
            nul = d.find(b"\x00")
            if nul >= 0:
                d = d[:nul]
            try:
                t = d.decode("ascii", "replace")
                if all(32 <= ord(c) < 127 or c == "\x00" for c in t):
                    return t
            except Exception:
                return None
    return None

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

# 1. Find "Master Assistant" string VA in .rdata
rdata_va, rdata, _ = sections[".rdata"]
needle = b"Master Assistant\x00"
hits = []
i = 0
while True:
    j = rdata.find(needle, i)
    if j < 0:
        break
    hits.append(rdata_va + j)
    i = j + 1
print(f"'Master Assistant' string occurrences in .rdata: {len(hits)}")
for h in hits[:5]:
    print(f"  VA 0x{h:x} (RVA 0x{h-IMAGE_BASE:x})")

# 2. Find .rdata qword pointers whose value == 0xD572F0+IMAGE_BASE (vtable slot proof)
target_va = 0xD572F0 + IMAGE_BASE
print(f"\nSearching .rdata for qword == 0x{target_va:x} (0xD572F0 as vtable slot):")
import struct
qword_hits = []
for off in range(0, len(rdata) - 8, 8):
    v = struct.unpack_from("<Q", rdata, off)[0]
    if v == target_va:
        qword_hits.append(rdata_va + off)
for q in qword_hits:
    print(f"  .rdata slot @ VA 0x{q:x} (RVA 0x{q-IMAGE_BASE:x})")
    # Show vtable neighborhood: 0x80 before and after
    base_rva = q - IMAGE_BASE
    print(f"  Neighborhood (-0x80..+0x80, qword funcs):")
    for delta in range(-0x80, 0x90, 8):
        slot_va = base_rva + delta
        off2 = slot_va - rdata_va
        if 0 <= off2 < len(rdata) - 8:
            v2 = struct.unpack_from("<Q", rdata, off2)[0]
            rva2 = v2 - IMAGE_BASE if v2 > IMAGE_BASE else 0
            marker = " <-- 0xD572F0" if rva2 == 0xD572F0 else ""
            if 0 < rva2 < 0x3000000:
                # is it a function?
                is_fn = any(b == rva2 for b, _ in runtime[:1000]) or True  # skip expensive check
                print(f"    [{delta:+5d}] -> 0x{rva2:x}{marker}")

# 3. Verify 0xD50740 is the state-machine stepper (writes phase enum)
print("\n=== 0xD50740 (state stepper candidate) first 0x60 bytes ===")
data = read_sec(0xD50740, 0x60)
for ins in md.disasm(data, IMAGE_BASE + 0xD50740):
    print(f"  0x{ins.address-IMAGE_BASE:06x}  {ins.mnemonic:7s} {ins.op_str}")

# 4. What calls 0xD50740 from inside 0xD58A20 region? (Agent C claimed call sites)
print("\n=== Scan 0xD58A20..0xD59D10 for any call to 0xD507xx or 0xD508xx ===")
data = read_sec(0xD58A20, 0xD59D10 - 0xD58A20)
for ins in md.disasm(data, IMAGE_BASE + 0xD58A20):
    if ins.mnemonic == "call":
        tgt = ins.op_str
        if "0xd50740" in tgt.lower() or "0xd508" in tgt.lower() or "0xd509" in tgt.lower() or "0xd50a" in tgt.lower():
            print(f"  0x{ins.address-IMAGE_BASE:06x}  call {tgt}")

# 5. The numbered writes [rbp+0x720] = 6/5/7/8/9 - what immediately follows each write?
#   This shows what state value is being staged.
print("\n=== Context around each [rbp+0x720] write in 0xD58A20 ===")
data = read_sec(0xD58A20, 0xD59D10 - 0xD58A20)
ins_list = list(md.disasm(data, IMAGE_BASE + 0xD58A20))
for idx, ins in enumerate(ins_list):
    if ins.mnemonic == "mov" and "rbp + 0x720" in ins.op_str:
        # print 3 before and 5 after
        start = max(0, idx-2)
        end = min(len(ins_list), idx+6)
        print(f"  --- around 0x{ins.address-IMAGE_BASE:06x} ---")
        for k in range(start, end):
            marker = " *" if k == idx else "  "
            print(f"  {marker}0x{ins_list[k].address-IMAGE_BASE:06x}  {ins_list[k].mnemonic:7s} {ins_list[k].op_str}")

# 6. Also confirm 0xE9FC30 is a callee of 0xD58A20 (the pipeline root link)
print("\n=== Scan 0xD58A20 for call 0xE9FC30 (pipeline root) ===")
for ins in ins_list:
    if ins.mnemonic == "call" and "0xe9fc30" in ins.op_str.lower():
        print(f"  0x{ins.address-IMAGE_BASE:06x}  call {ins.op_str}  (PIPELINE ROOT CALL)")

print("\nDONE")
