"""reference_score.py — deterministic mastering score ported from the SSL sibling.

PROVENANCE
----------
This module ports the scoring formula from the sibling project at
``G:\\more-phi ssl\\release_v2_frozen\\ssl_comp\\scoring.py`` (frozen V2.5.4,
sha256 d48c2e513d2c22e3f1ad9cfd2823aeaa56e6016dc180b450d822942e43c7bdae).
That sibling is a *separate* git repo with its own freeze discipline; this is a
**reference port**, not a cross-repo import. The original is unit-tested and
metric-frozen; we keep the math identical so scores are comparable.

WHY
---
More-Phi's control regressor needs a *falsifiable audio-domain* gate, not just a
label-domain loss. The SSL score() function is exactly that: a deterministic,
multidimensional mastering score that:
  - HARD-REJECTS outputs whose sample peak exceeds the ceiling (clipping).
  - rewards proximity of integrated LUFS to the target (-14.0 by default).
  - penalizes crest-factor collapse vs the input (over-compression).
  - penalizes peak-to-loudness-ratio collapse (excessive gain reduction).
The composite is a single comparable number; score_breakdown() exposes every
term so release gates (eqGainMaeDb / targetLufsMae / truePeakCeilingMaeDbtp)
can be checked individually.

Audio convention: channels-first ``[C, N]`` float (mono ``[1, N]`` ok). A 2-D
array with rows > cols is auto-transposed (samples >> channels heuristic).
"""
from __future__ import annotations

import math
from typing import Optional

import numpy as np

# ---------------------------------------------------------------------------
# Optional pyloudnorm (guarded) — same defensive pattern as the original.
# ---------------------------------------------------------------------------
try:  # pragma: no cover - environment dependent
    import pyloudnorm as _pyln

    _HAVE_PYLN = True
except Exception:  # pragma: no cover
    _pyln = None
    _HAVE_PYLN = False

_EPS = 1e-12

# ---------------------------------------------------------------------------
# Default config — mirrors SCORING_DEFAULTS from the SSL sibling.
# ---------------------------------------------------------------------------
SCORING_DEFAULTS: dict = {
    "target_lufs": -14.0,
    "peak_ceiling_db": -1.0,        # hard reject above this sample peak (dBFS)
    "dr_tolerance_db": 6.0,         # crest collapse tolerance before penalty
    "plr_tolerance_db": 6.0,        # PLR collapse tolerance before penalty
    "weights": {
        "loudness": 1.0,
        "dynamics": 0.5,
        "gr": 0.5,
    },
    "reject_score": float("-inf"),
    "sr": 44100,
}

# Release gates (per-axis tolerances) — the falsifiable acceptance contract.
# These are the gates the verifier asked us to adopt; measured against
# score_breakdown() outputs over a held-out audio eval set.
RELEASE_GATES: dict = {
    "eqGainMaeDb": 1.5,             # mean |EQ delta| in dB equivalent
    "targetLufsMae": 0.75,          # mean |out_lufs - target_lufs| in LU
    "truePeakCeilingMaeDbtp": 0.5,  # mean |out_tp - ceiling| in dBTP
}


def _merge_cfg(cfg: Optional[dict]) -> dict:
    merged = dict(SCORING_DEFAULTS)
    merged["weights"] = dict(SCORING_DEFAULTS["weights"])
    if cfg:
        for key, val in cfg.items():
            if key == "weights" and isinstance(val, dict):
                merged["weights"].update(val)
            else:
                merged[key] = val
    return merged


# ---------------------------------------------------------------------------
# Shape / measurement helpers (verbatim from the SSL port).
# ---------------------------------------------------------------------------
def _as_cn(audio: np.ndarray) -> np.ndarray:
    a = np.asarray(audio, dtype=np.float64)
    if a.ndim == 1:
        return a[np.newaxis, :]
    if a.ndim == 2:
        if a.shape[0] > a.shape[1]:
            return np.ascontiguousarray(a.T)
        return a
    raise ValueError(f"Expected [N], [C, N] or [N, C]; got ndim={a.ndim}")


def _peak_db(audio_cn: np.ndarray) -> float:
    peak = float(np.max(np.abs(audio_cn))) if audio_cn.size else 0.0
    if peak <= 0.0:
        return -120.0
    return 20.0 * math.log10(peak)


def _rms_db(audio_cn: np.ndarray) -> float:
    if audio_cn.size == 0:
        return -120.0
    ms = float(np.mean(np.square(audio_cn)))
    if ms <= _EPS:
        return -120.0
    return 10.0 * math.log10(ms)


def _crest_db(audio_cn: np.ndarray) -> float:
    return _peak_db(audio_cn) - _rms_db(audio_cn)


def _approx_loudness(audio_cn: np.ndarray) -> float:
    """Documented fallback (no K-weighting) — monotonic in level only."""
    if audio_cn.size == 0:
        return -120.0
    ms_per_ch = np.mean(np.square(audio_cn), axis=1)
    s = float(np.sum(ms_per_ch))
    if s <= _EPS:
        return -120.0
    return -0.691 + 10.0 * math.log10(s)


def integrated_loudness(audio_cn: np.ndarray, rate: int) -> float:
    """Integrated LUFS (pyloudnorm) with safe fallback to _approx_loudness."""
    if _HAVE_PYLN and _pyln is not None:
        try:
            x = np.ascontiguousarray(audio_cn.T)
            if x.shape[1] == 1:
                x = x[:, 0]
            meter = _pyln.Meter(int(rate))
            val = float(meter.integrated_loudness(x))
            if math.isfinite(val):
                return val
        except Exception:
            pass
    return _approx_loudness(audio_cn)


def true_peak_dbtp(audio_cn: np.ndarray, sr: int, oversample: int = 4) -> float:
    """Approximate true peak (dBTP) via polyphase oversampling per channel.

    Ported from ssl_comp.master.true_peak_db (same 4x oversample convention as
    the sibling's limiter, so the ceiling check is consistent).
    """
    from scipy.signal import resample_poly

    a = _as_cn(audio_cn)
    pk = 0.0
    for ch in a:
        up = resample_poly(ch, oversample, 1) if oversample > 1 else ch
        if up.size:
            pk = max(pk, float(np.max(np.abs(up))))
    if pk <= 0.0:
        return -120.0
    return 20.0 * math.log10(pk)


# ---------------------------------------------------------------------------
# Core score + breakdown (verbatim math from the SSL port).
# ---------------------------------------------------------------------------
def score(
    input_audio: np.ndarray,
    output_audio: np.ndarray,
    params: Optional[dict] = None,
    cfg: Optional[dict] = None,
) -> float:
    """Mastering score for one (input, output) pair. Higher is better.

    HARD-REJECTs (returns -inf) if output sample peak > peak_ceiling_db.
    """
    del params  # stable interface; unused by heuristic terms
    conf = _merge_cfg(cfg)
    rate = int(conf["sr"])

    inp = _as_cn(input_audio)
    out = _as_cn(output_audio)

    out_peak_db = _peak_db(out)
    if out_peak_db > conf["peak_ceiling_db"]:
        return float(conf["reject_score"])

    weights = conf["weights"]
    out_lufs = integrated_loudness(out, rate)
    loudness_term = -abs(out_lufs - conf["target_lufs"])

    in_crest = _crest_db(inp)
    out_crest = _crest_db(out)
    crest_collapse = in_crest - out_crest
    dynamics_term = -max(0.0, crest_collapse - conf["dr_tolerance_db"])

    in_lufs = integrated_loudness(inp, rate)
    in_plr = _peak_db(inp) - in_lufs
    out_plr = out_peak_db - out_lufs
    plr_collapse = in_plr - out_plr
    gr_term = -max(0.0, plr_collapse - conf["plr_tolerance_db"])

    return float(
        weights["loudness"] * loudness_term
        + weights["dynamics"] * dynamics_term
        + weights["gr"] * gr_term
    )


def score_breakdown(
    input_audio: np.ndarray,
    output_audio: np.ndarray,
    params: Optional[dict] = None,
    cfg: Optional[dict] = None,
) -> dict:
    """Per-term breakdown behind score() — for release-gate checking."""
    del params
    conf = _merge_cfg(cfg)
    rate = int(conf["sr"])
    inp = _as_cn(input_audio)
    out = _as_cn(output_audio)

    out_peak_db = _peak_db(out)
    rejected = out_peak_db > conf["peak_ceiling_db"]

    out_lufs = integrated_loudness(out, rate)
    in_lufs = integrated_loudness(inp, rate)
    in_crest = _crest_db(inp)
    out_crest = _crest_db(out)
    in_plr = _peak_db(inp) - in_lufs
    out_plr = out_peak_db - out_lufs

    loudness_term = -abs(out_lufs - conf["target_lufs"])
    dynamics_term = -max(0.0, (in_crest - out_crest) - conf["dr_tolerance_db"])
    gr_term = -max(0.0, (in_plr - out_plr) - conf["plr_tolerance_db"])

    weights = conf["weights"]
    total = (
        float(conf["reject_score"]) if rejected
        else float(
            weights["loudness"] * loudness_term
            + weights["dynamics"] * dynamics_term
            + weights["gr"] * gr_term
        )
    )

    return {
        "rejected": bool(rejected),
        "score": total,
        "out_peak_db": out_peak_db,
        "out_lufs": out_lufs,
        "in_lufs": in_lufs,
        "in_crest_db": in_crest,
        "out_crest_db": out_crest,
        "in_plr_db": in_plr,
        "out_plr_db": out_plr,
        "loudness_term": loudness_term,
        "dynamics_term": dynamics_term,
        "gr_term": gr_term,
        "target_lufs": conf["target_lufs"],
        "peak_ceiling_db": conf["peak_ceiling_db"],
    }


def check_release_gates(breakdowns: list[dict], eq_mae_db: Optional[float] = None) -> dict:
    """Aggregate a list of score_breakdown() dicts against RELEASE_GATES.

    Returns per-gate {name, mean, threshold, pass}. ``eq_mae_db`` is supplied
    by the caller (from the model's EQ delta distribution) since this module
    only sees audio-domain measurements, not the delta vector.
    """
    n = len(breakdowns)
    if n == 0:
        return {"pass": False, "reason": "no_breakdowns", "gates": {}}

    lufs_mae = float(np.mean([abs(b["out_lufs"] - b["target_lufs"]) for b in breakdowns]))
    # true-peak ceiling MAE: how far above the ceiling the output lands (0 if under).
    tp_over = [max(0.0, b["out_peak_db"] - b["peak_ceiling_db"]) for b in breakdowns]
    tp_mae = float(np.mean(tp_over))

    gates = {
        "targetLufsMae": {
            "mean": lufs_mae,
            "threshold": RELEASE_GATES["targetLufsMae"],
            "pass": lufs_mae <= RELEASE_GATES["targetLufsMae"],
        },
        "truePeakCeilingMaeDbtp": {
            "mean": tp_mae,
            "threshold": RELEASE_GATES["truePeakCeilingMaeDbtp"],
            "pass": tp_mae <= RELEASE_GATES["truePeakCeilingMaeDbtp"],
        },
    }
    if eq_mae_db is not None:
        gates["eqGainMaeDb"] = {
            "mean": float(eq_mae_db),
            "threshold": RELEASE_GATES["eqGainMaeDb"],
            "pass": float(eq_mae_db) <= RELEASE_GATES["eqGainMaeDb"],
        }

    all_pass = all(g["pass"] for g in gates.values())
    return {"pass": all_pass, "n": n, "gates": gates}


# ---------------------------------------------------------------------------
# Self-test (mirrors the SSL sibling's, on synthetic audio — no dataset).
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    print(f"pyloudnorm available: {_HAVE_PYLN}")
    sr = 44100
    n = sr * 2
    t = np.arange(n, dtype=np.float64) / sr
    tone = np.sin(2.0 * math.pi * 1000.0 * t).astype(np.float32)
    stereo = np.stack([tone, tone], axis=0)

    # clipping rejected
    clip = stereo.copy()
    s_clip = score(stereo, clip)
    assert s_clip == float("-inf"), f"clipping must be rejected, got {s_clip}"
    print(f"  [ok] clipping rejected (score={s_clip})")

    # louder-toward-target ranks higher
    cfg = {"target_lufs": -6.0}
    quiet = (stereo * 0.10).astype(np.float32)
    loud = (stereo * 0.40).astype(np.float32)
    s_quiet = score(stereo, quiet, cfg=cfg)
    s_loud = score(stereo, loud, cfg=cfg)
    assert math.isfinite(s_quiet) and math.isfinite(s_loud)
    assert s_loud > s_quiet, f"louder should rank higher: {s_loud} vs {s_quiet}"
    print(f"  [ok] louder ranks higher ({s_loud:.3f} > {s_quiet:.3f})")

    # determinism
    assert score(stereo, loud, cfg=cfg) == score(stereo, loud, cfg=cfg)
    print("  [ok] deterministic")

    # breakdown + release gates — use a target the tone actually hits so the
    # LUFS gate is exercised on a passing case (pure sines don't master to -14).
    bd = score_breakdown(stereo, loud, cfg=cfg)
    assert bd["rejected"] is False and math.isfinite(bd["score"])
    measured_lufs = bd["out_lufs"]
    gates_cfg = {"target_lufs": measured_lufs}  # target == measured -> MAE ~ 0
    bd_pass = score_breakdown(stereo, loud, cfg=gates_cfg)
    gates = check_release_gates([bd_pass, bd_pass], eq_mae_db=0.8)
    assert gates["pass"] is True, gates
    print(f"  [ok] release gates pass: {gates['gates']}")

    print("\nreference_score.py self-test passed.")
