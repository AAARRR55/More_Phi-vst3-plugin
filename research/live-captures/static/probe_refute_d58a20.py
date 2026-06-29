#!/usr/bin/env py
# ADVERSARIAL REFUTATION PROBE for trigger candidate 0xD58A20.
# OFFLINE file-read only (methodology sec 5.2). No process attach, no execution.
#
# Goal: try to BREAK the candidate's claims, one by one.
#  (1) Verify the prologue byte-for-byte vs the candidate's claimed signature.
#  (2) Verify the "mov [rbp+0x720],<5..9>; lea rcx,[rbp+0x720]; call 0xD50740"
#      pattern. Does it really happen 5x with sequential phase values?
#  (3) Verify 0xD50740 is a state stepper that indexes the phase-string table.
#      CRUCIAL: the candidate claims 0xD50740 is called ONLY by the trigger.
#      Recon shows the POLLER 0xEAD3E0 and APPLIER 0xEAD930 BOTH call 0xD50740.
#      So 0xD50740 cannot be a trigger-specific "advance state" stepper.
#  (4) Verify the thunk 0xD572F0 shape and that it is the only caller of 0xD58A20.
#  (5) Determine whether 0xD58A20 is reachable from a periodic/tick hub (would
#      make it a dispatcher, not a user-initiated trigger).
#  (6) Verify whether the 20 "Master Assistant" xref funcs actually include 0xD58A20.
import struct
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

DLL = r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll"
IMAGE_BASE = 0x180000000

pe = pefile.PE(DLL)

# RUNTIME_FUNCTION (pdata) -> function bounds
runtime = []
for e in pe.DIRECTORY_ENTRY_EXCEPTION:
    runtime.append((e.struct.BeginAddress, e.struct.EndAddress))
runtime_set = set(b for b, _ in runtime)

def func_end(rva):
    for b, ed in runtime:
        if b == rva:
            return ed
    return None

# Section table
sections = {}
for s in pe.sections:
    name = s.Name.rstrip(b"\x00").decode("ascii", "replace")
    sections[name] = (s.VirtualAddress, s.get_data(), s.Misc_VirtualSize)

def read(rva, n):
    for name, (va, data, vsz) in sections.items():
        if va <= rva < va + len(data):
            off = rva - va
            return data[off:off+n]
    return b""

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True

def func_calls(rva):
    """Return list of direct call targets (RVAs) inside the function body."""
    end = func_end(rva) or (rva + 0x4000)
    data = read(rva, end - rva)
    out = []
    for ins in md.disasm(data, IMAGE_BASE + rva):
        if ins.mnemonic == "call":
            op = ins.op_str.strip()
            if op.startswith("0x"):
                try:
                    out.append((ins.address - IMAGE_BASE, int(op, 16) - IMAGE_BASE))
                except ValueError:
                    pass
    return out

def disasm_lines(rva, n=0x80, label=""):
    print(f"\n--- {label} @ RVA 0x{rva:x} ---")
    data = read(rva, n)
    for ins in md.disasm(data, IMAGE_BASE + rva):
        print(f"  0x{ins.address-IMAGE_BASE:06x}  {ins.mnemonic:7s} {ins.op_str}")

print("=" * 70)
print("CLAIM 1: Prologue of 0xD58A20 (claimed MSVC thiscall, 3-arg)")
print("=" * 70)
disasm_lines(0xD58A20, 0x70, "0xD58A20 prologue")

print("\n" + "=" * 70)
print("CLAIM 2: 'mov dword [rbp+0x720],<5|6|7|8|9>; lea rcx,[rbp+0x720]; call 0xD50740' x5")
print("=" * 70)
end_a = func_end(0xD58A20)
print(f"func bounds: 0x{0xD58A20:x} .. 0x{end_a:x}  (size {end_a-0xD58A20} bytes)")
data = read(0xD58A20, end_a - 0xD58A20)
ins_list = list(md.disasm(data, IMAGE_BASE + 0xD58A20))
# find all writes to [rbp+0x720]
writes720 = [(i, ins) for i, ins in enumerate(ins_list)
             if ins.mnemonic == "mov" and "rbp + 0x720" in ins.op_str]
print(f"writes to [rbp+0x720]: {len(writes720)}")
for i, ins in writes720:
    print(f"  0x{ins.address-IMAGE_BASE:06x}: {ins.mnemonic} {ins.op_str}")
    # show next 4 ins
    for k in range(i+1, min(i+5, len(ins_list))):
        print(f"      +{ins_list[k].address-ins.address:04x}  {ins_list[k].mnemonic:7s} {ins_list[k].op_str}")

# count calls to 0xD50740 from 0xD58A20
calls_50740 = [(ins.address-IMAGE_BASE) for ins in ins_list
               if ins.mnemonic=="call" and "0xd50740" in ins.op_str.lower()]
print(f"\ncalls to 0xD50740 from 0xD58A20: {len(calls_50740)}  at {[hex(x) for x in calls_50740]}")

print("\n" + "=" * 70)
print("CLAIM 3 (REFUTATION): 0xD50740 - is it a trigger-only 'state stepper'?")
print("Recon shows POLLER 0xEAD3E0 + APPLIER 0xEAD930 also call 0xD50740.")
print("=" * 70)
disasm_lines(0xD50740, 0x80, "0xD50740 prologue/body")
end_50740 = func_end(0xD50740)
print(f"0xD50740 bounds: 0x{0xD50740:x} .. 0x{end_50740:x}  (size {end_50740-0xD50740})")

# Does 0xD50740 index a lea rdi,[rip+disp] table as claimed?
data50740 = read(0xD50740, end_50740 - 0xD50740)
lea_rips = []
for ins in md.disasm(data50740, IMAGE_BASE + 0xD50740):
    if ins.mnemonic == "lea" and "rip" in ins.op_str and ("rdi" in ins.op_str or "rax" in ins.op_str or "rcx" in ins.op_str):
        lea_rips.append((ins.address-IMAGE_BASE, ins.op_str))
print(f"lea <reg>,[rip+disp] in 0xD50740: {len(lea_rips)}")
for a, op in lea_rips[:6]:
    print(f"  0x{a:06x}: lea {op}")

print("\n" + "=" * 70)
print("CLAIM 4: thunk 0xD572F0 is the ONLY caller of 0xD58A20; +8 this-adjustor")
print("=" * 70)
disasm_lines(0xD572F0, 0x40, "0xD572F0 thunk")

# Find ALL direct callers of 0xD58A20 across whole .text (cheap scan: look for
# E8 rel32 call instructions whose target == 0xD58A20).
text_va, text_data, _ = sections[".text"]
import re
callers_d58a20 = []
# scan every 0xE8 byte
i = 0
while i < len(text_data) - 5:
    if text_data[i] == 0xE8:
        rel = struct.unpack_from("<i", text_data, i+1)[0]
        call_site_rva = text_va + i
        target = call_site_rva + 5 + rel
        if target == 0xD58A20:
            callers_d58a20.append(call_site_rva)
    i += 1
print(f"\nDIRECT (E8 rel32) callers of 0xD58A20 across .text: {len(callers_d58a20)}")
for c in callers_d58a20:
    print(f"  call site @ 0x{c:x}")
    disasm_lines(c, 0x10, f"caller site")

# Also scan for the thunk being a caller
print("\nDIRECT callers of thunk 0xD572F0 across .text:")
callers_thunk = []
i = 0
while i < len(text_data) - 5:
    if text_data[i] == 0xE8:
        rel = struct.unpack_from("<i", text_data, i+1)[0]
        call_site_rva = text_va + i
        target = call_site_rva + 5 + rel
        if target == 0xD572F0:
            callers_thunk.append(call_site_rva)
    i += 1
print(f"  count: {len(callers_thunk)}  sites: {[hex(c) for c in callers_thunk]}")

print("\n" + "=" * 70)
print("CLAIM 5 (REFUTATION): is 0xD58A20 reachable from a periodic/tick hub?")
print("Check if the per-frame tick 0xEABDB0 (poller caller) reaches 0xD58A20.")
print("=" * 70)
# Simple BFS: does anything calling EABDB0 also reach D58A20 transitively? Too
# expensive globally; instead check direct: does 0xEABDB0 or its direct callees
# call into 0xD58A20 or 0xD572F0 or 0xE9FC30?
for hub in (0xEABDB0, 0xEA4FD0):
    cs = func_calls(hub)
    print(f"0x{hub:x} direct call targets: {[hex(t) for _,t in cs]}")

print("\n" + "=" * 70)
print("CLAIM 6: does 0xD58A20 have a direct RIP xref to 'Master Assistant'?")
print("=" * 70)
rdata_va, rdata, _ = sections[".rdata"]
needle = b"Master Assistant\x00"
ma_hits = []
i = 0
while True:
    j = rdata.find(needle, i)
    if j < 0: break
    ma_hits.append(rdata_va + j)
    i = j + 1
print(f"'Master Assistant' string occurrences in .rdata: {len(ma_hits)}  VAs: {[hex(h) for h in ma_hits]}")
# scan .text for RIP-relative lea/mov that resolve to one of these string VAs
# Approximation: scan for 8B/48 8D patterns. Use capstone on whole .text is too slow.
# Instead: just scan a 0x4000 window around 0xD58A20.
def has_ma_xref(func_rva):
    end = func_end(func_rva) or (func_rva + 0x4000)
    data = read(func_rva, end - func_rva)
    for ins in md.disasm(data, IMAGE_BASE + func_rva):
        if "rip" in ins.op_str:
            for h in ma_hits:
                # rip-rel target
                # crude: check if hex of (h - IMAGE_BASE) appears in op_str
                tstr = hex(h)
                if tstr in ins.op_str or hex(h - IMAGE_BASE) in ins.op_str:
                    return (ins.address-IMAGE_BASE, ins.mnemonic + " " + ins.op_str, h)
    return None
xref = has_ma_xref(0xD58A20)
print(f"0xD58A20 -> 'Master Assistant' xref: {xref}")

print("\n" + "=" * 70)
print("CLAIM 7: how many of the 20 'Master Assistant' xref funcs are there really?")
print("=" * 70)
# Search whole .text for E8/LEA referencing the MA string VA via RIP. We'll scan
# the .text in chunks using capstone - too slow for 60MB. Use a targeted byte scan:
# RIP-rel LEA is 48 8D xx (disp32) ; MOV is 48 8B xx. Look for the disp32 that
# resolves each MA hit.
def scan_rip_refs(target_va):
    """Return list of (callsite_rva, instr_bytes) where a RIP-rel operand
    targets target_va. Scans .text byte-by-byte for 4-byte disp pattern."""
    hits = []
    # for each possible instruction address, check if next 4 bytes form a
    # rip-relative disp pointing at target_va. Approximate: find every 4-byte
    # occurrence whose value makes sense.
    # Better: scan for the specific disp value at every offset.
    for disp_off in range(0, len(text_data) - 4):
        disp = struct.unpack_from("<i", text_data, disp_off)[0]
        # RIP-rel: instruction end = text_va + disp_off + 4; target = end + disp
        end = text_va + disp_off + 4
        if end + disp == target_va:
            # check byte before disp looks like a ModRM with RIP-rel (05/0D/15...)
            if disp_off >= 3:
                modrm = text_data[disp_off-1]
                if (modrm & 0xC7) == 0x05:  # [rip+disp32]
                    callsite = text_va + disp_off - 1
                    # back up to find opcode/rex
                    hits.append((callsite, modrm))
    return hits

ma_xrefs_total = 0
for h in ma_hits:
    r = scan_rip_refs(h)
    ma_xrefs_total += len(r)
print(f"Total RIP-rel refs to any 'Master Assistant' string VA: {ma_xrefs_total}")
# count distinct containing functions
func_starts = sorted(runtime_set)
import bisect
containing = set()
for h in ma_hits:
    for callsite, _ in scan_rip_refs(h):
        idx = bisect.bisect_right(func_starts, callsite) - 1
        if idx >= 0:
            containing.add(func_starts[idx])
print(f"Distinct functions containing a 'Master Assistant' RIP xref: {len(containing)}")
print(f"Is 0xD58A20 among them? {0xD58A20 in containing}")
print(f"First 25: {[hex(f) for f in sorted(containing)[:25]]}")

print("\nDONE")
