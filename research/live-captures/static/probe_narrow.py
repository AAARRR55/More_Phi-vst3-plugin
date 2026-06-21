#!/usr/bin/env python3
"""Option 3 — narrow the Master Assistant TRIGGER candidate set (offline).

0xD58A20 was refuted as parameter-metadata ENUMERATION (strings: Maximizer,
Threshold, ElementChain, "{module}.{param}"). The real trigger is a sibling in
the Master-Assistant cluster. This probe ranks cluster methods by the trigger
signature: reaches the audio-feed/poller pipeline, is vtable-dispatched (UI shape),
and is NOT enumeration-like.

Offline only; reads iZOzonePro.dll from disk.
"""
from __future__ import annotations
import bisect, json, re
from pathlib import Path
import pefile

DLL = r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll"
RECON = "tools/live_captures/static/ozone_recon.json"
IB = 0x180000000

# Audio-feed / state pipeline targets the trigger must drive.
PIPELINE = {0xE9FC30, 0xEABDB0, 0xEAD3E0, 0xEAD930, 0xEA4FD0, 0xEA1B20, 0xFD7F30}
# Strings that mark parameter-metadata enumeration (what 0xD58A20 actually is).
ENUM_STR = [b"Maximizer", b"Threshold", b"ElementChain", b"{}.{}", b"%s.%s"]
PHASE_STR = [b"PROCESSING_LISTENING", b"LEARNING_EQ_AND_CLASSIFYING_GENRE",
             b"PROCESSING_SETTING_SIGNAL_CHAIN"]


def parse_functions(pe, data):
    pd = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".pdata")
    funcs = []
    for off in range(pd.PointerToRawData, pd.PointerToRawData + pd.SizeOfRawData - 11, 12):
        b = int.from_bytes(data[off:off + 4], "little")
        e = int.from_bytes(data[off + 4:off + 8], "little")
        if b:
            funcs.append((b, e))
    funcs.sort()
    return funcs, [b for b, _ in funcs]


def fof(funcs, begins, rva):
    i = bisect.bisect_right(begins, rva) - 1
    if i < 0:
        return None
    b, e = funcs[i]
    return (b, e) if b <= rva < e else None


def main():
    data = Path(DLL).read_bytes()
    pe = pefile.PE(DLL, fast_load=True)
    funcs, begins = parse_functions(pe, data)
    begins_set = set(begins)
    text = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text")
    code = data[text.PointerToRawData:text.PointerToRawData + text.SizeOfRawData]
    t0 = text.VirtualAddress
    n = len(code)

    fwd, rev = {}, {}
    for m in re.compile(rb"[\xe8\xe9]", re.DOTALL).finditer(code):
        i = m.start()
        if i + 5 > n:
            continue
        rel = int.from_bytes(code[i + 1:i + 5], "little", signed=True)
        s = t0 + i
        t = s + 5 + rel
        if t not in begins_set:
            continue
        sf = fof(funcs, begins, s)
        if not sf:
            continue
        fwd.setdefault(sf[0], set()).add(t)
        rev.setdefault(t, set()).add(sf[0])

    recon = json.load(open(RECON, encoding="utf-8"))
    cluster = sorted(set(int(x, 16) for x in recon["strings"]["Master Assistant"]["referenced_by_funcs"]))

    # vtable slots: cluster funcs whose VA appears as a qword in .rdata (dispatched, not direct-called)
    rdata = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".rdata")
    rbytes = data[rdata.PointerToRawData:rdata.PointerToRawData + rdata.SizeOfRawData]
    slot_funcs = set()
    for f in cluster:
        needle = (IB + f).to_bytes(8, "little")
        if rbytes.find(needle) >= 0:
            slot_funcs.add(f)

    def reaches(start, hops=6):
        seen, front = {start}, {start}
        for _ in range(hops):
            nxt = set()
            for f in front:
                for d in fwd.get(f, ()):
                    if d in PIPELINE:
                        return d
                    if d not in seen:
                        seen.add(d)
                        nxt.add(d)
            front = nxt
            if not front:
                break
        return None

    def body_has(frva, needles):
        fr = fof(funcs, begins, frva)
        if not fr:
            return []
        b, e = fr
        off = text.PointerToRawData + (b - t0)
        body = data[off:off + (e - b)]
        return [nd.decode("latin1") for nd in needles if body.find(nd) >= 0]

    print(f"Master Assistant cluster: {len(cluster)} funcs; vtable-slot-dispatched: {len(slot_funcs)}")
    print("\n=== ranked trigger candidates (reach pipeline, vtable-dispatched, NOT enumeration) ===\n")
    rows = []
    for f in cluster:
        hit = reaches(f)
        enum = body_has(f, ENUM_STR)
        phase = body_has(f, PHASE_STR)
        callers = rev.get(f, set())
        is_slot = f in slot_funcs
        score = 0
        if hit:
            score += 3
        if is_slot:
            score += 2
        if not enum:
            score += 2
        if phase:
            score += 2
        if len(callers) <= 1:
            score += 1  # UI-dispatch shape
        rows.append((score, f, hit, enum, phase, len(callers), is_slot))

    for score, f, hit, enum, phase, nc, is_slot in sorted(rows, reverse=True):
        if score < 3:
            continue
        tag = "TRIGGER?" if (hit and not enum) else ""
        print(f"  score={score} 0x{f:x} reach_pipeline={hex(hit) if hit else '-'} "
              f"enum={enum} phase={phase} callers={nc} vtable_slot={is_slot}  {tag}")

    # Specifically: who calls the pipeline root 0xE9FC30? (the trigger should be among them)
    print("\n=== direct callers of pipeline root 0xE9FC30 ===")
    for c in sorted(rev.get(0xE9FC30, set())):
        in_clust = c in cluster
        enum = body_has(c, ENUM_STR)
        print(f"  0x{c:x} in_cluster={in_clust} enum={enum} callers={len(rev.get(c,set()))}")


if __name__ == "__main__":
    main()
