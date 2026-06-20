#!/usr/bin/env python3
"""Empirically characterize a trained Model A ONNX: what does it ACTUALLY output
across the feature space, on edge cases, and vs the T1 teacher it learned from?

Builds FeatureFrame probes (one axis swept at a time from a neutral baseline),
runs the model + labels.py (T1), and prints a structured summary:
per-axis response, restraint behavior, edge-case robustness, T1 fidelity.
This answers "what can this model do right now" with evidence, not assertion.

Usage: python characterize_model.py [path/to/model.onnx]
"""
from __future__ import annotations

import dataclasses
import random
import sys
from pathlib import Path

import numpy as np
import onnxruntime as ort

sys.path.insert(0, str(Path(__file__).parent))
from codec import (  # noqa: E402
    FeatureFrame, OUTPUT_DELTA_COUNT, serialize_feature_frame, vector_to_control_deltas,
)
from labels import synthesize_deltas  # noqa: E402


def baseline() -> FeatureFrame:
    """A neutral 'already-decent mix' frame (balanced loudness/tilt/width)."""
    sb = tuple(-22.0 - 0.4 * i for i in range(32))  # gentle descending spectral contour (dB)
    return FeatureFrame(
        integrated_lufs=-14.0, short_term_lufs=-14.0, momentary_lufs=-14.0, loudness_range=7.0,
        true_peak_dbtp=-1.0, crest_factor_db=12.0, spectral_tilt=0.0, mono_fold_down_delta_db=0.0,
        transient_density=0.5, harmonic_risk=0.1, source_quality_score=1.0,
        spectral_bands=sb, stereo_correlation=(0.9, 0.85, 0.8, 0.75, 0, 0, 0, 0),
        mid_side_ratio=(0.1, 0.15, 0.2, 0.25, 0, 0, 0, 0),
        sample_rate=48000.0, channel_count=2, block_size=512, frame_index=0,
    )


def run_model(sess: ort.InferenceSession, frame: FeatureFrame) -> np.ndarray:
    x = np.asarray(serialize_feature_frame(frame), dtype=np.float32).reshape(1, 63)
    return sess.run(None, {"input": x})[0][0]


def line(tag: str, deltas) -> None:
    d = deltas if hasattr(deltas, "eq") else vector_to_control_deltas(deltas.tolist())
    eq = list(d.eq)
    n = max(abs(v) for v in (list(d.eq) + list(d.dynamics) + list(d.stereo) + list(d.harmonic) + list(d.limiter) + list(d.loudness)))
    print(f"  {tag:30s} eq[min/max]={min(eq):+.3f}/{max(eq):+.3f} loud={d.loudness[0]:+.3f} "
          f"dyn={d.dynamics[0]:+.3f} stereo={d.stereo[0]:+.3f} lim={d.limiter[0]:+.3f} harm={d.harmonic[0]:+.3f} |maxΔ|={n:.3f}")


def main() -> int:
    model = sys.argv[1] if len(sys.argv) > 1 else "runs/blackwell-fma/model_fma.onnx"
    sess = ort.InferenceSession(model)
    b = baseline()
    rng = random.Random(1)

    print(f"=== MODEL: {model} ===")
    print("\n[1] NEUTRAL baseline (already-decent mix) — restraint test")
    bm = run_model(sess, b)
    line("model", bm)
    line("T1 teacher", synthesize_deltas(b, rng))

    print("\n[2] LOUDNESS sweep (integrated_lufs) — does loudness/dynamics delta respond?")
    for lufs in (-28, -22, -18, -14, -10, -6):
        f = dataclasses.replace(b, integrated_lufs=lufs)
        line(f"lufs={lufs:>4}", run_model(sess, f))

    print("\n[3] SPECTRAL TILT sweep — does EQ respond correctively?")
    for tilt in (-6, -3, 0, 3, 6):
        f = dataclasses.replace(b, spectral_tilt=tilt)
        line(f"tilt={tilt:>+3}", run_model(sess, f))

    print("\n[4] STEREO WIDTH sweep (stereo_correlation[0]) — does stereo delta respond?")
    for corr in (1.0, 0.5, 0.0):
        sc = list(b.stereo_correlation); sc[0] = corr
        f = dataclasses.replace(b, stereo_correlation=tuple(sc))
        line(f"corr={corr:>3}", run_model(sess, f))

    print("\n[5] EDGE CASES — finite & bounded? (tanh should clamp to [-1,1])")
    cases = {
        "silence (LUFS -inf->0 frame)": baseline(),  # all meters ~0/nominal
        "NaN injected (integrated_lufs)": dataclasses.replace(b, integrated_lufs=float("nan")),
        "Inf injected (true_peak)": dataclasses.replace(b, true_peak_dbtp=float("inf")),
        "huge spectral_tilt (+50)": dataclasses.replace(b, spectral_tilt=50.0),
        "all-zero feature vector": FeatureFrame(),
    }
    for tag, f in cases.items():
        vec = serialize_feature_frame(f)  # codec coerces non-finite -> 0 before the model sees it
        x = np.asarray(vec, dtype=np.float32).reshape(1, 63)
        out = sess.run(None, {"input": x})[0][0]
        finite = bool(np.all(np.isfinite(out)))
        in_range = bool(out.min() >= -1.0001 and out.max() <= 1.0001)
        print(f"  {tag:34s} finite={finite} in[-1,1]={in_range} min={out.min():+.3f} max={out.max():+.3f}")

    print("\n[6] T1 FIDELITY — model vs teacher across 200 random frames (MSE + corr)")
    diffs, corrs, model_max, t1_max = [], [], [], []
    for _ in range(200):
        r = random.Random(rng.random())
        fr = FeatureFrame(
            integrated_lufs=r.uniform(-28, -6), short_term_lufs=r.uniform(-28, -6),
            momentary_lufs=r.uniform(-30, -4), loudness_range=r.uniform(2, 14),
            true_peak_dbtp=r.uniform(-3, 0), crest_factor_db=r.uniform(6, 18),
            spectral_tilt=r.uniform(-6, 6), mono_fold_down_delta_db=r.uniform(-3, 3),
            transient_density=r.uniform(0, 1), harmonic_risk=r.uniform(0, 0.5),
            source_quality_score=r.uniform(0.5, 1),
            spectral_bands=tuple(r.uniform(-45, -15) for _ in range(32)),
            stereo_correlation=tuple(r.uniform(-0.3, 1) for _ in range(8)),
            mid_side_ratio=tuple(r.uniform(0, 1) for _ in range(8)),
            sample_rate=r.choice([44100.0, 48000.0]), channel_count=2,
            block_size=r.choice([256, 512, 1024]), frame_index=r.randint(0, 100000),
        )
        m = run_model(sess, fr)
        t = np.asarray(serialize_feature_frame(fr) and [0] * OUTPUT_DELTA_COUNT)  # placeholder
        tv = []
        # build T1 delta vector via codec
        from codec import control_deltas_to_vector
        tv = np.asarray(control_deltas_to_vector(synthesize_deltas(fr, r)), dtype=np.float32)
        mv = np.asarray(m, dtype=np.float32)
        diffs.append(float(np.mean((mv - tv) ** 2)))
        if mv.std() > 1e-6 and tv.std() > 1e-6:
            corrs.append(float(np.corrcoef(mv, tv)[0, 1]))
        model_max.append(float(np.max(np.abs(mv))))
        t1_max.append(float(np.max(np.abs(tv))))
    print(f"  MSE(model,T1) mean={np.mean(diffs):.5f} median={np.median(diffs):.5f}")
    print(f"  corr(model,T1) mean={np.mean(corrs):.3f} (1.0=model reproduces teacher)")
    print(f"  |maxΔ| model mean={np.mean(model_max):.3f} vs T1 mean={np.mean(t1_max):.3f} "
          f"(model<{np.mean(t1_max):.3f} = learned restraint; > = more aggressive)")
    print(f"  restraint: model emits |maxΔ|<0.05 on {sum(1 for m in model_max if m < 0.05)}/200 random frames "
          f"(T1: {sum(1 for t in t1_max if t < 0.05)}/200)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
