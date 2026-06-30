#!/usr/bin/env python3
"""Build (feature, delta) JSONL manifests from the SolidStateBusComp corpus for
blended control-regressor training. (License: user-dismissed CC-BY-NC.)

Each SSBC pair's 72-delta target is a COHERENT, FEATURE-DRIVEN full-mastering
label. ALL 72 deltas are deterministic functions of the input's real features
(so they are regressable from the 63-dim feature vector available at runtime):
  - the DYNAMICS slice (8) comes from train.feature_driven_dynamics(frame) —
    mastering-bus heuristics (dense/low-crest -> compress harder; louder ->
    higher threshold; louder/denser -> higher ratio), proxy-aligned.
  - the OTHER 64 deltas (eq/stereo/harmonic/limiter/loudness) come from the
    synthetic mastering teacher (train.SyntheticTeacher) evaluated on that
    input's real features.

Why not the SSBC combo params as dynamics targets: features are extracted once
per song (shared across ~220 combos), so the combo-specific target varies only
WITHIN a song — a within-song-variance decomposition showed 100% of the dyn-MSE
residual was the combo-specific `ratio` dim (across-song ratio-mean std = 0),
i.e. it is non-regressable from song-level features, and `threshold` clamped to
+1.0 (SSBC thresholds saturate the proxy domain). So SSBC's role here is
REAL-SONG FEATURE DISTRIBUTIONS, not combo-param supervision. The per-combo
threshold/ratio/attack/release are retained as provenance metadata only.

Features (63) are extracted from the INPUT audio once per unique song via
features.py (strict C++ runtime parity).

Output JSONL rows: {"feature":[63], "delta":[72], split, sourceId, combo, ...}.
Consumed by train.py --data-mode manifest (ConcatDataset blends with synthetic).
"""
from __future__ import annotations

import argparse
import json
import math
import sys
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from codec import OUTPUT_DELTA_COUNT, DYNAMICS_SLICE, serialize_feature_frame  # noqa: E402
from features import extract_feature_frame  # noqa: E402

DY0 = DYNAMICS_SLICE[0]  # 32
DY_LEN = DYNAMICS_SLICE[1] - DYNAMICS_SLICE[0]  # 8


def read_audio(path: Path) -> tuple[np.ndarray, int]:
    import soundfile as sf
    data, sr = sf.read(str(path), always_2d=True, dtype="float32")
    return data.T.astype(np.float32), int(sr)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--manifest", type=Path, required=True)
    ap.add_argument("--audio-root", type=Path, required=True)
    ap.add_argument("--out-train", type=Path, required=True)
    ap.add_argument("--out-val", type=Path, required=True)
    ap.add_argument("--max-workers", type=int, default=16)
    ap.add_argument("--limit", type=int, help="cap items (debug)")
    ap.add_argument("--selfcheck", action="store_true")
    args = ap.parse_args()

    from train import SyntheticTeacher, feature_driven_dynamics  # lazy: pulls torch
    teacher = SyntheticTeacher(seed=1337)  # teacher_deltas is rng-free; seed irrelevant

    payload = json.loads(args.manifest.read_text(encoding="utf-8"))
    items = payload.get("items", payload if isinstance(payload, list) else [])
    if args.limit:
        items = items[: args.limit]
    if not items:
        raise SystemExit("no items in manifest")

    uniq: list[str] = []
    seen: set[str] = set()
    for it in items:
        rel = it.get("inputPath")
        if rel and rel not in seen:
            seen.add(rel)
            uniq.append(rel)
    print(f"ssbc: {len(items)} items, {len(uniq)} unique inputs -> {args.audio_root}")

    frames: dict[str, object] = {}
    errs: list[str] = []
    lock = threading.Lock()
    done = [0]

    def extract(rel: str):
        path = args.audio_root / rel
        if not path.exists():
            return rel, None, f"missing: {rel}"
        try:
            audio, sr = read_audio(path)
        except Exception as exc:  # noqa: BLE001
            return rel, None, f"read fail {rel}: {exc!r}"
        frame = extract_feature_frame(audio, float(sr),
                                      channel_count=int(audio.shape[0]), block_size=512, frame_index=0)
        return rel, frame, None

    with ThreadPoolExecutor(max_workers=args.max_workers) as ex:
        futs = {ex.submit(extract, rel): rel for rel in uniq}
        for fut in as_completed(futs):
            rel, frame, err = fut.result()
            with lock:
                done[0] += 1
                if err:
                    errs.append(err)
                else:
                    frames[rel] = frame
                if done[0] % 10 == 0 or done[0] == len(uniq):
                    print(f"  features {done[0]}/{len(uniq)} (ok={len(frames)})")
    if len(frames) < max(1, len(uniq) // 2):
        raise SystemExit(f"too many input extractions failed: {len(errs)}/{len(uniq)}; first: {errs[:3]}")

    by_split: dict[str, list[dict]] = {"train": [], "val": [], "test": []}
    miss = 0
    for it in items:
        frame = frames.get(it.get("inputPath"))
        if frame is None:
            miss += 1
            continue
        feature = serialize_feature_frame(frame)
        base = list(teacher.teacher_deltas(frame))            # full synthetic vocab (64+8)
        base[DY0:DY0 + DY_LEN] = feature_driven_dynamics(frame)  # dynamics: feature-driven (combo params are non-regressable; kept as metadata below)
        delta = base
        row = {
            "id": it.get("id"),
            "split": it.get("split", "train"),
            "sourceId": it.get("sourceId"),
            "feature": feature,
            "delta": delta,
            "inputPath": it.get("inputPath"),
            "targetPath": it.get("targetPath"),
            "combo": it.get("combo"),
            "threshold": float(it["threshold"]),
            "attack": float(it.get("attack", 0.0)),
            "release": float(it.get("release", 0.0)),
            "ratio": float(it["ratio"]),
            "dataset": "ssbc",
        }
        by_split.setdefault(row["split"], []).append(row)

    args.out_train.parent.mkdir(parents=True, exist_ok=True)
    with args.out_train.open("w", encoding="utf-8") as fh:
        for r in by_split["train"]:
            fh.write(json.dumps(r) + "\n")
    val_rows = by_split["val"] + by_split["test"]
    with args.out_val.open("w", encoding="utf-8") as fh:
        for r in val_rows:
            fh.write(json.dumps(r) + "\n")
    print(f"wrote {args.out_train}: {len(by_split['train'])} train | "
          f"{args.out_val}: {len(val_rows)} val(+test) | missing-feature items: {miss}")
    if errs:
        print(f"WARN: {len(errs)} input extractions failed; first: {errs[:3]}")

    # ONE self-check on feature_driven_dynamics (the dynamics target source; the 64
    # non-dynamics deltas are SyntheticTeacher.teacher_deltas, tested in train.py).
    if args.selfcheck:
        import torch
        from types import SimpleNamespace
        from diff_dsp import DifferentiableMasteringChain

        def frame(lufs: float = -16.0, lra: float = 6.0, bands=None):
            return SimpleNamespace(integrated_lufs=lufs, loudness_range=lra,
                                   spectral_bands=tuple(bands) if bands is not None else (0.5,) * 32)

        failures: list[str] = []
        print("=== feature_driven_dynamics: 3-band contract (bounded, d6/d7=0, C++ decode roundtrip) ===")
        for lufs in (-24.0, -14.0, -6.0):
            fd = feature_driven_dynamics(frame(lufs=lufs))
            in_range = all(-1.0 <= v <= 1.0 for v in fd)
            unused_zero = fd[6] == 0.0 and fd[7] == 0.0
            dec = [(-20.0 + 8.0 * fd[2 * b], 3.5 + 2.5 * fd[2 * b + 1]) for b in range(3)]
            print(f"  lufs={lufs:<5} -> bands(thr,rat)={[(round(t,1), round(r,2)) for t, r in dec]} "
                  f"unused0:{unused_zero} in_range:{in_range}")
            if not (in_range and unused_zero and all(-40 <= t <= -6 and 1 <= r <= 6 for t, r in dec)):
                failures.append(f"bounded/decode lufs={lufs}: {dec}")
        # louder -> more compression in EVERY band (lower threshold, higher ratio)
        quiet = feature_driven_dynamics(frame(lufs=-24.0))
        loud = feature_driven_dynamics(frame(lufs=-6.0))
        for b in range(3):
            if loud[2 * b] >= quiet[2 * b]:     # v_thr must DECREASE for louder (lower threshold)
                failures.append(f"band{b} threshold not lower for louder: q={quiet[2*b]:+.3f} l={loud[2*b]:+.3f}")
            if loud[2 * b + 1] <= quiet[2 * b + 1]:  # v_rat must INCREASE for louder
                failures.append(f"band{b} ratio not higher for louder: q={quiet[2*b+1]:+.3f} l={loud[2*b+1]:+.3f}")
        # V1 emits UNIFORM threshold/ratio across the 3 bands (coherent symmetric
        # bus compression); per-band differentiation is a V2 follow-up.
        fd = feature_driven_dynamics(frame(lufs=-14.0))
        if not (fd[0] == fd[2] == fd[4] and fd[1] == fd[3] == fd[5]):
            failures.append(f"V1 not uniform across bands: {[round(v, 3) for v in fd[:6]]}")
        # near-silent input abstains to midpoint zeros (lufs-only gate)
        silent = feature_driven_dynamics(frame(lufs=-60.0, bands=[0.0] * 32))
        if silent != [0.0] * 8:
            failures.append(f"silent input did not abstain to zeros: {silent}")

        # isolated 3-band compressor: a feature-driven dynamics vector must reduce level
        chain = DifferentiableMasteringChain(sample_rate=48000.0).eval()
        rng = np.random.default_rng(0)
        audio = torch.tensor(0.3 * rng.standard_normal((1, 2, 48000 * 2), dtype=np.float32))
        rin = 20.0 * math.log10(max(float(np.sqrt((audio[0].numpy() ** 2).mean())), 1e-9))
        print("=== isolated _apply_compressor reduction (loud frame) ===")
        fd = feature_driven_dynamics(frame(lufs=-8.0))
        dyn = torch.zeros((1, OUTPUT_DELTA_COUNT), dtype=torch.float32)
        dyn[0, DY0:DY0 + DY_LEN] = torch.tensor(fd)
        with torch.no_grad():
            out = chain._apply_compressor(audio, dyn)
        red = 20.0 * math.log10(max(float(np.sqrt((out[0].numpy() ** 2).mean())), 1e-9)) - rin
        print(f"  bands(thr,rat)={[(round(-20+8*fd[2*b],1), round(3.5+2.5*fd[2*b+1],2)) for b in range(3)]}: reduction={red:+.2f}dB")
        if red > -0.1:
            failures.append(f"compressor not reducing: {red:.2f}dB")

        if failures:
            print("SELF-CHECK FAIL: " + "; ".join(failures))
            return 2
        print("SELF-CHECK PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
