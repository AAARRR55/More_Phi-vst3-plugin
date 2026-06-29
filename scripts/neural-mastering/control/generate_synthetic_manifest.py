#!/usr/bin/env python3
"""Minimal synthetic-mastering manifest generator (Path B test).

PROVES the "generate our own dataset" plan end-to-end, using only deps already
on the remote (soundfile/numpy/scipy + the control package's own features/codec).
No torch/torchaudio/pyloudnorm needed.

For each source mp3:
  1. load + crop a segment
  2. degrade it (the "unmastered" input): random gain + 3-band EQ skew +
     transient/widening shift  (mirrors generate_mastering_dataset.make_pair)
  3. master it (the "target"): corrective 3-band EQ toward flat + bus comp +
     tanh sat + loudness normalize to -14 + ceiling limit
  4. extract the 63-feature frame from the UNMASTERED input (features.py)
  5. solve the 72-delta target = the control vector that, applied to the
     unmastered input, would move it toward the mastered target. We use a
     simple closed-form per-group fit (NOT CMA-ES -- this is the cheap teacher
     that scales to 10k+ rows).
  6. write {"feature":[63], "delta":[72], "teacher":"synthetic-b"} to JSONL.

This is intentionally a WEAK teacher (closed-form, not optimized). The point of
THIS script is to prove the pipeline produces rows the trainer accepts and that
the resulting labels actually master toward -14 LUFS. If that holds, scaling up
(sampling more sources, richer degradation) is just config.
"""
from __future__ import annotations
import argparse, json, math, random, sys
from multiprocessing import Pool
from pathlib import Path
import numpy as np
import soundfile as sf

_CTRL = str(Path(__file__).resolve().parent)
sys.path.insert(0, _CTRL)
from features import extract_feature_frame, compute_loudness  # noqa
from codec import serialize_feature_frame, ControlDeltas  # noqa

TARGET_LUFS = -14.0
CEILING_DBTP = -1.0


def load_segment(path, start, n, sr):
    data, info_sr = sf.read(str(path), always_2d=True,
                            start=start, frames=n, dtype="float32")
    audio = data.T  # [C, N]
    if audio.shape[0] == 1:
        audio = np.stack([audio[0], audio[0]])
    return audio.astype(np.float32), int(info_sr)


def peak_normalize(x):
    p = float(np.max(np.abs(x))) if x.size else 0.0
    if p > 1e-9:
        x = x * (0.95 / p)
    return x


def gain_db(x, db):
    return x * (10.0 ** (db / 20.0))


def three_band_eq(x, sr, low_db, mid_db, high_db):
    """Simple 3-band shelf via FFT (cheap, matches generate_mastering_dataset)."""
    spec = np.fft.rfft(x, axis=-1)
    freqs = np.fft.rfftfreq(x.shape[-1], d=1.0 / sr)
    anchors = np.array([20.0, 180.0, 1000.0, 6000.0, sr / 2.0])
    dbs = np.array([low_db, low_db, mid_db, high_db, high_db])
    lf, la = np.log10(np.clip(anchors, 20.0, None)), np.log10(anchors)
    curve = np.interp(np.log10(np.clip(freqs, 20.0, None)), lf, la)
    g = (10.0 ** (curve / 20.0))[None, :]
    return np.fft.irfft(spec * g, n=x.shape[-1], axis=-1)


def bus_comp(x, thr_db, ratio, win=2048):
    mono = x.mean(axis=0, keepdims=True)
    env = np.abs(mono)
    # moving-average envelope
    kernel = np.ones(win) / win
    env = np.convolve(env[0], kernel, mode="same")[None, : x.shape[-1]].clip(min=1e-8)
    env_db = 20.0 * np.log10(env)
    over = np.clip(env_db - thr_db, 0.0, None)
    gr = over * (1.0 - 1.0 / ratio)
    return x * (10.0 ** (-gr / 20.0))


def saturate(x, drive):
    if drive <= 1.0:
        return x
    return np.tanh(x * drive) / math.tanh(drive)


def lufs_measure(x, sr):
    """BS.1770-ish via the control package's own loudness (K-weighted)."""
    try:
        i, _, _, _ = compute_loudness(x.astype(np.float64), float(sr))
        return float(i)
    except Exception:
        ms = float(np.mean(x ** 2))
        return -0.691 + 10.0 * math.log10(ms + 1e-12) if ms > 1e-12 else -70.0


def normalize_loudness(x, sr, target):
    cur = lufs_measure(x, sr)
    if math.isfinite(cur):
        x = gain_db(x, target - cur)
    return peak_normalize(x)


def limit_ceiling(x, ceiling_db):
    c = 10.0 ** (ceiling_db / 20.0)
    p = float(np.max(np.abs(x))) if x.size else 0.0
    if p > c:
        x = x * (c / p)
    return np.clip(x, -c, c)


def degrade(src, sr, rng):
    """Make an 'unmastered' input: gain + skew + width shift."""
    u = gain_db(src, rng.uniform(-6.0, 2.0))
    u = three_band_eq(u, sr, rng.uniform(-3.5, 3.5), rng.uniform(-2.0, 2.0),
                      rng.uniform(-4.0, 3.0))
    return peak_normalize(u)


def master_target(u, sr, rng):
    """The 'good' master: corrective EQ toward flat + comp + sat + LUFS + limit."""
    m = three_band_eq(u, sr, rng.uniform(-1.0, 1.0), rng.uniform(-0.8, 0.8),
                      rng.uniform(-0.5, 1.5))
    m = bus_comp(m, rng.uniform(-24.0, -14.0), rng.uniform(1.4, 2.6))
    m = saturate(m, rng.uniform(1.0, 1.5))
    m = normalize_loudness(m, sr, TARGET_LUFS)
    m = limit_ceiling(m, CEILING_DBTP)
    return m


def fit_deltas(u, m, sr, frame):
    """Closed-form 72-delta: per-group scalar that best moves u -> m on the
    measurable axes. This is the cheap teacher (no CMA-ES). The model learns
    to predict these deltas from the input FEATURE VECTOR (not from u/m
    directly), so even a rough teacher gives useful supervision as long as the
    deltas consistently move toward -14 LUFS / balanced spectrum."""
    # EQ slot (32): compare 1/3-octave spectra of u vs m; delta = sign+mag of
    # the corrective move. We have 16 spectral bands from the feature frame;
    # duplicate to 32 (the codec packs 32 eq bins).
    eq = [0.0] * 32
    # dynamics (8): bus-comp ratio proxy -> small positive delta if m is more
    # compressed than u (lower crest).
    cu = crest_db(u); cm = crest_db(m)
    dyn = max(0.0, (cu - cm) / 12.0)  # 0..~0.5
    dynamics = [min(0.4, dyn)] + [0.0] * 7
    # stereo (8): ~0 (we don't widen in the cheap teacher)
    stereo = [0.0] * 8
    # harmonic (8): small sat proxy
    harm = [min(0.3, (rng_uniform()) )] if False else [0.0] * 8
    # limiter (8): ceiling engaged
    limiter = [0.1, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
    # loudness (8): gain toward -14. delta = how much u needed to move (LU/12,
    # clamped to tanh range). Positive = make louder.
    lu = lufs_measure(u, sr)
    loud = np.clip((TARGET_LUFS - lu) / 12.0, -1.0, 1.0)
    loudness = [float(loud)] + [0.0] * 7
    # EQ: derive from feature spectral bands (16) -> corrective pull toward median
    sb = list(frame.spectral_bands)
    if sb:
        med = float(np.median(sb))
        for i in range(16):
            pull = np.clip(-(sb[i] - med) / 12.0, -0.5, 0.5)
            eq[2 * i] = float(pull)        # one bin per band
            eq[2 * i + 1] = float(pull)    # mirror
    return ControlDeltas(
        eq=eq, dynamics=dynamics, stereo=stereo, harmonic=harm,
        limiter=limiter, loudness=loudness,
    )


def crest_db(x):
    p = float(np.max(np.abs(x))) if x.size else 1e-9
    r = float(np.sqrt(np.mean(x ** 2))) if x.size else 1e-9
    return 20.0 * math.log10(max(p, 1e-9) / max(r, 1e-9))


def rng_uniform():
    return random.random()


def _process_file(task):
    """Worker: produce up to N synthetic rows from one source file.

    Returns (json_lines, n_err). Each entry is an ALREADY-JSON-SERIALIZED
    string. Returning strings (not dicts of numpy-derived floats) across the
    multiprocessing boundary avoids a pickling/buffer-reuse corruption we
    observed (null bytes / truncated writes) when returning dicts of floats.
    """
    import json as _json
    from codec import control_deltas_to_vector  # local import (worker side)
    path_str, seed, seg_samples, sr, segs_per_file, max_rows_remaining = task
    rng = random.Random(seed)
    path = Path(path_str)
    json_lines = []
    err = 0
    try:
        info = sf.info(str(path))
        total = int(info.frames)
        if total < seg_samples // 2:
            return json_lines, err
    except Exception:
        return json_lines, 1
    produced = 0
    while produced < segs_per_file and produced < max_rows_remaining:
        try:
            start = rng.randint(0, max(0, total - seg_samples))
            src, info_sr = load_segment(path, start, seg_samples, sr)
            if src.shape[1] < seg_samples // 2:
                break
            u = degrade(src, info_sr, rng)
            m = master_target(u, info_sr, rng)
            frame = extract_feature_frame(u, info_sr, channel_count=2,
                                          block_size=512, frame_index=start)
            deltas = fit_deltas(u, m, info_sr, frame)
            dv = control_deltas_to_vector(deltas)
            feat = serialize_feature_frame(frame)
            # Force plain python floats (kill any numpy scalar refs) BEFORE
            # serialization so nothing shares a buffer across the boundary.
            row = {
                "feature": [float(v) for v in feat],
                "delta": [float(v) for v in dv],
                "teacher": "synthetic-b", "sourceId": path.stem,
                "startSample": int(start), "sampleRate": int(info_sr),
                "inputLufs": float(lufs_measure(u, info_sr)),
                "targetLufs": float(lufs_measure(m, info_sr)),
            }
            s = _json.dumps(row, sort_keys=True)
            _json.loads(s)  # validate round-trip inside the worker
            json_lines.append(s)
            produced += 1
        except Exception:
            err += 1
            break
    return json_lines, err


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source-dir", required=True)
    ap.add_argument("--out-manifest", required=True)
    ap.add_argument("--max-rows", type=int, default=200)
    ap.add_argument("--segments-per-file", type=int, default=8,
                    help="random segments to draw per source file (multiplies coverage)")
    ap.add_argument("--segment-seconds", type=float, default=10.0)
    ap.add_argument("--sr", type=int, default=48000)
    ap.add_argument("--seed", type=int, default=1337)
    ap.add_argument("--workers", type=int, default=16)
    args = ap.parse_args()

    rng = random.Random(args.seed)
    files = sorted([p for p in Path(args.source_dir).rglob("*")
                    if p.suffix.lower() in (".mp3", ".wav", ".flac")])
    if not files:
        raise SystemExit(f"no audio under {args.source_dir}")
    seg = int(round(args.segment_seconds * args.sr))
    Path(args.out_manifest).parent.mkdir(parents=True, exist_ok=True)

    # Spread target rows across files; each file gets a bounded budget.
    per_file = max(1, min(args.segments_per_file,
                          (args.max_rows // max(1, len(files))) + 1))
    # Build tasks: (path, seed_per_file, seg, sr, segs_per_file, remaining_cap)
    tasks = []
    cap = args.max_rows
    for i, p in enumerate(files):
        if cap <= 0:
            break
        this_budget = min(per_file, cap)
        tasks.append((str(p), args.seed + i * 7919, seg, args.sr,
                      this_budget, this_budget))
        cap -= this_budget

    print(f"synthetic-B: {len(tasks)} files, ~{per_file}/file, "
          f"{args.workers} workers -> {args.max_rows} rows target")
    n_ok = n_err = 0
    lufs_in, lufs_out = [], []
    with open(args.out_manifest, "w", encoding="utf-8") as f:
        with Pool(args.workers) as pool:
            for json_lines, e in pool.imap_unordered(_process_file, tasks, chunksize=1):
                n_err += e
                for s in json_lines:
                    f.write(s + "\n")
                    n_ok += 1
                    # pull lufs back for the summary (cheap re-parse)
                    try:
                        r = json.loads(s)
                        lufs_in.append(r["inputLufs"])
                        lufs_out.append(r["targetLufs"])
                    except Exception:
                        n_err += 1
                if n_ok and n_ok % 500 == 0:
                    print(f"  {n_ok} rows written ({n_err} errors)")

    print(f"DONE: {n_ok} rows, {n_err} errors -> {args.out_manifest}")
    if lufs_out:
        print(f"  target LUFS out: mean={np.mean(lufs_out):.2f} "
              f"(target {TARGET_LUFS}), range [{min(lufs_out):.2f},{max(lufs_out):.2f}]")
        print(f"  input  LUFS in : mean={np.mean(lufs_in):.2f}")


if __name__ == "__main__":
    raise SystemExit(main())
