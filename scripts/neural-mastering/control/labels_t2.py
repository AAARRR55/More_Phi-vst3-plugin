#!/usr/bin/env python3
"""T2 teacher: recover mastering deltas by CMA-ES optimization of the multi-
objective loss against a REAL DSP render.

Searches the 43 ACTIVE control slots (eq32 + dynamics[0:4] + stereo[0:4]
+ harmonic[0] + limiter[0] + loudness[0]); the 29 dead slots stay 0 (the DSP
ignores them — parity with labels.py dead-slot policy). x0 = T1 synthesize_deltas
warm-start; if CMA-ES can't beat T1's loss, falls back to T1 (honest).

Returns a ControlDeltas exactly like labels.synthesize_deltas — drop-in for
build_manifest via --teacher t2.
"""
from __future__ import annotations

import numpy as np

from labels import synthesize_deltas, assert_label_semantics
from codec import control_deltas_to_vector, vector_to_control_deltas
from objective import loss
from target import build_target

# 43 active slots (the wired controls; 29 dead slots are fixed at 0).
ACTIVE = (
    list(range(32))              # eq[0..31]
    + [32, 33, 34, 35]           # dynamics[0..3]
    + [40, 41, 42, 43]           # stereo[0..3]
    + [48]                       # harmonic[0]
    + [56]                       # limiter[0]
    + [64]                       # loudness[0]
)


def _to_active(v72):
    return np.asarray([v72[i] for i in ACTIVE], dtype=np.float32)


def _from_active(av):
    d = np.zeros(72, dtype=np.float32)
    for j, i in enumerate(ACTIVE):
        d[i] = float(av[j])
    return d


def recover_deltas_render(frame, seg_audio, rng, genre="neutral", render_host=None,
                          max_fevals=400, sigma0=0.15):
    """Return a ControlDeltas (72 floats, [-1,1], dead slots 0).

    CMA-ES over the 43 active dims, warm-started from T1. Falls back to T1 if the
    search can't improve on T1's loss (so the teacher is never worse than T1).
    """
    import cma

    target = build_target(genre)
    t1 = synthesize_deltas(frame, rng)
    t1_vec = np.asarray(control_deltas_to_vector(t1), dtype=np.float32)
    x0 = _to_active(t1_vec)

    seg = np.asarray(seg_audio, dtype=np.float32)
    if seg.ndim == 1:
        seg = np.stack([seg, seg])

    def obj(av):
        try:
            value, _ = loss(_from_active(av), seg, target, render_host, x_t1=t1_vec)
            return value
        except Exception:  # noqa: BLE001 -- a failed render must not kill the search
            return 1e6

    t1_loss = obj(x0)
    # harmonic[0] (index 48) is ONE-SIDED [0,1] (an "amount", labels.py
    # harmonic_to_params clamps to [0,1]); all other active dims are [-1,1].
    lo = [0.0 if i == 48 else -1.0 for i in ACTIVE]
    hi = [1.0] * len(ACTIVE)
    es = cma.CMAEvolutionStrategy(
        x0.tolist(), sigma0,
        {
            "maxfevals": int(max_fevals),
            "verbose": -9,
            "seed": int(rng.randint(0, 2**31 - 1)),
            "bounds": [lo, hi],
        },
    )
    es.optimize(obj)
    xbest = np.asarray(es.result.xbest, dtype=np.float32)
    fbest = float(es.result.fbest)
    if fbest >= t1_loss - 1e-6:          # didn't beat T1 -> keep T1
        xbest = x0

    d72 = _from_active(xbest)
    deltas = vector_to_control_deltas([float(x) for x in d72])
    assert_label_semantics(deltas)       # never emit a label that violates the DSP mapping
    return deltas
