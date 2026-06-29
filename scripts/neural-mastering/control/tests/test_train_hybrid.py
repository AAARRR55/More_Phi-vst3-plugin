#!/usr/bin/env python3
from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))


def test_train_hybrid_synthetic_smoke_exports_onnx(tmp_path: Path) -> None:
    pytest.importorskip("torch")
    pytest.importorskip("onnx")
    pytest.importorskip("onnxscript")

    import train_hybrid

    onnx_path = tmp_path / "control_regressor_hybrid.onnx"
    card_path = tmp_path / "control_regressor_hybrid.model-card.json"
    args = train_hybrid.parse_args(
        [
            "--data-mode",
            "synthetic",
            "--epochs",
            "1",
            "--synthetic-train",
            "8",
            "--synthetic-val",
            "4",
            "--batch-size",
            "4",
            "--segment-seconds",
            "0.08",
            "--sample-rate",
            "8000",
            "--device",
            "cpu",
            "--output-dir",
            str(tmp_path),
            "--export-onnx",
            str(onnx_path),
            "--model-card",
            str(card_path),
        ]
    )
    train_hybrid.train(args)

    assert onnx_path.exists()
    assert card_path.exists()
    card = json.loads(card_path.read_text(encoding="utf-8"))
    assert card["trainingMode"] == "hybrid_param_policy"
    assert card["inputFeatureCount"] == 63
    assert card["outputDeltaCount"] == 72
    assert card["audioCallbackInference"] is False


def test_manifest_mode_fails_without_audio_refs(tmp_path: Path) -> None:
    import train_hybrid

    row = {"feature": [0.0] * 63, "delta": [0.0] * 72}
    manifest = tmp_path / "rows.jsonl"
    manifest.write_text(json.dumps(row) + "\n", encoding="utf-8")

    args = train_hybrid.parse_args(
        [
            "--data-mode",
            "manifest",
            "--train-manifest",
            str(manifest),
            "--val-manifest",
            str(manifest),
            "--segment-seconds",
            "0.08",
            "--sample-rate",
            "8000",
        ]
    )
    with pytest.raises(ValueError, match="without an audio reference"):
        train_hybrid.make_datasets(args)
