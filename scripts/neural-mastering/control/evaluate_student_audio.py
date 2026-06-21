#!/usr/bin/env python3
"""Audio-domain evaluator for the trained control regressor.

Converts the model from a label-domain object (MSE-vs-label, |delta| restraint)
into a LISTENABLE, FALSIFIABLE audio-domain object: it renders the model's
proposed 72-delta plan through the REAL more_phi AutoMasteringEngine (via the
headless ctypes harness — the exact source set the plugin compiles), then
scores the OUTPUT AUDIO against target.py on the axes a master is judged by:
LUFS, spectral contour, crest factor, true-peak, limiter GR.

This is the missing measurement the "mastering brain" objective requires.
Label-domain metrics proved restraint + T1-fidelity; this proves (or refutes)
that the plan actually masters real audio toward the target.

Usage (run on the remote where the .so + audio live):
  python evaluate_student_audio.py \
      --model runs/blackwell_restraint_v5/model_blackwell_restraint_v5.onnx \
      --lib ~/more-phi-cpp/build/tools/headless_mastering_render/libmore_phi_headless_render.so \
      --manifest data/manifest_fma_clean/val.jsonl \
      --max-segments 25 --genre neutral

It compares, per segment:
  INPUT  (unmastered)  : feature frame -> model -> plan -> render -> output meters
  ZERO   (do-nothing)  : zero-delta plan -> render -> output meters (the "if we
                         did nothing" baseline the restraint corpus supervises)
and reports how the model's plan moves each axis vs the do-nothing baseline and
vs the target. A plan that over-corrects will overshoot target LUFS / crush
crest; a plan that under-corrects won't move the input toward target.
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

from features import extract_feature_frame  # noqa: E402
from target import build_target  # noqa: E402
from reference_score import score_breakdown, check_release_gates, RELEASE_GATES  # noqa: E402
from labels import eq_to_gain_db  # noqa: E402  EQ delta[-1,1] -> gain dB (AdaptiveEQ kMaxGainDB=12)


def _load_segment(source_path: str, start: int, seg_samples: int, sr: int) -> np.ndarray:
    """Load a [2, seg_samples] stereo segment from source_path at startSample."""
    try:
        import soundfile as sf
        data, _ = sf.read(source_path, always_2d=True)
        audio = data.T.astype(np.float32)
    except Exception:
        import librosa
        y, _ = librosa.load(source_path, sr=sr, mono=False)
        audio = y.astype(np.float32) if y.ndim == 2 else y[np.newaxis, :].astype(np.float32)
    seg = audio[:, start:start + seg_samples]
    if seg.shape[1] < seg_samples // 2:
        return np.zeros((2, seg_samples), dtype=np.float32)  # too short -> skip signal
    if seg.shape[0] == 1:
        seg = np.stack([seg[0], seg[0]])
    return seg.astype(np.float32)


def _to_interleaved(seg: np.ndarray) -> np.ndarray:
    a = np.asarray(seg, dtype=np.float32)
    if a.ndim == 1:
        return np.repeat(a, 2).astype(np.float32)
    L, R = (a[:, 0], a[:, 1]) if a.shape[0] > a.shape[1] else (a[0], a[1])
    out = np.empty(L.shape[0] * 2, dtype=np.float32)
    out[0::2] = L
    out[1::2] = R
    return out


def _render_plan(host, seg_interleaved: np.ndarray, delta72: np.ndarray):
    """Render a plan; return (rendered[2,N] planar, meters, spectral_frame)."""
    rendered, meters = host.render_candidate(seg_interleaved, delta72)
    rendered_planar = np.ascontiguousarray(rendered.T)  # [2, N]
    frame = extract_feature_frame(rendered_planar, host.sample_rate,
                                  channel_count=2, block_size=512, frame_index=0)
    return rendered_planar, meters, frame


def _spec_rmse(spec_db, target_spec_db):
    k = min(len(spec_db), len(target_spec_db))
    return float(np.sqrt(np.mean((np.asarray(spec_db[:k]) - np.asarray(target_spec_db[:k])) ** 2)))


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--model", required=True, help="trained .onnx control regressor")
    p.add_argument("--lib", required=True, help="libmore_phi_headless_render.so")
    p.add_argument("--manifest", required=True, help="JSONL manifest with feature+sourcePath+startSample")
    p.add_argument("--max-segments", type=int, default=25)
    p.add_argument("--segment-seconds", type=float, default=10.0)
    p.add_argument("--sr", type=int, default=48000)
    p.add_argument("--genre", default="neutral")
    p.add_argument("--normalizer-mode", type=int, default=1, choices=(0, 1),
                   help="0=normalizer OFF (loudness delta is a dead control on quiet audio); "
                        "1=normalizer ON, loudness delta drives toward targetLUFS (capped [-12,+6]dB)")
    p.add_argument("--source-dir", default="", help="dir holding real audio (resolve stale temp-shard paths by basename)")
    args = p.parse_args()

    import onnxruntime as ort
    from morephi_render import HeadlessRenderer

    target = build_target(args.genre)
    seg_samples = int(round(args.segment_seconds * args.sr))
    host = HeadlessRenderer(args.lib, sample_rate=args.sr, block_size=512,
                            normalizer_mode=args.normalizer_mode)
    sess = ort.InferenceSession(args.model)

    rows = []
    with open(args.manifest, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    rows = rows[: args.max_segments]
    print(f"=== evaluating {len(rows)} segments | model={args.model} | genre={args.genre} ===")
    print(f"target: lufs={target.lufs} crest={target.crest_db} ceiling={target.ceiling_dbtp}dBTP")
    print()

    # Per-segment accumulators
    keys = ["lufs_model", "lufs_zero", "tp_model", "tp_zero",
            "spec_rmse_model", "spec_rmse_zero", "limiter_gr_model", "crest_model"]
    acc = {k: [] for k in keys}
    source_dirs = [s.strip() for s in args.source_dir.split(",") if s.strip()] if args.source_dir else []
    plan_maxdelta = []
    heard = 0

    # Release-gate evidence (reference_score): one breakdown per rendered model
    # segment, plus the EQ-gain MAE in dB (caller-supplied; from the plan's
    # eq delta group via labels.eq_to_gain_db, matching AdaptiveEQ kMaxGainDB).
    score_cfg = {
        "target_lufs": float(target.lufs),
        "peak_ceiling_db": float(target.ceiling_dbtp),
        "sr": int(args.sr),
    }
    breakdowns_model: list[dict] = []
    eq_gain_abs_db_model: list[float] = []

    for i, row in enumerate(rows):
        sp = row.get("sourcePath", "")
        sp_path = Path(sp) if sp else None
        if (not sp_path) or (not sp_path.exists()):
            if sp_path and source_dirs:
                for sd in source_dirs:
                    cand = Path(sd) / sp_path.name
                    if cand.exists():
                        sp_path = cand
                        break
        if (not sp_path) or (not sp_path.exists()):
            continue
        start = int(row.get("startSample", 0))
        seg = _load_segment(str(sp_path), start, seg_samples, args.sr)
        if not np.any(np.abs(seg) > 1e-6):
            continue
        pcm = _to_interleaved(seg)
        if pcm.shape[0] < 2 * host.chain_latency() * 4:
            continue

        # The model's plan (from the stored feature, matching training exactly)
        feat = np.asarray(row["feature"], dtype=np.float32).reshape(1, -1)
        plan = sess.run(None, {"input": feat})[0][0].astype(np.float32)
        plan_maxdelta.append(float(np.max(np.abs(plan))))

        try:
            r_model, m_model, f_model = _render_plan(host, pcm, plan)
            r_zero, m_zero, f_zero = _render_plan(host, pcm, np.zeros(72, dtype=np.float32))
        except RuntimeError as e:
            print(f"  seg {i}: render failed ({e})")
            continue

        # Release-gate evidence for this segment: score the rendered model output
        # against its input on the SSL-derived reference axes. r_model is [2, N].
        try:
            bd = score_breakdown(seg, r_model, cfg=score_cfg)
            breakdowns_model.append(bd)
            eq_gain_abs_db_model.append(
                float(np.mean(np.abs([eq_to_gain_db(v) for v in plan[0:32]])))
            )
        except Exception as e:  # noqa: BLE001
            print(f"  seg {i}: reference_score breakdown failed ({e})")

        acc["lufs_model"].append(m_model.lufs_integrated)
        acc["lufs_zero"].append(m_zero.lufs_integrated)
        acc["tp_model"].append(m_model.true_peak_dbtp)
        acc["tp_zero"].append(m_zero.true_peak_dbtp)
        acc["spec_rmse_model"].append(_spec_rmse(list(f_model.spectral_bands), target.spec_db))
        acc["spec_rmse_zero"].append(_spec_rmse(list(f_zero.spectral_bands), target.spec_db))
        acc["limiter_gr_model"].append(m_model.limiter_gain_reduction_db)
        # crest of the RENDERED output (both peak and rms from the planar render)
        out_inter = _to_interleaved(r_model)
        peak = float(np.max(np.abs(out_inter)) + 1e-9)
        rms = float(np.sqrt(np.mean(out_inter ** 2)) + 1e-9)
        acc["crest_model"].append(20.0 * math.log10(max(peak / rms, 1e-9)))
        heard += 1

    if heard == 0:
        print("ERROR: 0 segments rendered. Check sourcePath exists / --source-dir.")
        host.close()
        return 2

    def mean(k):
        return float(np.mean(acc[k])) if acc[k] else float("nan")

    print(f"=== AUDIO-DOMAIN RESULTS over {heard} real segments (mean) ===")
    print()
    print(f"  {'axis':<28} {'model plan':>12} {'do-nothing':>12} {'target':>10} {'verdict':>10}")
    print(f"  {'-'*28} {'-'*12} {'-'*12} {'-'*10} {'-'*10}")
    # LUFS: model should move input toward target.lufs
    lm, lz, lt = mean("lufs_model"), mean("lufs_zero"), target.lufs
    print(f"  {'integrated LUFS':<28} {lm:>12.2f} {lz:>12.2f} {lt:>10.2f} "
          f"{'CLOSER' if abs(lm-lt) < abs(lz-lt) else 'FARTHER':>10}")
    # True peak: must stay under ceiling
    tm, tz, tc = mean("tp_model"), mean("tp_zero"), target.ceiling_dbtp
    tp_ok = "OK" if tm <= tc + 0.3 else "OVER"
    print(f"  {'true peak dBTP':<28} {tm:>12.2f} {tz:>12.2f} {tc:>10.2f} {tp_ok:>10}")
    # Spectral contour RMSE: lower = closer to target contour
    sm, sz = mean("spec_rmse_model"), mean("spec_rmse_zero")
    print(f"  {'spectral RMSE vs target (dB)':<28} {sm:>12.2f} {sz:>12.2f} {'(lower)':>10} "
          f"{'CLOSER' if sm < sz else 'FARTHER':>10}")
    # Crest: target crest_db (don't crush dynamics)
    cm = mean("crest_model")
    print(f"  {'crest factor dB (rendered)':<28} {cm:>12.2f} {'':>12} {target.crest_db:>10.2f} "
          f"{'OK' if cm >= target.crest_db - 2 else 'CRUSHED':>10}")
    # Limiter GR: proxy for over-limiting
    gm = mean("limiter_gr_model")
    print(f"  {'limiter gain reduction dB':<28} {gm:>12.2f} {'':>12} {'(low ok)':>10} "
          f"{'OK' if abs(gm) < 4 else 'HEAVY':>10}")
    print()
    print(f"  mean |max delta| of plans = {float(np.mean(plan_maxdelta)):.4f}")
    print(f"  (restraint on these segments: |max|<0.05 on "
          f"{sum(1 for m in plan_maxdelta if m < 0.05)}/{len(plan_maxdelta)})")

    # ── FALSIFIABLE RELEASE GATE (reference_score.check_release_gates) ──────
    # Three per-axis gates with fixed thresholds; ALL must pass for the model
    # to be accepted as a release-ready "mastering brain":
    #   eqGainMaeDb            <= 1.5 dB   (mean |EQ gain| the plan applies)
    #   targetLufsMae          <= 0.75 LU  (mean |out_lufs - target_lufs|)
    #   truePeakCeilingMaeDbtp <= 0.5 dBTP (mean amount output exceeds ceiling)
    print()
    print("=== RELEASE GATES (reference_score.check_release_gates) ===")
    if not breakdowns_model:
        print("  (no breakdowns collected — cannot evaluate gates)")
    else:
        eq_mae_db = float(np.mean(eq_gain_abs_db_model)) if eq_gain_abs_db_model else None
        gates = check_release_gates(breakdowns_model, eq_mae_db=eq_mae_db)
        for name, g in gates["gates"].items():
            flag = "PASS" if g["pass"] else "FAIL"
            print(f"  {name:<26} mean={g['mean']:+.3f}  "
                  f"threshold<={g['threshold']:.2f}  [{flag}]")
        verdict = "ALL GATES PASS" if gates["pass"] else "ONE OR MORE GATES FAIL"
        print(f"  -> {verdict}  (n={gates.get('n', len(breakdowns_model))})")

    host.close()
    print()
    print("=== INTERPRETATION ===")
    closer_lufs = abs(lm - lt) < abs(lz - lt)
    closer_spec = sm < sz
    tp_safe = tm <= tc + 0.3
    if closer_lufs and closer_spec and tp_safe:
        print("  The model's plan masters real audio TOWARD target (LUFS + spectrum closer")
        print("  than do-nothing, true-peak under ceiling). This is a real audio-domain signal")
        print("  that label-domain metrics alone could not show.")
    else:
        print("  The model's plan does NOT improve real audio toward target on at least one")
        print("  axis — investigate before trusting it as a 'mastering brain'.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
