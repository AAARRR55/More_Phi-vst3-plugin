#!/usr/bin/env python3
"""Independent cross-check: find the BRIDGE function that connects Ozone's Master
Assistant controller cluster to the audio-feed path. The trigger that STARTS an
analysis must (a) be reachable from an entry/UI path and (b) drive audio into the
listening phase. Offline only; reads the DLL from disk.
"""
from __future__ import annotations
import json, re, bisect
from pathlib import Path
import pefile

DLL = r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll"
RECON = "tools/live_captures/static/ozone_recon.json"

def parse_functions(pe, data):
    pd = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".pdata")
    funcs = []
    for off in range(pd.PointerToRawData, pd.PointerToRawData + pd.SizeOfRawData - 11, 12):
        b = int.from_bytes(data[off:off+4], "little")
        e = int.from_bytes(data[off+4:off+8], "little")
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
    ib = pe.OPTIONAL_HEADER.ImageBase
    funcs, begins = parse_functions(pe, data)
    begins_set = set(begins)
    text = next(s for s in pe.sections if s.Name.rstrip(b"\x00") == b".text")
    code = data[text.PointerToRawData:text.PointerToRawData + text.SizeOfRawData]
    t0 = text.VirtualAddress
    n = len(code)

    fwd, rev = {}, {}  # src_func -> {dst}, dst_func -> {src}
    for m in re.compile(rb"[\xe8\xe9]", re.DOTALL).finditer(code):
        i = m.start()
        if i + 5 > n:
            continue
        rel = int.from_bytes(code[i+1:i+5], "little", signed=True)
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
    ma_funcs = set(int(x, 16) for x in recon["strings"]["Master Assistant"]["referenced_by_funcs"])
    controller = set(ma_funcs)
    controller |= {0x166ca90}
    controller |= set(rev.get(0x166ca90, set()))      # callers of the assistant caller
    controller |= set(rev.get(0xeabdb0, set()))        # callers of the poller+applier caller
    audio_feed = {0xfd7f30, 0xfbd0b0, 0x1072860}

    # Forward BFS up to 3 hops from each controller func; flag those reaching audio_feed.
    def reaches(start, hops=3):
        seen = {start}
        frontier = {start}
        for _ in range(hops):
            nxt = set()
            for f in frontier:
                for d in fwd.get(f, ()):
                    if d in audio_feed:
                        return True, d
                    if d not in seen:
                        seen.add(d)
                        nxt.add(d)
            frontier = nxt
            if not frontier:
                break
        return False, None

    print("controller cluster size:", len(controller))
    bridges = []
    for c in sorted(controller):
        ok, hit = reaches(c)
        if ok:
            # entry-ish: has callers from OUTSIDE the 0x16xxxx / 0xeaxxxx assistant regions
            callers = rev.get(c, set())
            outside = [cl for cl in callers if not (0xe00000 <= cl < 0xf00000 or 0x1600000 <= cl < 0x1700000)]
            bridges.append((c, hit, len(callers), len(outside), sorted(outside)[:6]))

    print("\n=== controller funcs that reach the audio-feed path (<=3 hops) ===")
    for c, hit, nc, no, out in sorted(bridges, key=lambda x: -x[3]):
        print(f"  func=0x{c:x} reaches_audio=0x{hit:x} callers={nc} outside_callers={no} {['0x%x'%o for o in out]}")

    # Also: who calls the audio_feed funcs directly (the immediate feed driver)?
    print("\n=== direct callers of audio-feed funcs ===")
    for af in sorted(audio_feed):
        cs = sorted(rev.get(af, set()))
        print(f"  0x{af:x}: callers={['0x%x'%c for c in cs]}")

if __name__ == "__main__":
    main()
