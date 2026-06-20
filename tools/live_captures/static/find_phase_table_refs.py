#!/usr/bin/env python3
"""Fresh static lead: find code that RIP-references the phase-state STRING TABLE.

The phase strings (PROCESSING_LISTENING etc.) are never RIP-referenced directly
(ref_funcs=0). But they sit in a 19-entry pointer TABLE in .rdata (~0x25c5af0).
The state-machine driver must index/LEA into that table. Finding those code
sites is the real lead on the GUI Play -> state-transition path.

Read-only, no process attach, no DAW risk.
"""
import sys, json
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent))
import ozone_static_recon as recon
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP
import pefile, bisect

dll = r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll"
data = Path(dll).read_bytes()
pe = pefile.PE(dll, fast_load=True); pe.parse_data_directories()
ib = pe.OPTIONAL_HEADER.ImageBase
funcs, begins = recon.parse_functions(pe, data)

# Phase string RVAs (from recon) — find the tightest cluster = the table base.
phase_strs = {
    'PROCESSING_LISTENING':              0x25c5b40,
    'LEARNING_EQ_AND_CLASSIFYING_GENRE': 0x25c5b10,
    'PROCESSING_SETTING_SIGNAL_CHAIN':   0x25c5af0,
}
# The table spans at least 0x25c5af0..0x25c5b40 = 80 bytes (19 entries * ~4-8 bytes).
TABLE_LO = 0x25c5af0 - 0x40
TABLE_HI = 0x25c5b40 + 0x40
print(f"searching for RIP-relative refs to table region 0x{TABLE_LO:x}..0x{TABLE_HI:x}")

# Also the OTHER table sites (0x25ccc88, 0x25d8e10 etc.) — gather all string RVAs.
all_phase_rvas = [0x25c5b40,0x25ccc88,0x25d8e10,0x2678318,0x267b430,
                  0x25c5b10,0x25ccca0,0x25d8e28,
                  0x25c5af0,0x25cccd0,0x25d8eb8]
search_set = set(all_phase_rvas)
# widen: include any addr in the two extra table clusters
for base in (0x25ccca0, 0x25d8e10):
    for off in range(-0x40, 0x80, 8):
        search_set.add(base+off)

def func_of(rva):
    i = bisect.bisect_right(begins, rva)-1
    if i<0: return None
    b,e = funcs[i]
    return (b,e) if b<=rva<e else None

text = next(s for s in pe.sections if s.Name.rstrip(b"\x00")==b".text")
code = data[text.PointerToRawData:text.PointerToRawData+text.SizeOfRawData]
text_va = ib + text.VirtualAddress
md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail=True

ref_sites = []  # (insn_addr, func_begin, target_rva)
n=len(code); pos=0; win=1<<20
while pos<n:
    chunk=code[pos:min(pos+win,n)]; prog=False; last=pos
    for insn in md.disasm(chunk, text_va+pos):
        last=(insn.address-text_va)+insn.size; prog=True
        for op in insn.operands:
            if op.type==X86_OP_MEM and op.mem.base==X86_REG_RIP:
                tgt = insn.address+insn.size+op.mem.disp - ib
                # match if target lands in the table region OR hits a known phase RVA
                if TABLE_LO <= tgt <= TABLE_HI or tgt in search_set:
                    f=func_of(insn.address)
                    ref_sites.append((insn.address-ib, f[0] if f else None, tgt, insn.mnemonic+' '+insn.op_str))
    pos = last if prog else pos+1

print(f"\nRIP-relative refs to phase string/table region: {len(ref_sites)}")
# Group by containing function.
from collections import Counter
byfunc = Counter(s[1] for s in ref_sites if s[1])
print(f"distinct referring functions: {len(byfunc)}")
print("\ntop referring functions:")
for fn,c in byfunc.most_common(20):
    print(f"  func 0x{fn:x}: {c} ref sites")
print("\nsample sites (first 20):")
for addr,fn,tgt,inst in ref_sites[:20]:
    print(f"  0x{addr:x} (in 0x{fn:x if fn else 0:x}) -> table+0x{tgt-0x25c5af0:x} : {inst}")

# Save
Path("tools/live_captures/static").mkdir(parents=True, exist_ok=True)
out = {"table_region":[hex(TABLE_LO),hex(TABLE_HI)],
       "ref_count":len(ref_sites),
       "by_function":[(hex(f),c) for f,c in byfunc.most_common(50)],
       "sites":[{"addr":hex(a),"func":hex(fn) if fn else None,"target":hex(t),"insn":i} for a,fn,t,i in ref_sites[:100]]}
Path("tools/live_captures/static/phase_table_refs.json").write_text(json.dumps(out,indent=1))
print("\nsaved -> tools/live_captures/static/phase_table_refs.json")
