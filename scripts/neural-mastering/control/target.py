#!/usr/bin/env python3
"""T2 target profile — the perceptual/objective mastering target the CMA-ES
teacher optimizes candidate deltas against.

Phase 1: genre LUFS (parity with ChainPlanExecutor::kGenreLUFS) + a smooth
pink-ish spectral reference + safe crest/correlation/ceiling defaults.
Phase 2: replace spec_db with a measured per-genre corpus-median contour.
"""
from __future__ import annotations

from dataclasses import dataclass

# Genre integrated-LUFS targets (parity with src/AI/ChainPlanExecutor.cpp kGenreLUFS).
GENRE_LUFS = {
    "neutral": -14.0, "pop": -14.0, "rock": -14.0, "electronic": -14.0,
    "classical": -16.0, "jazz": -15.0, "hiphop": -13.0, "metal": -13.0,
    "folk": -15.0, "country": -14.0, "ambient": -16.0,
}


@dataclass
class TargetProfile:
    lufs: float
    spec_db: list  # 32 bands (dB) — the contour a balanced master tends toward
    crest_db: float
    corr: list     # 4 stereo-correlation targets
    ceiling_dbtp: float


def build_target(genre: str = "neutral") -> TargetProfile:
    """Build the mastering target for a genre. spec_db is a gentle descending
    pink-ish reference (features.compute_spectrum returns dB clamped [-120,24])."""
    lufs = GENRE_LUFS.get(genre, -14.0)
    spec_db = [-20.0 - 3.0 * (i / 31.0) for i in range(32)]
    return TargetProfile(
        lufs=lufs, spec_db=spec_db, crest_db=11.0,
        corr=[0.92, 0.88, 0.82, 0.74], ceiling_dbtp=-1.0,
    )
