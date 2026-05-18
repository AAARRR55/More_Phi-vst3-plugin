#!/usr/bin/env python3
"""
Find candidate internal iZotope IPC / Assistant functions by string xref.

The script does static, read-only PE analysis:
  1. Finds ASCII and UTF-16LE string occurrences.
  2. Converts file offsets to RVAs/VAs.
  3. Disassembles executable sections.
  4. Finds RIP-relative or immediate references to those strings.
  5. Groups references by x64 runtime-function ranges when available.

This does not patch or modify binaries. The output is intended to feed
tools/izotope_internal_trace.py for runtime observation.
"""

from __future__ import annotations

import argparse
import bisect
import json
import re
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any

import capstone
import pefile
from capstone.x86_const import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP


DEFAULT_PATTERNS = [
    "Ozone IPC 1",
    "Neutron IPC 2",
    "SmoothAudioDataStream",
    "Smooth Audio Streamer",
    "AuxStream",
    "Audio Data Source",
    "Track Assist Processor",
    "Balance Assistant Learner",
    "Master Assistant",
    "Master Assistant: Launched",
    "PROCESSING_LISTENING",
    "PROCESSING_SETTING_SIGNAL_CHAIN",
    "LEARNING_EQ_AND_CLASSIFYING_GENRE",
    "OZONEPROMS",
    "NEUTRONPROMS",
    "TrackAssistantSupportFolder",
    "disconnected_mapping_environment",
    "CreateNamedPipeFailed",
    "Error preparing a server side named pipe",
    "Could not connect client socket",
    "Could not open socket",
    "inproc://",
    "ipc://",
    "tcp://",
]


DEFAULT_MODULES = [
    r"C:\Program Files\Common Files\VST3\iZotope\iZOzonePro.dll",
    r"C:\Program Files\Common Files\VST3\iZotope\iZNeutron3VisualMixer.dll",
    r"C:\Program Files\Common Files\VST3\iZotope\iZRelay.dll",
    r"C:\Program Files\Common Files\VST3\iZotope\iZNeutronPro.dll",
    r"C:\Program Files\Common Files\VST3\iZotope\iZTonalBalanceControlPro.dll",
]


def sanitize_preview(data: bytes) -> str:
    return "".join(chr(b) if 32 <= b < 127 else "." for b in data).replace("\n", "\\n")


def find_all(data: bytes, needle: bytes, limit: int) -> list[int]:
    hits: list[int] = []
    pos = 0
    while len(hits) < limit:
        idx = data.find(needle, pos)
        if idx < 0:
            break
        hits.append(idx)
        pos = idx + 1
    return hits


def section_name(section: pefile.SectionStructure) -> str:
    return section.Name.rstrip(b"\0").decode("ascii", "replace")


def executable_sections(pe: pefile.PE) -> list[pefile.SectionStructure]:
    result = []
    for section in pe.sections:
        if section.Characteristics & 0x20000000:  # IMAGE_SCN_MEM_EXECUTE
            result.append(section)
    return result


def rva_to_section(pe: pefile.PE, rva: int) -> str:
    for section in pe.sections:
        start = section.VirtualAddress
        end = start + max(section.Misc_VirtualSize, section.SizeOfRawData)
        if start <= rva < end:
            return section_name(section)
    return ""


def load_runtime_functions(pe: pefile.PE) -> list[tuple[int, int]]:
    try:
        pe.parse_data_directories(
            directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]]
        )
    except Exception:
        return []
    funcs = []
    for entry in getattr(pe, "DIRECTORY_ENTRY_EXCEPTION", []) or []:
        begin = int(entry.struct.BeginAddress)
        end = int(entry.struct.EndAddress)
        if begin and end > begin:
            funcs.append((begin, end))
    funcs.sort()
    return funcs


def find_function(runtime_funcs: list[tuple[int, int]], rva: int) -> tuple[int, int] | None:
    if not runtime_funcs:
        return None
    starts = [f[0] for f in runtime_funcs]
    i = bisect.bisect_right(starts, rva) - 1
    if i >= 0:
        begin, end = runtime_funcs[i]
        if begin <= rva < end:
            return begin, end
    return None


def fallback_function_start(code: bytes, section_rva: int, rva: int) -> int:
    # Conservative x64 prologue scan. Used only when pdata is absent.
    off = max(0, rva - section_rva)
    lo = max(0, off - 512)
    patterns = [b"\x40\x53", b"\x48\x89\x5c\x24", b"\x48\x83\xec", b"\x48\x8b\xc4"]
    best = lo
    for i in range(off, lo, -1):
        for pat in patterns:
            if code[i : i + len(pat)] == pat:
                return section_rva + i
    return section_rva + best


def interval_lookup(starts: list[int], intervals: list[tuple[int, int, Any]], va: int) -> Any | None:
    i = bisect.bisect_right(starts, va) - 1
    if i >= 0:
        start, end, idx = intervals[i]
        if start <= va < end:
            return idx
    return None


def analyze_module(path: Path, patterns: list[str], max_hits_per_pattern: int) -> dict[str, Any]:
    data = path.read_bytes()
    pe = pefile.PE(str(path), fast_load=False)
    image_base = int(pe.OPTIONAL_HEADER.ImageBase)
    pointer_size = 8 if pe.OPTIONAL_HEADER.Magic == 0x20B else 4

    strings: list[dict[str, Any]] = []
    for pattern in patterns:
        encodings = [("ascii", pattern.encode("utf-8"))]
        if all(ord(ch) < 128 for ch in pattern):
            encodings.append(("utf16le", pattern.encode("utf-16le")))
        for enc_name, needle in encodings:
            for off in find_all(data, needle, max_hits_per_pattern):
                try:
                    rva = pe.get_rva_from_offset(off)
                except Exception:
                    continue
                preview = sanitize_preview(data[max(0, off - 64) : min(len(data), off + len(needle) + 128)])
                strings.append(
                    {
                        "index": len(strings),
                        "pattern": pattern,
                        "encoding": enc_name,
                        "file_offset": off,
                        "rva": rva,
                        "va": image_base + rva,
                        "section": rva_to_section(pe, rva),
                        "length": len(needle),
                        "preview": preview[:260],
                    }
                )

    pointer_refs: list[dict[str, Any]] = []
    rva_map = {int(s["rva"]): int(s["index"]) for s in strings}
    va_map = {int(s["va"]): int(s["index"]) for s in strings}
    # Strings are usually referenced through generated dictionaries/tables in
    # .rdata rather than directly from code. Chase both 32-bit RVAs and 64-bit
    # image VAs from non-executable sections.
    for section in pe.sections:
        if section.Characteristics & 0x20000000:  # executable
            continue
        sec_data = section.get_data()
        sec_rva = int(section.VirtualAddress)
        sec_off = int(section.PointerToRawData)
        for i in range(0, max(0, len(sec_data) - 3), 4):
            value = int.from_bytes(sec_data[i : i + 4], "little", signed=False)
            sidx = rva_map.get(value)
            if sidx is not None:
                pointer_refs.append(
                    {
                        "kind": "rva32",
                        "file_offset": sec_off + i,
                        "rva": sec_rva + i,
                        "va": image_base + sec_rva + i,
                        "section": section_name(section),
                        "target_string_index": sidx,
                        "target_pattern": strings[sidx]["pattern"],
                        "target_rva": strings[sidx]["rva"],
                    }
                )
        if pointer_size == 8:
            for i in range(0, max(0, len(sec_data) - 7), 8):
                value = int.from_bytes(sec_data[i : i + 8], "little", signed=False)
                sidx = va_map.get(value)
                if sidx is not None:
                    pointer_refs.append(
                        {
                            "kind": "va64",
                            "file_offset": sec_off + i,
                            "rva": sec_rva + i,
                            "va": image_base + sec_rva + i,
                            "section": section_name(section),
                            "target_string_index": sidx,
                            "target_pattern": strings[sidx]["pattern"],
                            "target_rva": strings[sidx]["rva"],
                        }
                    )

    string_intervals = [(s["va"], s["va"] + s["length"], ("string", s["index"])) for s in strings]
    pointer_intervals = [(p["va"], p["va"] + (8 if p["kind"] == "va64" else 4), ("pointer", i)) for i, p in enumerate(pointer_refs)]
    intervals = sorted(string_intervals + pointer_intervals)
    starts = [x[0] for x in intervals]
    runtime_funcs = load_runtime_functions(pe)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64 if pointer_size == 8 else capstone.CS_MODE_32)
    md.detail = True

    xrefs: list[dict[str, Any]] = []
    for section in executable_sections(pe):
        sec_rva = int(section.VirtualAddress)
        sec_va = image_base + sec_rva
        code = section.get_data()
        for insn in md.disasm(code, sec_va):
            target_vas: list[int] = []
            try:
                for op in insn.operands:
                    if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                        target_vas.append(insn.address + insn.size + int(op.mem.disp))
                    elif op.type == X86_OP_IMM:
                        imm = int(op.imm) & 0xFFFFFFFFFFFFFFFF
                        target_vas.append(imm)
            except Exception:
                continue

            for target_va in target_vas:
                interval_payload = interval_lookup(starts, intervals, target_va)
                if interval_payload is None:
                    continue
                ref_kind, ref_index = interval_payload
                insn_rva = insn.address - image_base
                func = find_function(runtime_funcs, insn_rva)
                if func:
                    func_begin, func_end = func
                else:
                    func_begin = fallback_function_start(code, sec_rva, insn_rva)
                    func_end = 0
                if ref_kind == "string":
                    s = strings[ref_index]
                    target_string_index = ref_index
                    target_ref_kind = "direct_string"
                else:
                    pref = pointer_refs[ref_index]
                    s = strings[int(pref["target_string_index"])]
                    target_string_index = int(pref["target_string_index"])
                    target_ref_kind = f"via_{pref['kind']}_pointer"
                xrefs.append(
                    {
                        "string_index": target_string_index,
                        "target_ref_kind": target_ref_kind,
                        "pattern": s["pattern"],
                        "encoding": s["encoding"],
                        "target_rva": target_va - image_base,
                        "from_rva": insn_rva,
                        "from_va": insn.address,
                        "instruction": f"{insn.mnemonic} {insn.op_str}".strip(),
                        "function_rva": func_begin,
                        "function_end_rva": func_end,
                        "function_size": (func_end - func_begin) if func_end else 0,
                        "code_section": rva_to_section(pe, insn_rva),
                    }
                )

    grouped: dict[int, dict[str, Any]] = {}
    for xr in xrefs:
        frva = int(xr["function_rva"])
        g = grouped.setdefault(
            frva,
            {
                "function_rva": frva,
                "function_end_rva": xr.get("function_end_rva", 0),
                "function_size": xr.get("function_size", 0),
                "xref_count": 0,
                "patterns": Counter(),
                "refs": [],
            },
        )
        g["xref_count"] += 1
        g["patterns"][xr["pattern"]] += 1
        if len(g["refs"]) < 12:
            g["refs"].append(
                {
                    "from_rva": xr["from_rva"],
                    "instruction": xr["instruction"],
                    "pattern": xr["pattern"],
                    "encoding": xr["encoding"],
                }
            )

    candidate_functions = []
    for g in grouped.values():
        g["patterns"] = dict(g["patterns"].most_common())
        score = g["xref_count"]
        score += 10 * sum(
            1
            for p in g["patterns"]
            if p
            in {
                "Ozone IPC 1",
                "Neutron IPC 2",
                "SmoothAudioDataStream",
                "AuxStream",
                "Track Assist Processor",
                "Balance Assistant Learner",
                "Master Assistant: Launched",
                "PROCESSING_LISTENING",
                "PROCESSING_SETTING_SIGNAL_CHAIN",
            }
        )
        g["score"] = score
        candidate_functions.append(g)

    candidate_functions.sort(key=lambda item: (item["score"], item["xref_count"]), reverse=True)

    return {
        "path": str(path),
        "module_name": path.name,
        "image_base": image_base,
        "machine": hex(pe.FILE_HEADER.Machine),
        "string_count": len(strings),
        "pointer_ref_count": len(pointer_refs),
        "xref_count": len(xrefs),
        "strings": strings,
        "pointer_refs": pointer_refs[:10000],
        "xrefs": xrefs,
        "candidate_functions": candidate_functions[:80],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--module", action="append", dest="modules", help="PE module path. Can be repeated.")
    parser.add_argument("--pattern", action="append", dest="patterns", help="String pattern. Can be repeated.")
    parser.add_argument("--out", default="tools/live_captures/ipc_decode/izotope_ipc_xrefs.json")
    parser.add_argument("--max-hits-per-pattern", type=int, default=64)
    args = parser.parse_args()

    modules = [Path(p) for p in (args.modules or DEFAULT_MODULES)]
    patterns = args.patterns or DEFAULT_PATTERNS
    modules = [p for p in modules if p.exists()]
    if not modules:
        raise SystemExit("No modules found.")

    result = {
        "patterns": patterns,
        "modules": [analyze_module(path, patterns, args.max_hits_per_pattern) for path in modules],
    }

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, indent=2), encoding="utf-8")

    print(f"Wrote {out}")
    for module in result["modules"]:
        print(f"\n== {module['module_name']} ==")
        print(f"strings={module['string_count']} xrefs={module['xref_count']}")
        for fn in module["candidate_functions"][:12]:
            pats = ", ".join(list(fn["patterns"].keys())[:4])
            print(
                f"score={fn['score']:>3} xrefs={fn['xref_count']:>3} "
                f"rva=0x{fn['function_rva']:X} size={fn.get('function_size', 0)} patterns={pats}"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
