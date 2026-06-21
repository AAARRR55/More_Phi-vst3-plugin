#!/usr/bin/env py
# UNIQUE probe: correlate the 4 recon agents, disassemble the two competing
# trigger candidates (0xD58A20 vs 0x166CA90) prologues + first ~0x80 bytes,
# and resolve the thunk at 0xD572F0 to confirm vtable-dispatch shape.
# OFFLINE file-read only; no process attach, no execution.
# (methodology sec 5.2 compliant)
import struct
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

DLL = r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll"
IMAGE_BASE = 0x180000000

pe = pefile.PE(DLL)

# Build RUNTIME_FUNCTION table for accurate function bounds
pdata_funcs = set()
runtime = []
if hasattr(pe, "DIRECTORY_ENTRY_EXCEPTION"):
    for e in pe.DIRECTORY_ENTRY_EXCEPTION:
        b = e.struct.BeginAddress
        ed = e.struct.EndAddress
        runtime.append((b, ed))
        pdata_funcs.add(b)

text = None
for s in pe.sections:
    if b".text" in s.Name:
        text = s
        break
text_va = text.VirtualAddress
text_data = text.get_data()
text_size = len(text_data)

def read_bytes(rva, n):
    off = rva - text_va
    if off < 0 or off + n > text_size:
        return b""
    return text_data[off:off + n]

def func_end(rva):
    for b, ed in runtime:
        if b == rva:
            return ed
    return None

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

def disasm(rva, length=0x90, label=""):
    data = read_bytes(rva, length)
    print(f"\n=== {label} @ RVA 0x{rva:x} (first {length:#x} bytes) ===")
    for ins in md.disasm(data, IMAGE_BASE + rva):
        print(f"  0x{ins.address - IMAGE_BASE:06x}  {ins.mnemonic:7s} {ins.op_str}")
        if ins.mnemonic in ("ret", "jmp") and "rip" not in ins.op_str:
            # don't bail too early on tail jmp into helpers; keep going a bit
            pass

# 1. The two competing trigger candidates
disasm(0xD58A20, 0x90, "TRIGGER-CANDIDATE-A (MA-string body, via thunk 0xD572F0)")
disasm(0x166CA90, 0x90, "TRIGGER-CANDIDATE-B (orchestrator caller of POLLER+APPLIER)")

# 2. The thunks that reach each
disasm(0xD572F0, 0x40, "THUNK above A (single caller of 0xD58A20)")
disasm(0xD4B830, 0x70, "THUNK above B (caller of 0x166CA90)")
disasm(0x163DFA0, 0x60, "TWIN THUNK above B (2nd caller of 0x166CA90)")

# 3. Quick state-machine stepper check: does 0xD58A20 write phase immediates
#    and call 0xD50740 (the state stepper named by agent C)?
print("\n=== Scan 0xD58A20..0xD59D10 for: calls to 0xD50740, mov dword [rbp+N],imm ===")
end_a = func_end(0xD58A20) or 0xD59D10
data = read_bytes(0xD58A20, end_a - 0xD58A20)
calls_d50740 = 0
imm_writes = []
for ins in md.disasm(data, IMAGE_BASE + 0xD58A20):
    if ins.mnemonic == "call" and "0xd50740" in ins.op_str.lower():
        calls_d50740 += 1
        print(f"  call 0xD50740 @ 0x{ins.address - IMAGE_BASE:06x}")
    if ins.mnemonic == "mov" and "dword ptr" in ins.op_str and "rbp" in ins.op_str:
        # look for mov [rbp+X], IMM
        parts = ins.op_str.split(",")
        if len(parts) == 2:
            src = parts[1].strip()
            try:
                v = int(src, 0)
                if 0 <= v <= 0x40:
                    imm_writes.append((ins.address - IMAGE_BASE, ins.op_str, v))
            except ValueError:
                pass
print(f"  total calls to 0xD50740 (state stepper): {calls_d50740}")
print(f"  total small immediate writes to [rbp+N]: {len(imm_writes)}")
for a, s, v in imm_writes[:20]:
    print(f"    0x{a:06x}: {s}  (val={v})")

# 4. Same scan for 0x166CA90 to contrast (should NOT write phase sequence)
print("\n=== Scan 0x166CA90..0x166CD47 for calls to 0xD50740 + imm writes ===")
end_b = func_end(0x166CA90) or 0x166CD47
data = read_bytes(0x166CA90, end_b - 0x166CA90)
calls_b = 0
imm_b = []
for ins in md.disasm(data, IMAGE_BASE + 0x166CA90):
    if ins.mnemonic == "call" and "0xd50740" in ins.op_str.lower():
        calls_b += 1
    if ins.mnemonic == "mov" and "dword ptr" in ins.op_str and "rbp" in ins.op_str:
        parts = ins.op_str.split(",")
        if len(parts) == 2:
            try:
                v = int(parts[1].strip(), 0)
                if 0 <= v <= 0x40:
                    imm_b.append((ins.address - IMAGE_BASE, ins.op_str, v))
            except ValueError:
                pass
print(f"  total calls to 0xD50740: {calls_b}")
print(f"  total small immediate writes to [rbp+N]: {len(imm_b)}")
for a, s, v in imm_b[:20]:
    print(f"    0x{a:06x}: {s}  (val={v})")

# 5. RIP-relative string refs in 0xD58A20 (verify "Master Assistant" / "Maximizer" etc.)
print("\n=== RIP-relative refs from 0xD58A20 (LEA/MOV r, [rip+X]) ===")
data = read_bytes(0xD58A20, end_a - 0xD58A20)
def read_cstr(rva, maxlen=64):
    # read from any section
    for s in pe.sections:
        if s.VirtualAddress <= rva < s.VirtualAddress + s.Misc_VirtualSize:
            off = rva - s.VirtualAddress
            d = s.get_data()[off:off+maxlen]
            nul = d.find(b"\x00")
            if nul >= 0:
                d = d[:nul]
            try:
                return d.decode("ascii", "replace")
            except Exception:
                return None
    return None

seen_targets = set()
for ins in md.disasm(data, IMAGE_BASE + 0xD58A20):
    if ins.mnemonic in ("lea", "mov") and "rip" in ins.op_str:
        # resolve target
        try:
            disp_str = ins.op_str.split("[rip")[-1].split("]")[0]
            disp = int(disp_str, 16) if disp_str.strip().startswith(("0x","-0x")) else int(disp_str, 0)
            tgt = ins.address + ins.size + disp - IMAGE_BASE
            if tgt in seen_targets:
                continue
            seen_targets.add(tgt)
            s = read_cstr(tgt)
            if s and 3 <= len(s) <= 60 and all(32 <= ord(c) < 127 for c in s):
                print(f"  0x{ins.address-IMAGE_BASE:06x} -> 0x{tgt:x}  \"{s}\"")
        except Exception:
            pass

print("\nDONE")
