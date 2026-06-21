#!/usr/bin/env python3
"""T2 target profile — the perceptual/objective mastering target the CMA-ES
teacher (objective.py) optimizes candidate deltas against.

v1 (original): a fixed hand-written pink-ish spectral reference
    spec_db = [-20.0 - 3.0 * (i / 31.0)]
plus a hardcoded genre LUFS table. That synthetic target is the root cause of
the label-domain-vs-audio-domain mismatch this project uncovered: a one-size
spectral line cannot be a real mastering reference, and it taught the student
to apply a fixed EQ tilt that overshot loudness by ~11 dB on real audio.

v2 (this version) ports the deterministic decision logic from the SonicMaster
v3 decision engine (engine/sonicmaster_engine/decision.py): the spectral
reference is now CORRECTIVE per-track — each band pulled toward the input's
spectral median, clamped to +/-6 dB (gentle flattening, never a fixed tilt),
exactly as a balanced master tends toward. LUFS/ceiling/crest stay genre-
keyed (parity with ChainPlanExecutor::kGenreLUFS) but the spectral target is
now adaptive and defensible.

This makes the teacher's spectral term measure "did you flatten the lopsided
spectrum?" instead of "did you match an arbitrary line?" — the foundation a
real mastering brain needs. Ported faithfully; behavior is deterministic.

Phase 3: replace the median model with a measured per-genre corpus-median
contour (the AAM human-mix restraint corpus already on the remote is the
source for that).
"""
from __future__ import annotations

from dataclasses import dataclass

# Genre integrated-LUFS targets (parity with src/AI/ChainPlanExecutor.cpp kGenreLUFS).
GENRE_LUFS = {
    "neutral": -14.0, "pop": -14.0, "rock": -14.0, "electronic": -14.0,
    "classical": -16.0, "jazz": -15.0, "hiphop": -13.0, "metal": -13.0,
    "folk": -15.0, "country": -14.0, "ambient": -16.0,
}

# SonicMaster decision.py parity: corrective EQ clamp (dB). Gentle flattening,
# never a fixed tilt. Audio-domain sweep proved a flat +6 dB EQ lift raises
# output ~11 dB, so the clamp is the single most important loudness guardrail.
_EQ_CORRECTIVE_CLAMP_DB = 6.0
# SonicMaster decision.py: the correction is -0.5 * (level - median); keep that
# factor (do not fully flatten in one pass).
_EQ_MEDIAN_PULL_FACTOR = 0.5


@dataclass
class TargetProfile:
    lufs: float
    spec_db: list  # 32 bands (dB) — corrective-per-input when built via build_target_from_features
    crest_db: float
    corr: list     # 4 stereo-correlation targets
    ceiling_dbtp: float


def _pink_reference() -> list:
    """v1 fallback: the original fixed pink-ish contour (dB clamped [-120,24])."""
    return [-20.0 - 3.0 * (i / 31.0) for i in range(32)]


def _corrective_spec_db(input_spec_db: list, clamp_db: float = _EQ_CORRECTIVE_CLAMP_DB,
                        pull: float = _EQ_MEDIAN_PULL_FACTOR) -> list:
    """Port of SonicMaster decision.py _eq_bands: corrective target contour.

    Pulls each band toward the input's spectral MEDIAN by `pull` (0.5 = halfway),
    clamped to +/-`clamp_db`. The TARGET the teacher optimizes toward is therefore
    "the input spectrum, gently flattened" — not a fixed line. A student that
    matches this target applies corrective (median-toward) EQ, never a broad boost.

    (SonicMaster's _eq_bands emits per-band *gains* = -pull*(lvl-median); here we
    express the same idea as a TARGET CONTOUR = input + those gains, since
    objective.py scores spec_dB(render) vs spec_db(target).)
    """
    import numpy as np

    levels = np.asarray(input_spec_db, dtype=np.float64)
    if levels.size == 0 or not np.isfinite(levels).any():
        return _pink_reference()
    median = float(np.median(levels[np.isfinite(levels)]))
    target = []
    for lvl in input_spec_db:
        # gain = -pull * (lvl - median), clamped; target = input + gain
        gain = -pull * (float(lvl) - median)
        gain = max(-clamp_db, min(clamp_db, gain))
        target.append(float(lvl) + gain)
    return target


def build_target(genre: str = "neutral") -> TargetProfile:
    """Build the v1 fallback target (fixed pink reference). Kept for backward
    compatibility with code paths that don't have input features. Prefer
    build_target_from_features() for the corrective (v2) target."""
    lufs = GENRE_LUFS.get(genre, -14.0)
    spec_db = _pink_reference()
    return TargetProfile(
        lufs=lufs, spec_db=spec_db, crest_db=11.0,
        corr=[0.92, 0.88, 0.82, 0.74], ceiling_dbtp=-1.0,
    )


def build_target_from_features(input_spec_db: list, genre: str = "neutral",
                               input_lufs: float | None = None) -> TargetProfile:
    """v2 corrective target — the real upgrade. Given the INPUT spectrum, build a
    target contour that flattens it toward median (SonicMaster decision logic),
    so the teacher scores "did you correctively flatten?" not "did you match a
    fixed line?". LUFS target is genre-keyed (parity with kGenreLUFS).

    Parameters
    ----------
    input_spec_db : 32-band input spectral levels (dB), as features.compute_spectrum returns.
    genre : genre key for the LUFS/ceiling/crest profile.
    input_lufs : optional; the target_lufs is still genre-keyed (the teacher's
        L_loud term handles the makeup math), kept for API symmetry.
    """
    lufs = GENRE_LUFS.get(genre, -14.0)
    spec_db = _corrective_spec_db(input_spec_db) if input_spec_db else _pink_reference()
    return TargetProfile(
        lufs=lufs, spec_db=spec_db, crest_db=11.0,
        corr=[0.92, 0.88, 0.82, 0.74], ceiling_dbtp=-1.0,
    )
