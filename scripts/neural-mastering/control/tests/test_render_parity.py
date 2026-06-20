#!/usr/bin/env python3
# =============================================================================
# tools/headless_mastering_render/test_render_parity.py
#
# T2 Phase-0 parity test for the headless mastering render harness.
# Run AFTER building libmore_phi_headless_render.so and placing it on PATH
# (or pass --lib <path>).
#
# Checks:
#   1. DETERMINISM     render(delta) twice on the same input -> byte-identical PCM
#                      + identical meters (engine.reset() + clearLastSafePlan()
#                      per candidate make this true).
#   2. RE-ENTRANCY     interleaving render(delta_A) and render(delta_B) across
#                      many calls does not leak state (candidate N output does
#                      not depend on candidate N-1's tail).
#   3. ADMISSIBILITY   output is finite, bounded, stereo-shaped, non-silent for
#                      a non-zero input; meters are finite or the documented
#                      sentinel (LUFS <= -200 for <3s audio).
#
# Not a pytest test by default (no pytest dep assumed on Lightning). Plain
# asserts with a clear pass/fail print. Convert to pytest by renaming the
# check_* functions to test_*.
# =============================================================================
from __future__ import annotations

import os
import sys

import numpy as np

# Allow importing the sibling module when run from the repo.
HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

from morephi_render import HeadlessRenderer, RenderMeters  # noqa: E402


DEFAULT_LIB_CANDIDATES = [
    os.environ.get("MOREPHI_HEADLESS_LIB", ""),
    os.path.join(HERE, "libmore_phi_headless_render.so"),
    os.path.join(HERE, "build", "libmore_phi_headless_render.so"),
    "./libmore_phi_headless_render.so",
]


def _resolve_lib(cli_path: str | None) -> str:
    candidates = [cli_path] if cli_path else []
    candidates += DEFAULT_LIB_CANDIDATES
    for c in candidates:
        if c and os.path.exists(c):
            return c
    raise FileNotFoundError(
        "Could not find libmore_phi_headless_render.so. Pass --lib <path> or set "
        "MOREPHI_HEADLESS_LIB."
    )


def _make_input(sr: float, duration_s: float = 4.0, freq_hz: float = 220.0) -> np.ndarray:
    """Deterministic interleaved stereo sine: left 0.3, right 0.25 (asymmetric)."""
    n = int(sr * duration_s)
    t = np.arange(n, dtype=np.float32) / sr
    left = (0.3 * np.sin(2 * np.pi * freq_hz * t)).astype(np.float32)
    right = (0.25 * np.sin(2 * np.pi * freq_hz * t)).astype(np.float32)
    pcm = np.empty(n * 2, dtype=np.float32)
    pcm[0::2] = left
    pcm[1::2] = right
    return pcm


def check_determinism(r: HeadlessRenderer, pcm: np.ndarray, delta: np.ndarray) -> None:
    """render(delta) twice -> byte-identical PCM + identical meters."""
    out1, m1 = r.render_candidate(pcm, delta)
    out2, m2 = r.render_candidate(pcm, delta)

    assert out1.shape == out2.shape == (pcm.shape[0] // 2, 2), "shape mismatch"
    # Byte-identical is the strictest determinism check.
    assert out1.tobytes() == out2.tobytes(), (
        "DETERMINISM FAIL: rendered PCM differs between two render(delta) calls. "
        "engine.reset()/clearLastSafePlan() is leaking state."
    )
    assert m1.lufs_integrated == m2.lufs_integrated, "LUFS meter not deterministic"
    assert m1.true_peak_dbtp == m2.true_peak_dbtp, "dBTP meter not deterministic"
    assert m1.limiter_gain_reduction_db == m2.limiter_gain_reduction_db, "limiter GR not deterministic"
    print(f"[PASS] determinism: PCM byte-identical, LUFS={m1.lufs_integrated:.3f}, "
          f"dBTP={m1.true_peak_dbtp:.3f}, GR={m1.limiter_gain_reduction_db:.3f}")


def check_reentrancy(r: HeadlessRenderer, pcm: np.ndarray, delta=None) -> None:
    """Interleaving two distinct delta vectors must not leak state.

    Candidate A and B are rendered in interleaved order; the second A must equal
    the first A (proving B did not leak into A's tail via limiter/normalizer/LUFS
    accumulators that reset() is supposed to flush).
    """
    delta_a = np.zeros(72, dtype=np.float32)
    delta_a[0] = 0.4   # EQ band 0 gain up
    delta_a[32] = 0.2  # dynamics band 0 threshold shift

    delta_b = np.zeros(72, dtype=np.float32)
    delta_b[5] = -0.3   # different EQ band
    delta_b[40] = -0.2  # different stereo region

    out_a1, m_a1 = r.render_candidate(pcm, delta_a)
    out_b,  m_b  = r.render_candidate(pcm, delta_b)
    out_a2, m_a2 = r.render_candidate(pcm, delta_a)

    assert out_a1.tobytes() == out_a2.tobytes(), (
        "RE-ENTRANCY FAIL: rendering delta_B between two delta_A calls changed "
        "delta_A's output. Per-candidate reset is not flushing state (limiter "
        "lookahead, compressor ballistics, normalizer ramp, or LUFS accumulator)."
    )
    assert m_a1.lufs_integrated == m_a2.lufs_integrated, "RE-ENTRANCY FAIL: LUFS leaked"
    # Sanity: distinct deltas should produce distinct output (catches a frozen engine).
    assert out_a1.tobytes() != out_b.tobytes(), (
        "RE-ENTRANCY WARNING: delta_A and delta_B produced identical PCM — the "
        "engine may be ignoring the deltas (check setActive(true) + applyValidatedPlan)."
    )
    print(f"[PASS] re-entrancy: A->B->A reproduces A exactly; A != B (deltas are wired)")


def check_admissibility(r: HeadlessRenderer, pcm: np.ndarray, delta: np.ndarray) -> None:
    """Output is finite, bounded, non-silent, stereo-shaped; meters are sane."""
    out, m = r.render_candidate(pcm, delta)

    assert np.isfinite(out).all(), "ADMISSIBILITY FAIL: rendered PCM contains NaN/Inf"
    # Limiter ceiling clamps to >= -3 dBTP; linear peak should stay well under 2.0.
    peak = float(np.abs(out).max())
    assert peak < 2.0, f"ADMISSIBILITY FAIL: output peak {peak} unbounded (>2.0)"
    assert peak > 1e-6, "ADMISSIBILITY FAIL: output is silent for a non-zero input"

    assert out.shape == (pcm.shape[0] // 2, 2), "ADMISSIBILITY FAIL: wrong stereo shape"

    # LUFS integrated is <= -200 until ~3s of audio. Our 4s input should yield a
    # real measurement; allow either (parity for short signals) but flag -inf.
    assert np.isfinite(m.lufs_integrated), "ADMISSIBILITY FAIL: LUFS not finite"
    assert np.isfinite(m.true_peak_dbtp), "ADMISSIBILITY FAIL: dBTP not finite"
    assert np.isfinite(m.limiter_gain_reduction_db), "ADMISSIBILITY FAIL: limiter GR not finite"
    print(f"[PASS] admissibility: peak={peak:.4f}, finite meters, shape={out.shape}")


def check_zero_delta_is_passthrough_ish(r: HeadlessRenderer, pcm: np.ndarray, delta=None) -> None:
    """delta=0 should NOT silence the chain — the mastering modules still run.

    Confirms the harness didn't accidentally bypass processBlock (e.g. forgot
    setActive(true)). Output should differ from input (EQ warm-start + limiter)
    but be non-silent.
    """
    delta_zero = np.zeros(72, dtype=np.float32)
    out, _ = r.render_candidate(pcm, delta_zero)
    in_stereo = pcm.reshape(-1, 2).astype(np.float32)
    # Output should be audibly processed (not identical to input).
    diff = float(np.abs(out - in_stereo).max())
    assert diff > 1e-4, (
        f"ZERO-DELTA WARNING: output == input (max diff {diff:.2e}); the chain may "
        "be bypassed. Expected the EQ warm-start + limiter to alter the signal."
    )
    print(f"[PASS] zero-delta: chain processes audio (max |out-in|={diff:.4f})")


def main() -> int:
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument("--lib", default=None, help="path to libmore_phi_headless_render.so")
    p.add_argument("--sr", type=float, default=48000.0)
    args = p.parse_args()

    lib = _resolve_lib(args.lib)
    sr = args.sr
    print(f"using library: {lib}")
    print(f"sample rate:   {sr}")

    r = HeadlessRenderer(lib, sample_rate=sr, block_size=512, normalizer_mode=0)
    print(f"chain latency: {r.chain_latency()} samples")

    pcm = _make_input(sr, duration_s=4.0)
    delta = np.zeros(72, dtype=np.float32)
    delta[8] = 0.2     # EQ band 8 gain axis (a wired slot)
    delta[32] = 0.1    # dynamics band 0
    delta[40] = 0.05   # stereo region 0

    failures = 0
    for fn in (check_determinism, check_reentrancy, check_admissibility, check_zero_delta_is_passthrough_ish):
        try:
            fn(r, pcm, delta)
        except AssertionError as e:
            failures += 1
            print(f"[FAIL] {fn.__name__}: {e}")
        except Exception as e:
            failures += 1
            print(f"[ERROR] {fn.__name__}: {type(e).__name__}: {e}")

    r.close()
    print(f"\n{'ALL PASS' if failures == 0 else f'{failures} FAILURE(S)'}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
