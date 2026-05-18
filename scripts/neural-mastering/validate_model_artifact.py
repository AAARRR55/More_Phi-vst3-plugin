#!/usr/bin/env python3
"""Validate a neural mastering model artifact metadata JSON."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


def validate(metadata: dict[str, Any]) -> dict[str, Any]:
    failures: list[str] = []
    if not metadata.get("modelId"):
        failures.append("modelId is required")
    if metadata.get("schemaVersion") != 1:
        failures.append("schemaVersion must be 1")
    if metadata.get("featureSchemaVersion") != 1:
        failures.append("featureSchemaVersion must be 1")
    if metadata.get("outputSchemaVersion") != 1:
        failures.append("outputSchemaVersion must be 1")
    if metadata.get("audioCallbackInference", False):
        failures.append("audioCallbackInference must be false")
    if not metadata.get("supportedSampleRates"):
        failures.append("supportedSampleRates must not be empty")
    if not metadata.get("supportedLayouts"):
        failures.append("supportedLayouts must not be empty")

    return {
        "valid": not failures,
        "failures": failures,
        "fallbackWhenInvalid": "deterministic-baseline",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("metadata", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    report = validate(json.loads(args.metadata.read_text(encoding="utf-8")))
    text = json.dumps(report, indent=2, sort_keys=True)
    if args.output:
        args.output.write_text(text + "\n", encoding="utf-8")
    else:
        print(text)

    return 0 if report["valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
