#!/usr/bin/env python3
"""P3 EQ augmentation: random minimum-phase biquad cascade (input-only).

Replaces the zero-phase FFT-multiply of generate_mastering_dataset.py::smooth_eq
(which pre-rings and is the wrong phase model for a compressor task — see
specs/006 plan.md section 1.1) with a causal 3-section RBJ biquad cascade:
low-shelf @180 Hz, peaking bell @1 kHz (the only true Q knob), high-shelf
@6 kHz. Stable IIR biquads are minimum-phase by construction (poles inside the
unit circle) -> NO pre-ring, so the augmented input stays a physically-plausible
signal the device could have received. That is a virtual-analog correctness
requirement, not a cosmetic one: a pre-ring transient is itself a cheap tag a
15.5M-param net can latch onto ("I saw this pre-ring -> recall the song"),
which would make a zero-phase augmentation self-defeating as a regularizer.

ponytail: implemented with torchaudio.functional.lfilter (existing dep, fast
causal IIR) rather than a hand-rolled Python DF1 loop. The loop is O(T) per
section in Python (~0.8 s per 262k-sample segment) and is unusable inside a
DataLoader worker; lfilter is the higher rung (reuse the installed dependency).

The filter is applied INPUT-ONLY with the clean device target left unchanged.
Known ceiling (plan section 1.1 tension): because the compressor D and spectral
shaping A do not commute (D's detector reads per-sample |x|), the pair
(A(x), D(x)) is NOT a sample of one coherent device map — this regularizes
toward LOCAL EQ invariance (a robustness proxy), not robustness itself. The
causally-correct fix (re-derive target = D(A(x)) through a chain we control) is
DEFERRED TO P4 because the SolidStateBusComp hardware is unreachable.
Minimum-phase was chosen partly to keep this bias in-distribution (analog-style
coloration) rather than adding zero-phase pre-ring that looks like a foreign
device artifact.

Self-check: python eq_augment.py
"""

from __future__ import annotations

import json
import math
import random
import sys

import torch
import torchaudio.functional as taf

# Anchor centres = the interior boundaries of smooth_eq's [20,180,1k,6k,Nyquist].
_LOW_F0 = 180.0
_MID_F0 = 1000.0
_HIGH_F0 = 6000.0
_SHELF_Q = 1.0 / math.sqrt(2.0)   # 0.7071 — broad, gentle, Butterworth-ish
_PRIME = 256                      # warm-start samples to kill IIR startup ring-in


def _rbj_shelf(f0: float, gain_db: float, Q: float, fs: float, low: bool):
    """RBJ cookbook low/high-shelf biquad -> normalized (b0,b1,b2,a1,a2)."""
    A = 10.0 ** (gain_db / 40.0)
    w0 = 2.0 * math.pi * f0 / fs
    cw, sw = math.cos(w0), math.sin(w0)
    alpha = sw / (2.0 * Q)
    sqA = math.sqrt(A)
    if low:
        b0 = A * ((A + 1) - (A - 1) * cw + 2.0 * sqA * alpha)
        b1 = 2.0 * A * ((A - 1) - (A + 1) * cw)
        b2 = A * ((A + 1) - (A - 1) * cw - 2.0 * sqA * alpha)
        a0 = (A + 1) + (A - 1) * cw + 2.0 * sqA * alpha
        a1 = -2.0 * ((A - 1) + (A + 1) * cw)
        a2 = (A + 1) + (A - 1) * cw - 2.0 * sqA * alpha
    else:
        b0 = A * ((A + 1) + (A - 1) * cw + 2.0 * sqA * alpha)
        b1 = -2.0 * A * ((A - 1) + (A + 1) * cw)
        b2 = A * ((A + 1) + (A - 1) * cw - 2.0 * sqA * alpha)
        a0 = (A + 1) - (A - 1) * cw + 2.0 * sqA * alpha
        a1 = 2.0 * ((A - 1) - (A + 1) * cw)
        a2 = (A + 1) - (A - 1) * cw - 2.0 * sqA * alpha
    return (b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0)


def _rbj_peak(f0: float, gain_db: float, Q: float, fs: float):
    """RBJ cookbook peaking/bell biquad -> normalized (b0,b1,b2,a1,a2)."""
    A = 10.0 ** (gain_db / 40.0)
    w0 = 2.0 * math.pi * f0 / fs
    cw, sw = math.cos(w0), math.sin(w0)
    alpha = sw / (2.0 * Q)
    b0 = 1.0 + alpha * A
    b1 = -2.0 * cw
    b2 = 1.0 - alpha * A
    a0 = 1.0 + alpha / A
    a1 = -2.0 * cw
    a2 = 1.0 - alpha / A
    return (b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0)


def _lfilter(xf: torch.Tensor, a: torch.Tensor, b: torch.Tensor) -> torch.Tensor:
    """torchaudio lfilter with output clamping DISABLED. Clamping (on by default)
    saturates any shelf whose impulse response exceeds [-1,1] (e.g. b0>1 for a
    boost), corrupting the frequency response. The kwarg was renamed across
    versions: `clamp` (>=2.x) -> `clamp_ct` (0.11-1.x); try both, then bare."""
    try:
        return taf.lfilter(xf, a, b, clamp=False)
    except TypeError:
        try:
            return taf.lfilter(xf, a, b, clamp_ct=False)
        except TypeError:
            return taf.lfilter(xf, a, b)


def _lfilter_section(x: torch.Tensor, sec: tuple, prime: int) -> torch.Tensor:
    """Apply one biquad section to [..., T] via a causal IIR, primed to kill
    startup ring-in. Linked (identical coeffs) across all leading dims."""
    b0, b1, b2, a1, a2 = sec
    dev = x.device
    a = torch.tensor([1.0, a1, a2], dtype=torch.float32, device=dev)
    b = torch.tensor([b0, b1, b2], dtype=torch.float32, device=dev)
    xf = x.float()
    if prime > 0 and xf.shape[-1] > 0:
        head = xf[..., :1].expand(*xf.shape[:-1], prime).clone()
        xp = torch.cat([head, xf], dim=-1)
        return _lfilter(xp, a, b)[..., prime:].to(x.dtype)
    return _lfilter(xf, a, b).to(x.dtype)


def min_phase_eq(waveform: torch.Tensor, sample_rate, low_db: float, mid_db: float,
                 high_db: float, mid_q: float = 1.0, prime: int = _PRIME) -> torch.Tensor:
    """Apply a 3-section minimum-phase EQ cascade.

    waveform [..., T] (stereo [2,T] or batched [B,2,T]); returns same shape, fp32
    internally, dtype preserved. Sections: low-shelf @180 Hz (gain low_db,
    Q=0.707), peaking bell @1 kHz (gain mid_db, Q=mid_q), high-shelf @6 kHz
    (gain high_db, Q=0.707). All gains 0 dB -> exact passthrough. Stable biquads
    -> minimum-phase, no pre-ring. Re-peak-normalize with tm.peak_normalize(_,
    0.98) at the call site so a +6 dB boost cannot clip the net's tanh head.
    """
    fs = float(sample_rate)
    for f0 in (_LOW_F0, _MID_F0, _HIGH_F0):
        if f0 >= fs / 2.0:
            raise ValueError(f"anchor {f0} Hz >= Nyquist ({fs / 2} Hz) at sample_rate {fs}")
    x = waveform
    x = _lfilter_section(x, _rbj_shelf(_LOW_F0, low_db, _SHELF_Q, fs, low=True), prime)
    x = _lfilter_section(x, _rbj_peak(_MID_F0, mid_db, mid_q, fs), prime)
    x = _lfilter_section(x, _rbj_shelf(_HIGH_F0, high_db, _SHELF_Q, fs, low=False), prime)
    return x


def sample_eq_gains(rng: random.Random, max_db: float = 6.0) -> tuple[float, float, float, float]:
    """Per-sample random EQ gains: low/mid/high ~ U(-max_db,+max_db), mid_q ~ U(0.5,1.5).

    Draw AFTER crop_pair so the crop stays deterministic-per-index while EQ varies
    per-epoch (multiplicative effective-dataset for ~175 songs)."""
    return (rng.uniform(-max_db, max_db), rng.uniform(-max_db, max_db),
            rng.uniform(-max_db, max_db), rng.uniform(0.5, 1.5))


def eq_probe_grid() -> list[tuple[str, tuple[float, float, float, float]]]:
    """Fixed deterministic perturbation grid for the eval robustness probe
    (plan section 6). 'flat' is the self-consistency anchor (must match the
    clean pass within tolerance -> wiring-bug catcher)."""
    return [
        ("flat",   (0.0, 0.0, 0.0, 1.0)),
        ("low+6",  (6.0, 0.0, 0.0, 1.0)),
        ("low-6",  (-6.0, 0.0, 0.0, 1.0)),
        ("high+6", (0.0, 0.0, 6.0, 1.0)),
        ("high-6", (0.0, 0.0, -6.0, 1.0)),
        ("tilt+3", (3.0, 0.0, -3.0, 1.0)),
        ("tilt-3", (-3.0, 0.0, 3.0, 1.0)),
    ]


def _mag_db(impulse_response_1d: torch.Tensor, fs: float, freq: float) -> float:
    X = torch.fft.rfft(impulse_response_1d)
    freqs = torch.fft.rfftfreq(impulse_response_1d.numel(), 1.0 / fs)
    idx = int((freqs - freq).abs().argmin())
    return float(20.0 * torch.log10(X[idx].abs().clamp_min(1e-8)))


def selfcheck() -> int:
    fs = 48000.0
    sr = int(fs)
    failures: list[str] = []

    g = torch.Generator().manual_seed(0)
    x = torch.randn(2, 32768, generator=g)
    x = x / x.abs().max() * 0.9

    # (a) IDENTITY: all gains 0 dB -> passthrough.
    y0 = min_phase_eq(x, sr, 0.0, 0.0, 0.0, 1.0)
    if (y0 - x).abs().max() > 1e-4:
        failures.append(f"identity max|y-x|={( y0 - x).abs().max():.2e} > 1e-4")

    # (b) BOUNDED MAGNITUDE AT ANCHORS (impulse response, prime=0 for clean |H|).
    N = 1 << 15
    delta = torch.zeros(1, N)
    delta[0, 0] = 1.0

    def h(lo, mi, hi, q=1.0):
        return min_phase_eq(delta, sr, lo, mi, hi, q, prime=0)[0]

    lo_db = _mag_db(h(6.0, 0.0, 0.0), fs, 60.0)
    mid_db = _mag_db(h(0.0, 6.0, 0.0), fs, 1000.0)
    hi_db = _mag_db(h(0.0, 0.0, 6.0), fs, 12000.0)
    for name, val in (("low@60", lo_db), ("mid@1k", mid_db), ("high@12k", hi_db)):
        if not (5.7 <= val <= 6.3):
            failures.append(f"anchor {name} gain {val:.2f} dB outside [5.7,6.3]")

    # (c) MINIMUM-PHASE / NO PRE-RING: impulse at K -> output strictly zero before K.
    # A zero-phase FFT kernel would ring before K; a causal IIR cannot.
    K = 256
    buf = torch.zeros(1, N)
    buf[0, K] = 1.0
    yk = min_phase_eq(buf, sr, 6.0, 0.0, -6.0, 1.0, prime=0)[0]
    pre = float(yk[:K].abs().max())
    if pre > 1e-7:
        failures.append(f"pre-ring: output before impulse = {pre:.2e} (not causal)")

    # (d) RBJ STABILITY: polynomial conditions for a Q sweep (a future Q=0.1 fails loud).
    for q in (0.5, _SHELF_Q, 1.0, 1.5):
        secs = (_rbj_shelf(_LOW_F0, 6.0, _SHELF_Q, fs, True),
                _rbj_peak(_MID_F0, 6.0, q, fs),
                _rbj_shelf(_HIGH_F0, -6.0, _SHELF_Q, fs, False))
        for _b0, _b1, _b2, a1, a2 in secs:
            if not ((1 + a1 + a2 > 0) and (1 - a1 + a2 > 0) and (1 - abs(a2) > 0)):
                failures.append(f"RBJ unstable for q={q}: a1={a1:.4f} a2={a2:.4f}")

    # (e) FINITENESS.
    if not torch.isfinite(y0).all():
        failures.append("non-finite output")

    # (f) STEREO INVARIANCE: identical L/R -> identical L/R out (linked coeffs).
    xs = torch.stack([x[0], x[0]], dim=0)
    ys = min_phase_eq(xs, sr, 6.0, -3.0, 4.0, 1.1)
    if (ys[0] - ys[1]).abs().max() > 1e-5:
        failures.append("stereo channels diverge (not linked)")

    # (g) PRIMING: DC input -> first kept sample near steady-state DC gain.
    # low-shelf DC gain = A^2 = 10^(g/20); without priming the first sample is b0*DC.
    dc = torch.ones(1, 4096) * 0.5
    expected = 0.5 * 10.0 ** (6.0 / 20.0)
    dc_primed = float(min_phase_eq(dc, sr, 6.0, 0.0, 0.0, 1.0, prime=_PRIME)[0, 0])
    dc_cold = float(min_phase_eq(dc, sr, 6.0, 0.0, 0.0, 1.0, prime=0)[0, 0])
    if abs(dc_primed - expected) > 0.05 * abs(expected):
        failures.append(f"priming DC: first sample {dc_primed:.4f} not near steady {expected:.4f}")
    if abs(dc_primed - expected) >= abs(dc_cold - expected):
        failures.append("priming did not improve steady-state convergence vs cold start")

    report = {
        "identityMaxErr": float((y0 - x).abs().max()),
        "anchorGainDb": {"low@60": round(lo_db, 2), "mid@1k": round(mid_db, 2), "high@12k": round(hi_db, 2)},
        "preRingMax": pre,
        "primingDC": {"primed": round(dc_primed, 4), "cold": round(dc_cold, 4), "expected": round(expected, 4)},
        "failures": failures,
    }
    print(json.dumps(report, indent=2, sort_keys=True))
    if failures:
        print("SELF-CHECK FAIL", file=sys.stderr)
        return 1
    print("SELF-CHECK PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(selfcheck())
