#!/usr/bin/env python3
"""Create a planner benchmark contract report for non-audio-callback execution."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    report = {
        "gateIds": ["G04", "G05"],
        "executionDomain": "outside-audio-callback",
        "audioCallbackInference": False,
        "worstCaseLatencyMs": None,
        "cpuPercentOfPerformanceCore": None,
        "status": "not-measured",
    }
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
