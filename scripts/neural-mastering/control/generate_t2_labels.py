#!/usr/bin/env python3
"""Re-label a T1 manifest's segments with T2 (CMA-ES against the real DSP render).

Reads a T1 manifest's JSONL (each row has the 63-feature vector + sourcePath +
startSample), and for each segment: loads the source audio, extracts the segment,
runs labels_t2.recover_deltas_render -> T2 deltas. Keeps the SAME feature vector
(no re-extraction skew), swaps the delta. Parallel: one HeadlessRenderer per worker
process (multiprocessing.Pool initializer). Outputs a T2 manifest.

Usage:
  python generate_t2_labels.py --in-manifest <t1_train.jsonl> --out-manifest <t2_train.jsonl> \\
      --render-lib <libmore_phi_headless_render.so> --max-segments 300 --workers 24
"""
from __future__ import annotations

import argparse
import hashlib
import json
import random
import sys
from multiprocessing import Pool
from pathlib import Path

import numpy as np

_CTRL_DIR = str(Path(__file__).resolve().parent)
_HOST = None  # per-worker HeadlessRenderer


def _init_worker(lib: str, sr: float, normalizer_mode: int) -> None:
    global _HOST
    sys.path.insert(0, _CTRL_DIR)
    from morephi_render import HeadlessRenderer  # noqa: E402
    # normalizer_mode=1 (ON) is REQUIRED for the loudness delta to be a live
    # control during teacher search. In mode 0 the loudness[0] delta is a dead
    # axis (setTargetLUFS on a disabled normalizer), so CMA-ES could only move
    # loudness via EQ-abuse — the root cause of restraint_v5's over-correction.
    _HOST = HeadlessRenderer(lib, sample_rate=sr, block_size=512, normalizer_mode=normalizer_mode)


def _load_segment(source_path: str, start: int, seg_samples: int, sr: int):
    try:
        import soundfile as sf
        data, _ = sf.read(source_path, always_2d=True)
        audio = data.T.astype(np.float32)
    except Exception:
        import librosa
        y, _ = librosa.load(source_path, sr=sr, mono=False)
        audio = y.astype(np.float32) if y.ndim == 2 else y[np.newaxis, :].astype(np.float32)
    seg = audio[:, start:start + seg_samples]
    if seg.shape[0] == 1:
        seg = np.stack([seg[0], seg[0]])
    return seg


def _relabel(task):
    from features import extract_feature_frame  # noqa: E402
    from labels_t2 import recover_deltas_render  # noqa: E402
    from codec import control_deltas_to_vector  # noqa: E402

    row, sr, seg_samples, genre, seed, source_dirs, normalizer_mode = task
    src = row.get("sourcePath")
    start = int(row.get("startSample", 0))
    src_path = Path(src) if src else None
    # build_manifest_parallel writes sourcePath as a temp-shard symlink that is
    # cleaned up post-build -- resolve by basename across the candidate dirs.
    if (not src_path) or (not src_path.exists()):
        if src_path and source_dirs:
            for sd in source_dirs:
                cand = Path(sd) / src_path.name
                if cand.exists():
                    src_path = cand
                    break
    if (not src_path) or (not src_path.exists()):
        return None
    src = str(src_path)
    try:
        seg = _load_segment(src, start, seg_samples, sr)
        if seg.shape[1] < seg_samples // 2:
            return None
        frame = extract_feature_frame(seg, sr, channel_count=2, block_size=512, frame_index=start)
        h = int(hashlib.md5(str(row.get("id", "")).encode()).hexdigest(), 16) % (2**31)
        rng = random.Random(seed + h)
        deltas = recover_deltas_render(frame, seg, rng, genre=genre, render_host=_HOST, max_fevals=150)
        dv = control_deltas_to_vector(deltas)
        return {**row, "delta": [float(v) for v in dv], "teacher": "t2-render-cmaes"}
    except Exception as e:  # noqa: BLE001
        return {"_error": str(e), "id": row.get("id")}


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--in-manifest", required=True)
    p.add_argument("--out-manifest", required=True)
    p.add_argument("--render-lib", required=True)
    p.add_argument("--sr", type=int, default=48000)
    p.add_argument("--segment-seconds", type=float, default=10.0)
    p.add_argument("--max-segments", type=int, default=300)
    p.add_argument("--genre", default="neutral")
    p.add_argument("--workers", type=int, default=24)
    p.add_argument("--seed", type=int, default=1337)
    p.add_argument("--normalizer-mode", type=int, default=1, choices=(0, 1),
                   help="render normalizer mode for teacher search: 1=ON (loudness delta is "
                        "live; REQUIRED for balanced labels), 0=OFF (loudness delta dead -> EQ abuse)")
    p.add_argument("--source-dir", default="",
                   help="comma-separated dirs holding real audio (resolves stale temp-shard sourcePaths by basename)")
    args = p.parse_args()

    seg_samples = int(round(args.segment_seconds * args.sr))
    source_dirs = [s.strip() for s in args.source_dir.split(",") if s.strip()] if args.source_dir else []
    rows = []
    for line in open(args.in_manifest, encoding="utf-8"):
        line = line.strip()
        if line:
            rows.append(json.loads(line))
    rows = rows[: args.max_segments]
    print(f"re-labeling {len(rows)} segments with T2 across {args.workers} workers "
          f"(~{150} fevals/seg, CMA-ES, normalizer_mode={args.normalizer_mode})...")

    tasks = [(r, args.sr, seg_samples, args.genre, args.seed, source_dirs, args.normalizer_mode) for r in rows]
    out_rows, errors = [], 0
    with Pool(args.workers, initializer=_init_worker,
              initargs=(args.render_lib, args.sr, args.normalizer_mode)) as pool:
        for i, result in enumerate(pool.imap_unordered(_relabel, tasks, chunksize=1)):
            if result is None or "_error" in result:
                errors += 1
            else:
                out_rows.append(result)
            if (i + 1) % 25 == 0:
                print(f"  {i + 1}/{len(tasks)} done ({errors} errors)")

    Path(args.out_manifest).parent.mkdir(parents=True, exist_ok=True)
    with open(args.out_manifest, "w", encoding="utf-8") as f:
        for r in out_rows:
            f.write(json.dumps(r, sort_keys=True) + "\n")
    print(f"DONE: {len(out_rows)} T2-labeled segments ({errors} errors) -> {args.out_manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
