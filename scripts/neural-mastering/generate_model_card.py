#!/usr/bin/env python3
"""Generate a neural mastering model-card skeleton and G01-G10 readiness summary."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


GATES = [f"G{i:02d}" for i in range(1, 11)]


def default_gate_summary() -> list[dict[str, str]]:
    return [
        {
            "gateId": gate,
            "status": "not-measured",
            "evidenceLevel": "planning",
            "summary": "No measured release evidence has been attached.",
        }
        for gate in GATES
    ]


def release_readiness(gates: list[dict[str, Any]]) -> dict[str, Any]:
    by_id = {str(gate.get("gateId")): gate for gate in gates}
    missing_or_failed = [
        gate_id
        for gate_id in GATES
        if by_id.get(gate_id, {}).get("status") != "pass"
    ]
    return {
        "releaseReady": not missing_or_failed,
        "blockedGates": missing_or_failed,
        "decision": "release-approved" if not missing_or_failed else "fallback-only",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--metadata", type=Path, help="Optional metadata JSON seed")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    seed: dict[str, Any] = {}
    if args.metadata:
        seed = json.loads(args.metadata.read_text(encoding="utf-8"))

    gates = seed.get("gateSummary") or default_gate_summary()
    card = {
        "modelId": seed.get("modelId", "disabled-poc-neural-mastering"),
        "schemaVersion": seed.get("schemaVersion", 1),
        "featureSchemaVersion": seed.get("featureSchemaVersion", 1),
        "outputSchemaVersion": seed.get("outputSchemaVersion", 1),
        "evidenceLevel": seed.get("evidenceLevel", "planning"),
        "runtimeModes": seed.get("runtimeModes", ["offline", "preview"]),
        "audioCallbackInference": False,
        "supportedSampleRates": seed.get("supportedSampleRates", [44100.0, 48000.0]),
        "supportedLayouts": seed.get("supportedLayouts", ["mono", "stereo"]),
        "licenseStatus": seed.get("licenseStatus", "unresolved"),
        "trainingDataSummary": seed.get(
            "trainingDataSummary",
            {
                "provenanceComplete": False,
                "splitLeakageChecked": False,
                "referenceQualityReviewed": False,
            },
        ),
        "gateSummary": gates,
        "fallbackBehavior": seed.get(
            "fallbackBehavior",
            {
                "missingModel": "deterministic-baseline",
                "invalidOutput": "reject",
                "lowConfidence": "review-only",
                "overload": "last-safe-hold",
            },
        ),
        "releaseReadiness": release_readiness(gates),
    }

    args.output.write_text(json.dumps(card, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
