"""Control-delta label synthesizer for the More-Phi control regressor.

PARITY SOURCE OF TRUTH: AutoMasteringEngine::applyValidatedPlan
(src/Core/AutoMasteringEngine.cpp:347-409). The 6 mapping formulas below are
copied verbatim from that function so a synthesized delta means EXACTLY what
the shipping DSP will do when the runtime projects it.

DEAD-SLOT POLICY: 40 of the 72 control slots currently drive nothing in the DSP
  - dynamics[4-7], stereo[4-7]   (only 4 bands/regions wired)
  - harmonic[1-7], limiter[1-7], loudness[1-7]  (only index 0 wired)
These are zeroed in every synthesized label so the model is never supervised on
unobservable controls. Documented, not hidden.

DELTA vs ABSOLUTE: the model's raw output is a "delta", but applyValidatedPlan
consumes the projected ABSOLUTE target, and from a cold start the first plan's
target equals the (clamped) delta (buildPlanCandidate sets targets == deltas).
So a synthesized target in [-1,1] is, operationally, the label. We do not
simulate the per-plan max-delta trajectory — that's a runtime safety property,
not a per-sample label concern.
"""

from __future__ import annotations

import math
import random

from codec import (
    DYNAMICS_COUNT,
    EQ_COUNT,
    HARMONIC_COUNT,
    LIMITER_COUNT,
    LOUDNESS_COUNT,
    STEREO_COUNT,
    ControlDeltas,
    FeatureFrame,
)

# ── The 6 applyValidatedPlan formulas (AutoMasteringEngine.cpp:347-409) ───────
# Each takes the raw [-1,1] control value and returns the physical DSP param.
# These are the inverse of "what label should produce this sound" — they DEFINE
# what a label means. Keep them byte-identical to the C++ or parity breaks.

_K_MAX_EQ_GAIN_DB = 12.0  # AdaptiveEQ::kMaxGainDB (AdaptiveEQ.h:33)


def eq_to_gain_db(val: float) -> float:
    """eq[i] -> gainDB. val in [-1,1] -> gain in [-12,+12]."""
    return max(-_K_MAX_EQ_GAIN_DB, min(_K_MAX_EQ_GAIN_DB, val * _K_MAX_EQ_GAIN_DB))


def dynamics_to_threshold_ratio(val: float) -> tuple[float, float]:
    """dynamics[band] -> (thresholdDB, ratio). val in [-1,1]."""
    threshold = max(-40.0, min(-6.0, -20.0 + val * 8.0))
    ratio = max(1.0, min(6.0, 2.5 + val * 1.5))
    return threshold, ratio


def stereo_to_width(val: float) -> float:
    """stereo[region] -> width. val in [-1,1] -> width in [0,2]."""
    return max(0.0, min(2.0, 1.0 + val))


def harmonic_to_params(val: float) -> tuple[bool, float, float]:
    """harmonic[0] -> (enabled, drive, dryWet). One-sided: only [0,1] matters."""
    amount = max(0.0, min(1.0, val))
    enabled = amount > 0.01
    drive = max(0.0, min(18.0, 6.0 + amount * 12.0))
    dry_wet = max(0.0, min(0.6, amount))
    return enabled, drive, dry_wet


def limiter_to_ceiling(val: float) -> float:
    """limiter[0] -> ceiling dBTP. val in [-1,1] -> ceiling in [-3,-0.1]."""
    return max(-3.0, min(-0.1, -1.0 + val * 0.5))


def loudness_to_target(val: float) -> float:
    """loudness[0] -> targetLUFS. val in [-1,1] -> target in [-23,-8]."""
    return max(-23.0, min(-8.0, -14.0 + val * 6.0))


# ── Feature-driven teacher: maps a feature frame -> sensible control deltas ──
# This is the "labeling function". It encodes mastering heuristics that are
# defensible and bounded: loud material gets pulled toward -14 LUFS, tilted
# spectra get corrective EQ, etc. It is NOT a gold-standard mastering engineer —
# it is a deterministic, smooth, physically-sensible target the model can learn.
# The honest governance posture (referenceQuality="synthetic") reflects this.


def synthesize_deltas(frame: FeatureFrame, rng: random.Random) -> ControlDeltas:
    """Produce a 72-float ControlDeltas label from a feature frame.

    Deterministic given the frame + rng. Small jitter is added so repeated
    segments of the same source don't produce identical labels (data
    augmentation, not noise injection on the target semantics).
    """
    lufs = frame.integrated_lufs if frame.integrated_lufs > -70.0 else -23.0
    tilt = frame.spectral_tilt  # dB/octave; >0 = bright, <0 = dull
    tp = frame.true_peak_dbtp if frame.true_peak_dbtp > -60.0 else -1.0
    corr = frame.stereo_correlation[0] if frame.stereo_correlation else 0.5

    # ── EQ: corrective tilt. Bright (tilt>0) -> cut highs/boost lows; dull -> inverse.
    # Smooth across the 32 bands so the curve is musical (not jagged).
    eq: list[float] = []
    for i in range(EQ_COUNT):
        band_pos = i / max(1, EQ_COUNT - 1)  # 0=low ... 1=high
        # corrective gain: push band_pos toward 0.5 inversely to tilt
        corrective = math.tanh(-tilt * (band_pos - 0.5) / 3.0) * 0.4
        # mild musical shape: gentle smile (cut mids slightly) — small magnitude
        shape = -0.05 * math.sin(math.pi * band_pos)
        val = corrective + shape
        val += rng.uniform(-0.02, 0.02)  # tiny jitter
        eq.append(max(-1.0, min(1.0, val)))

    # ── Dynamics: louder & denser material -> more compression (positive delta).
    # band 0=sub, 1=low, 2=mid, 3=high (matches the 4 wired MultibandDynamics bands).
    compression_need = max(0.0, min(1.0, (lufs + 14.0) / 8.0))  # loud -> high
    dynamics: list[float] = []
    for band in range(DYNAMICS_COUNT):
        if band < 4:
            base = compression_need * 0.4
            # mid band compresses a touch more
            scale = [0.8, 0.9, 1.1, 0.9][band]
            val = base * scale + rng.uniform(-0.02, 0.02)
            dynamics.append(max(-1.0, min(1.0, val)))
        else:
            dynamics.append(0.0)  # dead slot

    # ── Stereo: narrow material (corr high) -> widen; wide (corr low) -> narrow slightly.
    stereo: list[float] = []
    for region in range(STEREO_COUNT):
        if region < 4:
            # corr near 1 = very mono -> widen (positive); corr near 0 -> mild narrow
            width_delta = (corr - 0.5) * 0.4
            # high region widens more musically
            scale = [0.6, 0.8, 1.0, 1.2][region]
            val = width_delta * scale + rng.uniform(-0.02, 0.02)
            stereo.append(max(-1.0, min(1.0, val)))
        else:
            stereo.append(0.0)  # dead slot

    # ── Harmonic: dull material -> add a little excitement (positive, small).
    harmonic = [0.0] * HARMONIC_COUNT
    if tilt < 0.0:
        amount = max(0.0, min(1.0, -tilt / 6.0 * 0.3))
        harmonic[0] = amount + rng.uniform(-0.01, 0.01)
        harmonic[0] = max(0.0, min(1.0, harmonic[0]))

    # ── Limiter: hot true-peak -> lower ceiling (negative delta).
    limiter = [0.0] * LIMITER_COUNT
    # tp near 0 -> need more headroom -> val negative -> ceiling lower
    limiter[0] = max(-1.0, min(1.0, (-1.0 - tp) * 0.5 + rng.uniform(-0.02, 0.02)))

    # ── Loudness: pull toward -14 LUFS. loud -> negative delta (target lower).
    loudness = [0.0] * LOUDNESS_COUNT
    loudness[0] = max(-1.0, min(1.0, math.tanh((-14.0 - lufs) / 6.0) + rng.uniform(-0.02, 0.02)))

    return ControlDeltas(
        eq=tuple(eq),
        dynamics=tuple(dynamics),
        stereo=tuple(stereo),
        harmonic=tuple(harmonic),
        limiter=tuple(limiter),
        loudness=tuple(loudness),
    )


def assert_label_semantics(deltas: ControlDeltas) -> None:
    """Sanity-check that a synthesized label is consistent with the DSP mapping.

    Catches drift between the teacher and applyValidatedPlan formulas. Raises on
    violation — used by the manifest builder before writing each line.
    """
    assert len(deltas.eq) == EQ_COUNT
    assert len(deltas.dynamics) == DYNAMICS_COUNT
    assert len(deltas.stereo) == STEREO_COUNT
    assert len(deltas.harmonic) == HARMONIC_COUNT
    assert len(deltas.limiter) == LIMITER_COUNT
    assert len(deltas.loudness) == LOUDNESS_COUNT

    for v in deltas.eq:
        assert -1.0 <= v <= 1.0, f"eq out of range: {v}"
    for v in deltas.dynamics:
        assert -1.0 <= v <= 1.0, f"dynamics out of range: {v}"
    for v in deltas.stereo:
        assert -1.0 <= v <= 1.0, f"stereo out of range: {v}"
    for v in deltas.harmonic:
        assert 0.0 <= v <= 1.0, f"harmonic must be one-sided [0,1]: {v}"  # parity: clamp to [0,1]
    for v in deltas.limiter:
        assert -1.0 <= v <= 1.0, f"limiter out of range: {v}"
    for v in deltas.loudness:
        assert -1.0 <= v <= 1.0, f"loudness out of range: {v}"

    # Dead-slot policy: these MUST be zero (DSP ignores them).
    for v in deltas.dynamics[4:]:
        assert v == 0.0, "dead dynamics slot must be zero"
    for v in deltas.stereo[4:]:
        assert v == 0.0, "dead stereo slot must be zero"
    for v in deltas.harmonic[1:]:
        assert v == 0.0, "dead harmonic slot must be zero"
    for v in deltas.limiter[1:]:
        assert v == 0.0, "dead limiter slot must be zero"
    for v in deltas.loudness[1:]:
        assert v == 0.0, "dead loudness slot must be zero"

    # Harmonic one-sidedness already enforced above.
