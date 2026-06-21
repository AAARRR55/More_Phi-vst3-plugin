#!/usr/bin/env python3
"""Stage 0b: reachability-intersection trigger candidate finder.

Companion to tools/ozone_static_recon.py. The recon tool builds the first-level
call graph and reports anchor callers/callees, but does not compute transitive
reachability — which is exactly what's needed to find the *bridge* between the
Master Assistant controller cluster and the SmoothAudioDataStream audio-feed
region.

Hypothesis under test (from the operator's independent read of the live
findings): the real Assistant trigger is the minimal function whose forward
reachability intersects BOTH:

  - the controller cluster: {0x166ca90 (caller-of poller+applier), 0xeabdb0}
    with terminal markers {0xead3e0 (poller), 0xead930 (applier)}
  - the audio-feed region:  {0xfbd0b0, 0x1072860}
    with terminal marker    {0xfd7f30 (SmoothAudioDataStream ctor)}

An analysis run must BOTH drive the Assistant state machine (controller) AND
set up the smooth audio data stream (feed). A function upstream of both regions
is therefore a strong trigger candidate. Idle pollers and apply-only helpers
fail the intersection test by construction.

This tool re-derives the FULL directed call graph from iZOzonePro.dll (reusing
the proven disasm loop from ozone_static_recon.build_graph) and runs a forward
BFS from each function. The candidate set is the intersection of:
  { f : controller_marker in forward_reach(f) }  AND
  { f : audiofeed_marker in forward_reach(f) }

Authorized scope (OZONE_IPC_RESEARCH_METHODOLOGY.md sec 5.2): plain-string
xrefs and call-graph mapping of vendor code More-Phi hosts itself. No license,
PACE/iLok, anti-tamper, or integrity-check logic is decoded; no process attached.

Usage:
    py tools/ozone_trigger_reach.py
    py tools/ozone_trigger_reach.py --dll "<path>" --out tools/live_captures/static/bridge_candidates.json
"""
from __future__ import annotations

import argparse
import json
import sys
from collections import deque
from pathlib import Path

# Import the proven disasm + call-graph builder from the recon tool.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ozone_static_recon as recon  # noqa: E402

DEFAULT_DLL = r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll"

# ── Anchor sets (operator's independent read of the live findings) ────────────
# Controller cluster: the known caller that fans out to poller + applier, plus
# the second function the live trace saw reaching both.
CONTROLLER_CLUSTER = {
    0x166CA90,   # ozone_master_assistant_caller (calls poller + applier)
    0xEABDB0,    # also reaches poller + applier (per operator read)
}
# Callers of 0x166ca90 confirmed by recon: 0xd4b830, 0x163dfa0. These are
# included as controller-cluster frontier so the bridge can be found upstream.
CONTROLLER_FRONTIER = {
    0xD4B830,
    0x163DFA0,
}
CONTROLLER_MARKERS = CONTROLLER_CLUSTER | CONTROLLER_FRONTIER | {0xEAD3E0, 0xEAD930}

# Audio-feed region: the known callers of the SmoothAudioDataStream ctor.
AUDIOFEED_REGION = {
    0xFBD0B0,
    0x1072860,
}
AUDIOFEED_MARKERS = AUDIOFEED_REGION | {0xFD7F30}


def build_full_graph(dll_path: str):
    """Return (callees: dict[int,set[int]], funcs: list, begins: list)."""
    data = Path(dll_path).read_bytes()
    pe = pefile.PE(dll_path, fast_load=True)  # noqa: F821
    pe.parse_data_directories()
    funcs, begins = recon.parse_functions(pe, data)
    # str_target_set empty: we don't need string xrefs here, only call edges.
    callers, callees, _string_user_funcs, _stats = recon.build_graph(
        pe, data, funcs, begins, set())
    return callees, funcs, begins


def build_indirect_graph(dll_path: str):
    """Full direct + RIP-relative-indirect call graph.

    Extends the direct-call graph (recon.build_graph) with edges from
    `call qword ptr [rip+disp]` sites where [rip+disp] lands in a readable
    data section and dereferences to a value that looks like a function RVA
    (i.e. points inside .text). This resolves vtable / function-pointer-table
    dispatch that is invisible to pure `call imm` analysis.

    Still static, no process attach: we only read the DLL bytes on disk.

    Returns (callees: dict[int,set[int]], indirect_count: int, funcs, begins).
    """
    import bisect
    from capstone import Cs, CS_ARCH_X86, CS_MODE_64
    from capstone.x86 import X86_OP_MEM, X86_OP_IMM, X86_REG_RIP

    data = Path(dll_path).read_bytes()
    pe = pefile.PE(dll_path, fast_load=True)  # noqa: F821
    pe.parse_data_directories()
    image_base = pe.OPTIONAL_HEADER.ImageBase
    funcs, begins = recon.parse_functions(pe, data)
    callers, callees, _string_user_funcs, _stats = recon.build_graph(
        pe, data, funcs, begins, set())

    # Section table for RVA -> file-offset + readability checks.
    def rva_to_off(rva: int):
        for s in pe.sections:
            span = max(s.Misc_VirtualSize, s.SizeOfRawData)
            if s.VirtualAddress <= rva < s.VirtualAddress + span:
                return s.PointerToRawData + (rva - s.VirtualAddress), s
        return None, None

    text_section = next((s for s in pe.sections
                         if s.Name.rstrip(b"\x00") == b".text"), None)
    text_va_lo = text_section.VirtualAddress
    text_va_hi = text_section.VirtualAddress + max(
        text_section.Misc_VirtualSize, text_section.SizeOfRawData)

    def read_qword_at_rva(rva: int):
        off, sec = rva_to_off(rva)
        if off is None or off + 8 > len(data):
            return None
        return int.from_bytes(data[off:off + 8], "little")

    def func_of(rva: int):
        if not begins:
            return None
        i = bisect.bisect_right(begins, rva) - 1
        if i < 0:
            return None
        b, e = funcs[i]
        return (b, e) if b <= rva < e else None

    # Re-walk .text looking for indirect call/jmp via [rip+disp], resolving the
    # disp to a data pointer, dereferencing once, and adding an edge if the
    # pointed-to value is inside .text (a function RVA).
    code = data[text_section.PointerToRawData:
                text_section.PointerToRawData + text_section.SizeOfRawData]
    text_va = image_base + text_section.VirtualAddress
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True

    indirect_resolved = 0
    n = len(code)
    pos = 0
    win = 1 << 20
    while pos < n:
        chunk = code[pos:min(pos + win, n)]
        progressed = False
        last_end = pos
        for insn in md.disasm(chunk, text_va + pos):
            last_end = (insn.address - text_va) + insn.size
            progressed = True
            # Match `call qword ptr [rip+disp]` and `jmp qword ptr [rip+disp]`.
            if insn.mnemonic in ("call", "jmp") and insn.operands:
                op = insn.operands[0]
                if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                    target_data_rva = (insn.address + insn.size + op.mem.disp
                                       - image_base)
                    ptr_value = read_qword_at_rva(target_data_rva)
                    if ptr_value is None:
                        continue
                    ptr_rva = ptr_value - image_base
                    if text_va_lo <= ptr_rva < text_va_hi:
                        src_f = func_of(insn.address)
                        dst_f = func_of(image_base + ptr_rva)
                        if src_f and dst_f:
                            callees.setdefault(src_f[0], set()).add(dst_f[0])
                            indirect_resolved += 1
        pos = last_end if progressed else pos + 1

    return callees, indirect_resolved, funcs, begins


def forward_reach(start: int, callees: dict) -> set[int]:
    """All nodes reachable from start via forward call edges (excludes start)."""
    seen: set[int] = set()
    q = deque([start])
    while q:
        n = q.popleft()
        for nxt in callees.get(n, ()):  # callees values may be RVAs or func begins
            if nxt not in seen:
                seen.add(nxt)
                q.append(nxt)
    return seen


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dll", default=DEFAULT_DLL)
    ap.add_argument("--out", default=None)
    ap.add_argument("--indirect", action="store_true",
                    help="Also resolve RIP-relative indirect call/jmp edges.")
    args = ap.parse_args()

    global pefile
    import pefile  # type: ignore

    if args.indirect:
        print(f"building DIRECT + INDIRECT call graph from {args.dll} ...")
        callees, indirect_count, funcs, begins = build_indirect_graph(args.dll)
        total_edges = sum(len(v) for v in callees.values())
        print(f"functions={len(funcs)} direct+indirect_edges={total_edges} "
              f"resolved_indirect_edges={indirect_count}")
    else:
        print(f"building full call graph from {args.dll} ...")
        callees, funcs, begins = build_full_graph(args.dll)
        print(f"functions={len(funcs)} edges={sum(len(v) for v in callees.values())}")

    # Map any function-begin that equals a marker, plus verify markers are real
    # function begins (some markers may be mid-function RVAs the live trace used).
    func_begin_set = set(begins)

    def resolve(m: int) -> int:
        # Snap a marker RVA to the function begin that contains it.
        import bisect
        if not begins:
            return m
        i = bisect.bisect_right(begins, m) - 1
        if i < 0:
            return m
        return begins[i] if begins[i] <= m else m

    ctrl_resolved = {resolve(m) for m in CONTROLLER_MARKERS}
    feed_resolved = {resolve(m) for m in AUDIOFEED_MARKERS}
    print(f"controller markers (snapped to func begins): "
          f"{[hex(x) for x in sorted(ctrl_resolved)]}")
    print(f"audio-feed markers (snapped to func begins): "
          f"{[hex(x) for x in sorted(feed_resolved)]}")

    # Forward-reachability BFS is O(V*E) over all 90k functions. To bound it we
    # only need reachability for functions that could plausibly reach BOTH
    # regions. First compute, for each marker, its *callers* via reverse BFS, then
    # intersect the reverse-reachable sets of controller vs feed.
    print("computing reverse reachability (callers) for each marker ...")
    # Build reverse graph.
    reverse: dict[int, set[int]] = {}
    for src, dsts in callees.items():
        for d in dsts:
            reverse.setdefault(d, set()).add(src)

    def reverse_reach(targets: set[int]) -> set[int]:
        seen: set[int] = set()
        q = deque(targets)
        while q:
            n = q.popleft()
            for prev in reverse.get(n, ()):
                if prev not in seen:
                    seen.add(prev)
                    q.append(prev)
        return seen

    ctrl_upstream = reverse_reach(ctrl_resolved)
    feed_upstream = reverse_reach(feed_resolved)
    print(f"controller-upstream functions: {len(ctrl_upstream)}")
    print(f"audio-feed-upstream functions: {len(feed_upstream)}")

    # The bridge candidates = functions upstream of BOTH regions.
    # A function that is itself a marker is included only if it is also upstream
    # of the *other* region (e.g. a controller marker that reaches the feed).
    bridges = ctrl_upstream & feed_upstream
    # Also consider the markers themselves: does any controller marker reach the
    # feed, or vice versa? Compute forward reach for the markers directly.
    for m in ctrl_resolved:
        if AUDIOFEED_MARKERS & forward_reach(m, callees):
            bridges.add(m)
    for m in feed_resolved:
        if CONTROLLER_MARKERS & forward_reach(m, callees):
            bridges.add(m)

    print(f"\nBRIDGE CANDIDATES (upstream of both regions): {len(bridges)}")
    # Rank: prefer functions that are themselves markers (known anchor) or that
    # reach the most distinct markers forward. Tiebreak by function address.
    def score(f: int) -> tuple:
        fr = forward_reach(f, callees)
        ctrl_hits = len(ctrl_resolved & fr)
        feed_hits = len(feed_resolved & fr)
        is_anchor = f in (CONTROLLER_MARKERS | AUDIOFEED_MARKERS)
        return (is_anchor, ctrl_hits + feed_hits, -f)

    ranked = sorted(bridges, key=score, reverse=True)

    report = {
        "dll": str(args.dll),
        "controller_markers": [hex(x) for x in sorted(ctrl_resolved)],
        "audiofeed_markers": [hex(x) for x in sorted(feed_resolved)],
        "controller_upstream_count": len(ctrl_upstream),
        "audiofeed_upstream_count": len(feed_upstream),
        "bridge_candidate_count": len(bridges),
        "bridge_candidates": [
            {
                "func_rva": hex(f),
                "is_known_anchor": f in (CONTROLLER_MARKERS | AUDIOFEED_MARKERS),
                "forward_reach_controller_hits": len(ctrl_resolved & forward_reach(f, callees)),
                "forward_reach_audiofeed_hits": len(feed_resolved & forward_reach(f, callees)),
            }
            for f in ranked
        ],
    }

    out_path = Path(args.out) if args.out else Path(
        "tools/live_captures/static/bridge_candidates.json")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(report, indent=1), encoding="utf-8")
    print("\ntop 10 bridge candidates (ranked):")
    for c in report["bridge_candidates"][:10]:
        print(f"  {c['func_rva']} anchor={c['is_known_anchor']} "
              f"ctrl_hits={c['forward_reach_controller_hits']} "
              f"feed_hits={c['forward_reach_audiofeed_hits']}")
    print(f"\nfull report -> {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
