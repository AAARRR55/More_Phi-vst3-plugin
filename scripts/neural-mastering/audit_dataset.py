#!/usr/bin/env python3
"""Audit neural mastering dataset governance metadata."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


def _load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def audit_items(items: list[dict[str, Any]]) -> dict[str, Any]:
    failures: list[str] = []
    seen_by_split: dict[str, set[str]] = {}

    for index, item in enumerate(items):
        prefix = item.get("id") or item.get("itemId") or f"item[{index}]"
        if not item.get("provenanceComplete", False):
            failures.append(f"{prefix}: provenance incomplete")
        if item.get("licenseStatus") != "approved":
            failures.append(f"{prefix}: license not approved")
        if item.get("referenceQuality") != "reviewed":
            failures.append(f"{prefix}: reference quality not reviewed")
        if item.get("unsupportedMaterial", False):
            failures.append(f"{prefix}: unsupported material")

        split = str(item.get("split", "unassigned"))
        source = str(item.get("sourceId") or item.get("sourceFingerprint") or "")
        if not source:
            failures.append(f"{prefix}: missing source identity")
        if split == "unassigned":
            failures.append(f"{prefix}: split unassigned")

        for other_split, sources in seen_by_split.items():
            if other_split != split and source and source in sources:
                failures.append(f"{prefix}: source appears in split {other_split} and {split}")
        seen_by_split.setdefault(split, set()).add(source)

    return {
        "gateId": "G10",
        "itemCount": len(items),
        "passed": not failures,
        "failures": failures,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path, help="Dataset manifest JSON with an items array")
    parser.add_argument("--output", type=Path, help="Optional JSON report path")
    args = parser.parse_args()

    payload = _load_json(args.manifest)
    items = payload.get("items", payload if isinstance(payload, list) else [])
    if not isinstance(items, list):
        raise SystemExit("manifest must be a list or an object with an items list")

    report = audit_items(items)
    text = json.dumps(report, indent=2, sort_keys=True)
    if args.output:
        args.output.write_text(text + "\n", encoding="utf-8")
    else:
        print(text)

    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
