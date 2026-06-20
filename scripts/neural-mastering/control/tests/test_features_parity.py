#!/usr/bin/env python3
"""Parity + contract tests for features.py and labels.py.

Known-answer checks on synthetic signals with verifiable expectations, plus
strict-stub-parity assertions (the 4 zeroed scalars + sourceQualityScore=1.0 +
the 8-band zero-pad), and finiteness/range guarantees the ONNX input head and
the safety policy rely on.

Run: python tests/test_features_parity.py
"""

from __future__ import annotations

import math
import random
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from codec import (  # noqa: E402
    INPUT_FEATURE_COUNT,
    OUTPUT_DELTA_COUNT,
    SPECTRAL_BAND_COUNT,
    STEREO_BAND_COUNT,
    serialize_feature_frame,
)
from features import extract_feature_frame  # noqa: E402
from labels import (  # noqa: E402
    assert_label_semantics,
    dynamics_to_threshold_ratio,
    eq_to_gain_db,
    harmonic_to_params,
    limiter_to_ceiling,
    loudness_to_target,
    stereo_to_width,
    synthesize_deltas,
)

SR = 48000.0
N = int(SR * 6.0)  # 6 seconds — enough for short-term (3s) and LRA (>30 blocks)
T = np.arange(N) / SR


def _stereo(l: np.ndarray, r: np.ndarray | None = None) -> np.ndarray:
    r = l if r is None else r
    return np.stack([l.astype(np.float32), r.astype(np.float32)])


# ── Known-answer signals ─────────────────────────────────────────────────────

def test_full_scale_sine_true_peak_near_zero():
    """A full-scale sine's true peak should be near 0 dBTP (sample peak = 0)."""
    sine = np.sin(2 * math.pi * 1000.0 * T).astype(np.float32)
    frame = extract_feature_frame(_stereo(sine), SR, channel_count=2)
    assert -0.5 < frame.true_peak_dbtp < 0.5, f"true peak {frame.true_peak_dbtp} not ~0 dBTP"


def test_pink_noise_spectral_tilt_near_zero():
    """Pink noise has roughly equal energy per octave -> spectralTilt ~= 0 dB/octave.

    The Voss-McCartney generator used here rolls off slightly above ~5 kHz (a
    known limitation of the algorithm), so we allow ±4 dB/octave rather than
    demanding exact flatness. This test guards against gross tilt errors (e.g.
    the analyzer reporting a bright or dull slope on flat-ish input), not
    against the generator's high-frequency droop.
    """
    # Voss-McCartney pink noise approximation.
    rng = np.random.default_rng(42)
    white = rng.standard_normal(N)
    pink = np.zeros(N)
    b0 = b1 = b2 = b3 = b4 = b5 = b6 = 0.0
    for i in range(N):
        w = white[i]
        b0 = 0.99886 * b0 + w * 0.0555179
        b1 = 0.99332 * b1 + w * 0.0750759
        b2 = 0.96900 * b2 + w * 0.1538520
        b3 = 0.86650 * b3 + w * 0.3104856
        b4 = 0.55000 * b4 + w * 0.5329522
        b5 = -0.7616 * b5 - w * 0.0168980
        pink[i] = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362
        b6 = w * 0.115926
    pink = pink / (np.max(np.abs(pink)) + 1e-12) * 0.5
    frame = extract_feature_frame(_stereo(pink.astype(np.float32)), SR, channel_count=2)
    assert abs(frame.spectral_tilt) < 4.0, f"pink tilt {frame.spectral_tilt} not near 0"


def test_silence_lufs_handled_finite():
    """Silence -> LUFS is -inf in the meter, but the frame must coerce to finite 0.0."""
    silence = np.zeros(N, dtype=np.float32)
    frame = extract_feature_frame(_stereo(silence), SR, channel_count=2)
    # The extractor coerces -inf to 0.0 (finiteOrZero parity).
    assert math.isfinite(frame.integrated_lufs)
    assert math.isfinite(frame.short_term_lufs)
    assert math.isfinite(frame.true_peak_dbtp)
    vec = serialize_feature_frame(frame)
    assert all(math.isfinite(v) for v in vec), "non-finite in silence feature vector"


def test_mono_signal_correlation_near_one():
    """Identical L/R -> stereo correlation == 1.0 (perfectly mono)."""
    tone = (0.3 * np.sin(2 * math.pi * 220.0 * T)).astype(np.float32)
    frame = extract_feature_frame(_stereo(tone, tone), SR, channel_count=2)
    # Band 0 (sub) of a pure mono tone should correlate ~1.0
    assert frame.stereo_correlation[0] > 0.95, f"mono corr {frame.stereo_correlation[0]} not ~1.0"


# ── Strict stub parity ───────────────────────────────────────────────────────

def test_stub_fields_match_runtime():
    """The 4 stub scalars are always 0.0 and sourceQualityScore is always 1.0."""
    sine = (0.5 * np.sin(2 * math.pi * 440.0 * T)).astype(np.float32)
    frame = extract_feature_frame(_stereo(sine), SR, channel_count=2)
    assert frame.crest_factor_db == 0.0, "crestFactorDb must be 0.0 (runtime stub)"
    assert frame.mono_fold_down_delta_db == 0.0, "monoFoldDownDeltaDb must be 0.0 (runtime stub)"
    assert frame.transient_density == 0.0, "transientDensity must be 0.0 (runtime stub)"
    assert frame.harmonic_risk == 0.0, "harmonicRisk must be 0.0 (runtime stub)"
    assert frame.source_quality_score == 1.0, "sourceQualityScore must be 1.0 (runtime constant)"


def test_stereo_arrays_eight_bands_padded():
    """stereoCorrelation/midSideRatio are length-8; indices 4-7 are zero (4-band analyzer)."""
    sine = (0.5 * np.sin(2 * math.pi * 440.0 * T)).astype(np.float32)
    frame = extract_feature_frame(_stereo(sine), SR, channel_count=2)
    assert len(frame.stereo_correlation) == STEREO_BAND_COUNT
    assert len(frame.mid_side_ratio) == STEREO_BAND_COUNT
    assert frame.stereo_correlation[4:8] == (0.0, 0.0, 0.0, 0.0), "stereoCorr[4:8] must be zero-pad"
    assert frame.mid_side_ratio[4:8] == (0.0, 0.0, 0.0, 0.0), "midSideRatio[4:8] must be zero-pad"


def test_feature_vector_shape_and_finite():
    """The serialized feature vector is exactly 63 floats, all finite."""
    sine = (0.5 * np.sin(2 * math.pi * 440.0 * T)).astype(np.float32)
    frame = extract_feature_frame(_stereo(sine), SR, channel_count=2)
    vec = serialize_feature_frame(frame)
    assert len(vec) == INPUT_FEATURE_COUNT, f"len {len(vec)} != {INPUT_FEATURE_COUNT}"
    assert all(math.isfinite(v) for v in vec), "non-finite feature value"
    assert len(frame.spectral_bands) == SPECTRAL_BAND_COUNT


# ── Label semantics: the 6 applyValidatedPlan formulas ───────────────────────

def test_eq_mapping_matches_apply_validated_plan():
    """eq[i] -> gainDB = clamp(val*12, ±12), byte-identical to AutoMasteringEngine.cpp:355-358."""
    assert eq_to_gain_db(0.0) == 0.0
    assert eq_to_gain_db(1.0) == 12.0
    assert eq_to_gain_db(-1.0) == -12.0
    assert abs(eq_to_gain_db(0.3) - 3.6) < 1e-9, f"{eq_to_gain_db(0.3)}"  # +3.6 dB
    assert eq_to_gain_db(2.0) == 12.0  # clamped


def test_dynamics_mapping_matches():
    """dynamics -> threshold=clamp(-20+val*8,[-40,-6]), ratio=clamp(2.5+val*1.5,[1,6])."""
    thr, ratio = dynamics_to_threshold_ratio(0.3)
    assert abs(thr - (-17.6)) < 1e-9 and abs(ratio - 2.95) < 1e-9
    thr, ratio = dynamics_to_threshold_ratio(1.0)
    assert thr == -12.0 and ratio == 4.0
    thr, ratio = dynamics_to_threshold_ratio(-1.0)
    assert thr == -28.0 and ratio == 1.0


def test_stereo_mapping_matches():
    assert stereo_to_width(0.0) == 1.0
    assert stereo_to_width(0.3) == 1.3
    assert stereo_to_width(-1.0) == 0.0
    assert stereo_to_width(1.0) == 2.0


def test_harmonic_one_sided():
    enabled, drive, drywet = harmonic_to_params(0.5)
    assert enabled and abs(drive - 12.0) < 1e-9 and abs(drywet - 0.5) < 1e-9
    enabled, drive, drywet = harmonic_to_params(-0.5)  # negative -> no-op
    assert not enabled
    assert drywet == 0.0


def test_limiter_mapping_matches():
    assert limiter_to_ceiling(0.0) == -1.0
    assert abs(limiter_to_ceiling(0.3) - (-0.85)) < 1e-9
    assert limiter_to_ceiling(-1.0) == -1.5
    assert limiter_to_ceiling(1.0) == -0.5


def test_loudness_mapping_matches():
    assert loudness_to_target(0.0) == -14.0
    assert abs(loudness_to_target(0.3) - (-12.2)) < 1e-9
    assert loudness_to_target(1.0) == -8.0
    assert loudness_to_target(-1.0) == -20.0


# ── Label synthesis + dead-slot policy ───────────────────────────────────────

def test_synthesize_deltas_in_range_and_dead_slots_zero():
    frame = extract_feature_frame(
        _stereo((0.5 * np.sin(2 * math.pi * 440.0 * T)).astype(np.float32)), SR, channel_count=2
    )
    deltas = synthesize_deltas(frame, random.Random(0))
    assert_label_semantics(deltas)  # raises on any violation (range, dead slots, one-sided harmonic)
    # Explicit dead-slot re-check (defense in depth):
    assert deltas.dynamics[4:8] == (0.0, 0.0, 0.0, 0.0)
    assert deltas.stereo[4:8] == (0.0, 0.0, 0.0, 0.0)
    assert deltas.harmonic[1:8] == (0.0,) * 7
    assert deltas.limiter[1:8] == (0.0,) * 7
    assert deltas.loudness[1:8] == (0.0,) * 7


def main() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    passed = 0
    failed = 0
    for t in tests:
        try:
            t()
            print(f"PASS  {t.__name__}")
            passed += 1
        except AssertionError as exc:
            print(f"FAIL  {t.__name__}: {exc}")
            failed += 1
        except Exception as exc:  # noqa: BLE001
            print(f"ERROR {t.__name__}: {type(exc).__name__}: {exc}")
            failed += 1
    print(f"\n{passed} passed, {failed} failed, {len(tests)} total")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
