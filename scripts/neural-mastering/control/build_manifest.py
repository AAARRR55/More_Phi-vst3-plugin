#!/usr/bin/env python3
"""Build a training manifest for the More-Phi control regressor from a corpus.

Reads a directory of audio files, computes the 63-float feature frame per
segment (features.py), derives a 72-float control-delta label per segment
(labels.py), and writes TWO coordinated artifacts:

  train.jsonl / val.jsonl   — JSONL with inline feature/delta arrays
                              (what control/train.py --data-mode manifest reads)
  manifest.json             — World-A {items:[...]} shape
                              (what audit_dataset.py reads for G10 governance)

Both are joined by a stable per-segment `id` and grouped by `sourceId` so the
auditor's split-leakage check passes (split is assigned ONCE per source file,
not per segment).

GOVERNANCE: referenceQuality is honestly "synthetic" because labels come from
the algorithmic teacher (labels.py), not human-reviewed masters. This means G10
will NOT pass and the model is correctly gated to fallback-only — that is the
intended posture for a prototype. See specs/003-neural-mastering-roadmap.

Example:
  python build_manifest.py \\
    --source-dir data/raw \\
    --out-dir data/manifest \\
    --sample-rate 48000 --segment-seconds 10 \\
    --train-ratio 0.8 --val-ratio 0.1 --seed 1337 \\
    --license-status approved --corpus-name fma-small
"""

from __future__ import annotations

import argparse
import json
import math
import random
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

from codec import (  # noqa: E402
    INPUT_FEATURE_COUNT,
    OUTPUT_DELTA_COUNT,
    control_deltas_to_vector,
    serialize_feature_frame,
)
from features import extract_feature_frame  # noqa: E402
from labels import assert_label_semantics, synthesize_deltas  # noqa: E402

AUDIO_EXTENSIONS = {".wav", ".flac", ".aif", ".aiff", ".ogg", ".mp3"}


def _write_zero_label_manifest_from_existing(source_manifest_dir: Path, out_dir: Path, args: argparse.Namespace) -> bool:
    """Reuse extracted features from an existing manifest and replace labels with zero deltas."""
    train_src = source_manifest_dir / "train.jsonl"
    val_src = source_manifest_dir / "val.jsonl"
    if not train_src.exists() or not val_src.exists():
        return False

    out_dir.mkdir(parents=True, exist_ok=True)
    items: list[dict] = []
    for split, src_path in (("train", train_src), ("val", val_src)):
        out_lines: list[str] = []
        with src_path.open("r", encoding="utf-8") as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                record = json.loads(line)
                feature_vec = record.get("feature")
                if not isinstance(feature_vec, list) or len(feature_vec) != INPUT_FEATURE_COUNT:
                    continue
                record["delta"] = [0.0] * OUTPUT_DELTA_COUNT
                record["teacher"] = "zero-label-restraint"
                out_lines.append(json.dumps(record, sort_keys=True))
                items.append({
                    "id": record.get("id"),
                    "sourceId": record.get("sourceId"),
                    "split": split,
                    "provenanceComplete": True,
                    "licenseStatus": args.license_status,
                    "referenceQuality": "synthetic",
                    "unsupportedMaterial": False,
                    "sampleRate": record.get("sampleRate", args.sample_rate),
                    "segmentSeconds": args.segment_seconds,
                    "startSample": record.get("startSample", 0),
                    "sourcePath": record.get("sourcePath", ""),
                    "pairingMode": "zero-label-restraint",
                })
        (out_dir / f"{split}.jsonl").write_text("\n".join(out_lines) + ("\n" if out_lines else ""), encoding="utf-8")

    manifest = {
        "schemaVersion": 1,
        "createdBy": "control/build_manifest.py --zero-labels reuse",
        "corpusName": args.corpus_name,
        "sampleRate": args.sample_rate,
        "segmentSeconds": args.segment_seconds,
        "featureExtractor": "reused from existing manifest",
        "labelSource": "all-zero deltas for already-good/restraint supervision",
        "referenceQuality": "synthetic",
        "lufsDependency": "in-house BS.1770-4 (features.compute_loudness)",
        "items": items,
    }
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(
        f"\nDONE: reused {source_manifest_dir} features with zero labels "
        f"({len(items)} total) -> {out_dir}"
    )
    return True


def load_audio(path: Path, target_sr: int) -> tuple[np.ndarray, int, int]:
    """Load + resample to target_sr, return (audio[nch,samples], native_channels, target_sr).

    Uses soundfile for WAV/FLAC/OGG and falls back to librosa for mp3/aiff.
    """
    ext = path.suffix.lower()
    if ext in {".mp3", ".aif", ".aiff"}:
        import librosa
        audio, sr = librosa.load(str(path), sr=target_sr, mono=False)
        native_ch = 1 if audio.ndim == 1 else audio.shape[0]
        if audio.ndim == 1:
            audio = audio[np.newaxis, :]
        return audio.astype(np.float32), native_ch, sr
    import soundfile as sf
    data, sr = sf.read(str(path), always_2d=True)
    native_ch = data.shape[1]
    audio = data.T.astype(np.float32)  # [nch, samples]
    if sr != target_sr:
        import librosa
        audio = np.stack([librosa.resample(audio[c], orig_sr=sr, target_sr=target_sr) for c in range(native_ch)])
        sr = target_sr
    return audio, native_ch, sr


def assign_split(rng: random.Random, train_ratio: float, val_ratio: float) -> str:
    """Mirrors generate_mastering_dataset.py's ratio+RNG split assignment."""
    r = rng.random()
    if r < train_ratio:
        return "train"
    if r < train_ratio + val_ratio:
        return "val"
    return "test"


def segment_audio(audio: np.ndarray, fs: int, segment_samples: int):
    """Yield (start_sample, segment[nch, segment_samples]) over non-overlapping windows.

    Shorter tails are right-padded with zeros so every segment is the canonical
    length (matches the dataset generator's pad policy). A segment that is
    entirely silence is skipped (no useful features).
    """
    nch, n = audio.shape
    start = 0
    while start < n:
        seg = audio[:, start:start + segment_samples]
        if seg.shape[1] < segment_samples:
            seg = np.pad(seg, ((0, 0), (0, segment_samples - seg.shape[1])))
        if not np.any(np.abs(seg) > 1e-9):
            start += segment_samples
            continue
        yield start, seg.astype(np.float32)
        start += segment_samples


def build(args: argparse.Namespace) -> int:
    source_dir = Path(args.source_dir)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    if args.zero_labels and out_dir.name.endswith("_restraint"):
        sibling_manifest = out_dir.with_name(out_dir.name.removesuffix("_restraint"))
        if _write_zero_label_manifest_from_existing(sibling_manifest, out_dir, args):
            return 0

    audio_files = sorted(
        p for p in source_dir.rglob("*") if p.is_file() and p.suffix.lower() in AUDIO_EXTENSIONS
    )
    if not audio_files:
        print(f"ERROR: no audio files under {source_dir}", file=sys.stderr)
        return 2

    segment_samples = int(round(args.segment_seconds * args.sample_rate))
    split_rng = random.Random(args.seed + 17)  # matches generator's seed offset convention

    # Split assigned ONCE per sourceId (leakage-safe).
    source_splits: dict[str, str] = {}

    train_lines: list[str] = []
    val_lines: list[str] = []
    items: list[dict] = []

    seg_index = 0
    for file_idx, audio_path in enumerate(audio_files):
        source_id = audio_path.stem
        try:
            audio, native_ch, sr = load_audio(audio_path, args.sample_rate)
        except Exception as exc:  # noqa: BLE001
            print(f"  skip {audio_path.name}: load failed ({exc})", file=sys.stderr)
            continue
        channel_count = min(2, native_ch)

        if source_id not in source_splits:
            source_splits[source_id] = assign_split(split_rng, args.train_ratio, args.val_ratio)
        split = source_splits[source_id]
        if split == "test":
            # Hold out entirely; not written to train/val JSONL (test split is for later eval).
            continue

        label_rng = random.Random(args.seed + seg_index)
        n_seg_for_file = 0
        for start, seg in segment_audio(audio, args.sample_rate, segment_samples):
            frame = extract_feature_frame(
                seg, args.sample_rate, channel_count=channel_count,
                block_size=512, frame_index=start,
            )
            feature_vec = serialize_feature_frame(frame)
            assert len(feature_vec) == INPUT_FEATURE_COUNT

            if args.zero_labels:
                # Fix 1 (null-pair / restraint): already-good audio -> all-zero deltas
                # ("do nothing"). Builds a restraint corpus (e.g. AAM human mixes) so the
                # model learns identity, not only correction. Pair with L1 reg (train.py).
                delta_vec = [0.0] * OUTPUT_DELTA_COUNT
            else:
                deltas = synthesize_deltas(frame, label_rng)
                assert_label_semantics(deltas)  # never write a label that violates the DSP mapping
                delta_vec = control_deltas_to_vector(deltas)
                assert len(delta_vec) == OUTPUT_DELTA_COUNT

            seg_id = f"{source_id}_{file_idx:04d}_{n_seg_for_file:04d}"
            record = {
                "id": seg_id,
                "sourceId": source_id,
                "split": split,
                "feature": [float(v) for v in feature_vec],
                "delta": [float(v) for v in delta_vec],
                # Metadata for traceability (ignored by ManifestDataset, used by the audit view)
                "sampleRate": args.sample_rate,
                "startSample": start,
                "sourcePath": str(audio_path),
            }
            line = json.dumps(record, sort_keys=True)
            (train_lines if split == "train" else val_lines).append(line)

            items.append({
                "id": seg_id,
                "sourceId": source_id,
                "split": split,
                # Governance fields for audit_dataset.py (G10).
                # referenceQuality is honestly "synthetic" -> G10 stays red by design.
                "provenanceComplete": True,
                "licenseStatus": args.license_status,
                "referenceQuality": "synthetic",
                "unsupportedMaterial": False,
                "sampleRate": args.sample_rate,
                "segmentSeconds": args.segment_seconds,
                "startSample": start,
                "sourcePath": str(audio_path),
                "pairingMode": "synthesize-mastered",
            })

            seg_index += 1
            n_seg_for_file += 1

        print(f"  {audio_path.name}: {n_seg_for_file} segments -> {split}")

    train_path = out_dir / "train.jsonl"
    val_path = out_dir / "val.jsonl"
    train_path.write_text("\n".join(train_lines) + ("\n" if train_lines else ""), encoding="utf-8")
    val_path.write_text("\n".join(val_lines) + ("\n" if val_lines else ""), encoding="utf-8")

    manifest = {
        "schemaVersion": 1,
        "createdBy": "control/build_manifest.py",
        "corpusName": args.corpus_name,
        "sampleRate": args.sample_rate,
        "segmentSeconds": args.segment_seconds,
        "featureExtractor": "features.py (parity with C++ analyzers)",
        "labelSource": "labels.py synthesize_deltas (parity with AutoMasteringEngine::applyValidatedPlan)",
        "referenceQuality": "synthetic",
        "lufsDependency": "in-house BS.1770-4 (features.compute_loudness)",
        "items": items,
    }
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    print(
        f"\nDONE: {len(train_lines)} train, {len(val_lines)} val segments "
        f"({len(items)} total in manifest.json)\n"
        f"  train: {train_path}\n  val:   {val_path}\n  audit: {out_dir / 'manifest.json'}"
    )
    return 0


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source-dir", required=True)
    p.add_argument("--out-dir", required=True)
    p.add_argument("--sample-rate", type=int, default=48000)
    p.add_argument("--segment-seconds", type=float, default=10.0)
    p.add_argument("--train-ratio", type=float, default=0.8)
    p.add_argument("--val-ratio", type=float, default=0.1)
    p.add_argument("--seed", type=int, default=1337)
    p.add_argument("--license-status", default="approved",
                   help="Provenance licenseStatus value written to manifest.json (audit gate G10).")
    p.add_argument("--corpus-name", default="unnamed")
    p.add_argument("--zero-labels", action="store_true",
                   help="Fix 1 (restraint): emit all-zero deltas (null-pair corpus for already-good audio).")
    return p.parse_args()


if __name__ == "__main__":
    raise SystemExit(build(parse_args()))
