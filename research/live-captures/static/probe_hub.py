#!/usr/bin/env python3
"""
Hub analysis: the two functions that call BOTH the state poller (+0xEAD3E0)
AND the apply path (+0xEAD930) are +0xEABDB0 and +0x166CA90. The live findings
named +0x166CA90 the "caller" and explicitly flagged it NOT-the-trigger.
+0xEABDB0 is the UNNAMED hub. This probe:

  - disassembles +0xEABDB0 fully (capstone) and lists every direct call it
    makes, flagging anchor / cluster hits;
  - maps ITS callers (who invokes the hub) -> the trigger's callers;
  - checks whether it RIP-references the phase-state strings (table base) or
    the "Master Assistant" string;
  - compares its shape to a "start analysis" entry (single context-struct
    thiscall arg, fans out to setup + the poller/secondary).

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
STATE_HELPER_A = 0x0EAD2E0
STATE_HELPER_B = 0x0EAD360
SECONDARY      = 0x0EAD930
CALLER         = 0x166CA90
DATA_STREAM    = 0x0FD7F30
HUB            = 0x0EABDB0   # unnamed common caller of poller + secondary

PHASE_TABLE_BASE_RVAS = []  # filled at runtime from recon string RVAs


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
    pd = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".pdata")
    funcs = []
    base = pd.PointerToRawData
    for off in range(base, base + pd.SizeOfRawData - 11, 12):
        begin = int.from_bytes(data[off:off + 4], "little")
        end = int.from_bytes(data[off + 4:off + 8], "little")
        if begin == 0:
            continue
        funcs.append((begin, end))
    funcs.sort()
    return funcs, [b for b, _ in funcs]


def function_of(funcs, begins, rva):
    if not begins:
        return None
    i = bisect.bisect_right(begins, rva) - 1
    if i < 0:
        return None
    b, e = funcs[i]
    return (b, e) if b <= rva < e else None


def find_string_rvas(pe, data, needle):
    nb = needle.encode("latin1")
    rvas, start = [], 0
    while True:
        i = data.find(nb, start)
        if i < 0:
            break
        rva = off_to_rva(pe, i)
        if rva is not None:
            rvas.append(rva)
        start = i + 1
    return rvas


def main():
    recon = json.loads(RECON.read_text(encoding="utf-8"))
    ma_funcs = set(int(s, 16) for s in recon["strings"]["Master Assistant"]["referenced_by_funcs"])
    # Collect every string RVA that any anchor/phase string lives at, plus their
    # pointer-table base RVAs, so we can flag RIP-refs in the hub.
    watch = set()
    for name in ("Master Assistant", "PROCESSING_LISTENING",
                 "LEARNING_EQ_AND_CLASSIFYING_GENRE",
                 "PROCESSING_SETTING_SIGNAL_CHAIN", "SmoothAudioDataStream"):
        for s in recon["strings"][name]["string_rvas"]:
            watch.add(int(s, 16))

    data = Path(DLL).read_bytes()
    pe = pefile.PE(DLL, fast_load=True)
    pe.parse_data_directories()
    image_base = pe.OPTIONAL_HEADER.ImageBase
    funcs, begins = parse_functions(pe, data)
    begins_set = set(begins)

    # Build full call graph.
    text = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text")
    tbytes = data[text.PointerToRawData:text.PointerToRawData + text.SizeOfRawData]
    text_rva0 = text.VirtualAddress
    callers, callees = {}, {}
    for m in re.compile(rb"[\xe8\xe9]", re.DOTALL).finditer(tbytes):
        i = m.start()
        if i + 5 > len(tbytes):
            continue
        rel = int.from_bytes(tbytes[i + 1:i + 5], "little", signed=True)
        src_rva = text_rva0 + i
        tgt_rva = src_rva + 5 + rel
        if tgt_rva not in begins_set:
            continue
        sf = function_of(funcs, begins, src_rva)
        if not sf:
            continue
        callees.setdefault(sf[0], set()).add(tgt_rva)
        callers.setdefault(tgt_rva, set()).add(sf[0])

    def report(fb, label):
        end = None
        for b, e in funcs:
            if b == fb:
                end = e
                break
        cl = callees.get(fb, set())
        cr = callers.get(fb, set())
        in_cluster = cl & ma_funcs
        anchor_hits = cl & {STATE_POLLER, STATE_HELPER_A, STATE_HELPER_B, SECONDARY, CALLER, DATA_STREAM}
        print(f"\n=== {label}: func 0x{fb:x} .. {('0x%x'%end) if end else '?'} size={end-fb if end else 0} ===")
        print(f"  callers ({len(cr)}): {[hex(x) for x in sorted(cr)]}")
        print(f"  callees ({len(cl)}):")
        for c in sorted(cl):
            tag = ""
            if c == STATE_POLLER: tag = "  ; POLLER"
            elif c == SECONDARY: tag = "  ; SECONDARY(apply)"
            elif c == STATE_HELPER_A: tag = "  ; state_helper_a"
            elif c == STATE_HELPER_B: tag = "  ; state_helper_b"
            elif c == DATA_STREAM: tag = "  ; data_stream_ctor"
            elif c == CALLER: tag = "  ; caller(0x166ca90)"
            elif c in ma_funcs: tag = "  ; MasterAssistant-cluster"
            print(f"     0x{c:x}{tag}")
        print(f"  anchor callees: {[hex(x) for x in sorted(anchor_hits)]}")
        print(f"  cluster callees: {[hex(x) for x in sorted(in_cluster)]}")
        # capstone full body
        if end:
            md = Cs(CS_ARCH_X86, CS_MODE_64)
            md.detail = True
            off0 = rva_to_off(pe, fb)
            code = data[off0:off0 + min(end - fb, 0x600)]
            print(f"  --- capstone (first ~60 insns) ---")
            for n, insn in enumerate(md.disasm(code, image_base + fb)):
                if n >= 60:
                    break
                note = ""
                op = insn.op_str
                if insn.mnemonic in ("call", "jmp") and op.startswith("0x"):
                    tgt = int(op, 16) - image_base
                    if tgt == STATE_POLLER: note = "  ; POLLER"
                    elif tgt == SECONDARY: note = "  ; SECONDARY"
                    elif tgt == STATE_HELPER_A: note = "  ; helper_a"
                    elif tgt == STATE_HELPER_B: note = "  ; helper_b"
                    elif tgt == DATA_STREAM: note = "  ; data_stream_ctor"
                    elif tgt in ma_funcs: note = "  ; MA-cluster"
                # RIP-relative data refs -> watch set?
                for op_obj in insn.operands:
                    if op_obj.type == 3:  # X86_OP_MEM
                        if op_obj.mem.base == 0:  # raw disp
                            pass
                print(f"    0x{insn.address-image_base:x}  {insn.mnemonic:<7} {insn.op_str}{note}")
            # Also scan full body for RIP refs to watched strings.
            print(f"  --- RIP-refs to watched strings (full body) ---")
            rip_op = re.compile(rb"[\x48\x4c][\x8d\x8b\x89\x3b\x39\x85]"
                                rb"[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]")
            hits = {}
            body = data[off0:off0 + (end - fb)]
            for m in rip_op.finditer(body):
                j = m.start()
                if j + 7 > len(body):
                    continue
                disp = int.from_bytes(body[j + 3:j + 7], "little", signed=True)
                tgt_rva = (fb + j) + 7 + disp
                if tgt_rva in watch:
                    sf = fb  # within this func
                    hits.setdefault(tgt_rva, 0)
                    hits[tgt_rva] += 1
            for t, c in sorted(hits.items()):
                print(f"     0x{t:x}  ({c}x)")

    report(HUB, "HUB +0xEABDB0 (calls BOTH poller + secondary)")
    # For comparison, also dump the documented caller.
    report(CALLER, "CALLER +0x166CA90 (documented, NOT the trigger)")

    # Now: who calls the HUB? Those are the trigger candidates.
    hub_callers = callers.get(HUB, set())
    print("\n\n=== CALLERS OF HUB +0xEABDB0 (the trigger candidates) ===")
    for c in sorted(hub_callers):
        end = None
        for b, e in funcs:
            if b == c:
                end = e; break
        cl = callees.get(c, set())
        cr = callers.get(c, set())
        anchor_hits = cl & {STATE_POLLER, STATE_HELPER_A, STATE_HELPER_B, SECONDARY, CALLER, DATA_STREAM, HUB}
        clstr = cl & ma_funcs
        print(f"  0x{c:x} .. {('0x%x'%end) if end else '?'} size={end-c if end else 0} "
              f"callers={len(cr)} callees={len(cl)} hub_call={int(HUB in cl)} "
              f"anchor_hits={[hex(x) for x in sorted(anchor_hits)]} "
              f"cluster_hits={[hex(x) for x in sorted(clstr)]}")

    # Examine the hub callers' bodies for the entry-point signature.
    print("\n=== CAPSTONE of HUB CALLERS (entry-point shape) ===")
    md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True
    for c in sorted(hub_callers):
        end = None
        for b, e in funcs:
            if b == c:
                end = e; break
        if not end:
            continue
        off0 = rva_to_off(pe, c)
        code = data[off0:off0 + min(end - c, 0x120)]
        print(f"\n  ;;; caller 0x{c:x} (size {end-c})")
        for n, insn in enumerate(md.disasm(code, image_base + c)):
            if n >= 22:
                break
            note = ""
            if insn.mnemonic in ("call","jmp") and insn.op_str.startswith("0x"):
                tgt = int(insn.op_str,16)-image_base
                if tgt == HUB: note="  ;-> HUB"
                elif tgt == STATE_POLLER: note="  ;-> POLLER"
                elif tgt == SECONDARY: note="  ;-> SECONDARY"
            print(f"    0x{insn.address-image_base:x}  {insn.mnemonic:<7} {insn.op_str}{note}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
