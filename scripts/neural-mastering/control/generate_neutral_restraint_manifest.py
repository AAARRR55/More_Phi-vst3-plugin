#!/usr/bin/env python3
"""Generate synthetic already-good feature frames with all-zero deltas.

This is a targeted restraint corpus for Model A. It does not claim audio
coverage; it pins the "balanced mix, do nothing" region used by
characterize_model.py's neutral gate so correction-heavy datasets do not teach
the model to always push loudness/harmonic controls.
"""

from __future__ import annotations

import argparse
import json
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from codec import FeatureFrame, OUTPUT_DELTA_COUNT, serialize_feature_frame  # noqa: E402


def _frame(rng: random.Random, idx: int) -> FeatureFrame:
    spectral_base = rng.uniform(-23.5, -20.5)
    spectral_slope = rng.uniform(-0.55, -0.25)
    spectral = tuple(
        spectral_base + spectral_slope * i + rng.uniform(-0.45, 0.45)
        for i in range(32)
    )
    corr0 = rng.uniform(0.82, 0.98)
    stereo = (
        corr0,
        max(0.70, corr0 - rng.uniform(0.02, 0.08)),
        max(0.62, corr0 - rng.uniform(0.08, 0.16)),
        max(0.55, corr0 - rng.uniform(0.14, 0.24)),
        0.0,
        0.0,
        0.0,
        0.0,
    )
    midside = (
        rng.uniform(0.06, 0.16),
        rng.uniform(0.10, 0.22),
        rng.uniform(0.14, 0.28),
        rng.uniform(0.18, 0.34),
        0.0,
        0.0,
        0.0,
        0.0,
    )
    lufs = rng.uniform(-15.5, -12.8)
    return FeatureFrame(
        integrated_lufs=lufs,
        short_term_lufs=lufs + rng.uniform(-0.8, 0.8),
        momentary_lufs=lufs + rng.uniform(-1.2, 1.2),
        loudness_range=rng.uniform(5.0, 9.0),
        true_peak_dbtp=rng.uniform(-1.8, -0.8),
        crest_factor_db=rng.uniform(10.5, 13.5),
        spectral_tilt=rng.uniform(-0.6, 0.6),
        mono_fold_down_delta_db=rng.uniform(-0.4, 0.4),
        transient_density=rng.uniform(0.35, 0.65),
        harmonic_risk=rng.uniform(0.0, 0.12),
        source_quality_score=rng.uniform(0.88, 1.0),
        spectral_bands=spectral,
        stereo_correlation=stereo,
        mid_side_ratio=midside,
        sample_rate=48000.0,
        channel_count=2,
        block_size=512,
        frame_index=idx,
    )


def _write(path: Path, count: int, rng: random.Random, split: str) -> None:
    zero = [0.0] * OUTPUT_DELTA_COUNT
    with path.open("w", encoding="utf-8") as handle:
        for idx in range(count):
            frame = _frame(rng, idx)
            record = {
                "id": f"neutral_{split}_{idx:06d}",
                "sourceId": f"neutral_{split}_{idx // 16:06d}",
                "split": split,
                "feature": [float(v) for v in serialize_feature_frame(frame)],
                "delta": zero,
                "teacher": "neutral-restraint-zero",
                "sampleRate": 48000,
                "startSample": idx * 512,
                "sourcePath": "synthetic://neutral-restraint",
            }
            handle.write(json.dumps(record, sort_keys=True) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument("--train-count", type=int, default=60000)
    parser.add_argument("--val-count", type=int, default=6000)
    parser.add_argument("--seed", type=int, default=7331)
    args = parser.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    rng = random.Random(args.seed)
    _write(out_dir / "train.jsonl", args.train_count, rng, "train")
    _write(out_dir / "val.jsonl", args.val_count, rng, "val")
    manifest = {
        "schemaVersion": 1,
        "createdBy": "control/generate_neutral_restraint_manifest.py",
        "corpusName": "neutral-restraint",
        "referenceQuality": "synthetic",
        "labelSource": "all-zero deltas for already-good neutral feature frames",
        "items": [
            {"split": "train", "count": args.train_count},
            {"split": "val", "count": args.val_count},
        ],
    }
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"DONE: neutral restraint manifest -> {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
