#!/usr/bin/env python3
"""Generate a synthetic audio corpus for scaling-up the control-regressor pipeline.

This is NOT a substitute for real audio (MUSDB18 / MedleyDB / FMA). It exists
to exercise the full feature-extraction + manifest + training pipeline at scale
on waveform-shaped signals with varied loudness, spectral content, and stereo
width, so we can validate scaling, timing, and training stability without
depending on flaky real-corpus downloads (FMA's primary host is currently
unreachable; see README "Real corpus acquisition").

The signals are synthesized to span the feature space the model cares about:
  - loudness: quiet (-28 LUFS-ish) to loud (-6 LUFS-ish)
  - spectral tilt: dull (low-heavy) to bright (high-heavy)
  - stereo width: narrow (mono-correlated) to wide (decorrelated)
  - content type: harmonic stacks, pink noise, filtered noise

Each track is stereo WAV at --sample-rate, duration --track-seconds. The
feature extractor + label teacher then have a genuinely varied distribution
to learn from.

Example:
  python gen_synthetic_corpus.py --out-dir corpus_synth --n-tracks 60 \\
    --track-seconds 30 --sample-rate 48000 --seed 1337
"""

from __future__ import annotations

import argparse
import math
import random
from pathlib import Path

import numpy as np
import soundfile as sf


def _pink(n: int, rng: np.random.Generator) -> np.ndarray:
    """Voss-McCartney pink noise (equal energy per octave, approx)."""
    white = rng.standard_normal(n)
    out = np.zeros(n)
    b0 = b1 = b2 = b3 = b4 = b5 = b6 = 0.0
    for i in range(n):
        x = white[i]
        b0 = 0.99886 * b0 + x * 0.0555179
        b1 = 0.99332 * b1 + x * 0.0750759
        b2 = 0.96900 * b2 + x * 0.1538520
        b3 = 0.86650 * b3 + x * 0.3104856
        b4 = 0.55000 * b4 + x * 0.5329522
        b5 = -0.7616 * b5 - x * 0.0168980
        out[i] = b0 + b1 + b2 + b3 + b4 + b5 + b6 + x * 0.5362
        b6 = x * 0.115926
    return out


def _harmonic_stack(freqs: list[float], n: int, t: np.ndarray, rng: np.random.Generator) -> np.ndarray:
    """A note with harmonics + slight detune drift for musicality."""
    sig = np.zeros(n)
    for k, f in enumerate(freqs):
        # detune each harmonic slightly per-track for variation
        detune = 1.0 + rng.uniform(-0.003, 0.003)
        amp = 1.0 / (k + 1)  # natural harmonic decay
        sig += amp * np.sin(2 * math.pi * f * detune * t)
    return sig


def _tilt_filter(sig: np.ndarray, tilt_db: float, sr: float) -> np.ndarray:
    """Apply a gentle spectral tilt via a one-pole low/high shelf approximation.

    tilt_db > 0 -> boost highs (bright); < 0 -> boost lows (dull). Simple but
    gives the extractor a genuinely tilted spectrum to measure.
    """
    # First-order high-shelf: blend highpassed signal back at a gain.
    if abs(tilt_db) < 0.1:
        return sig
    from scipy.signal import butter, lfilter
    fc = 1000.0
    gain = 10.0 ** (tilt_db / 40.0)
    b, a = butter(1, fc / (sr / 2), btype="high")
    high = lfilter(b, a, sig)
    return sig + (gain - 1.0) * high


def gen_track(idx: int, n_samples: int, sr: float, rng: random.Random) -> tuple[str, np.ndarray]:
    """Return (name, stereo audio [2, n_samples]) for one varied track."""
    np_rng = np.random.default_rng(rng.randint(0, 2**31 - 1))
    t = np.arange(n_samples) / sr

    # Pick a content type and loudness/tilt/width profile spanning the feature space.
    ctype = rng.choice(["harmonic", "pink", "filtered_noise"])
    loudness_gain = rng.uniform(0.05, 0.7)   # ~quiet to ~loud
    tilt_db = rng.uniform(-8.0, 8.0)         # dull to bright
    width = rng.uniform(0.2, 1.0)            # narrow to wide (side energy fraction)

    if ctype == "harmonic":
        base = rng.choice([55.0, 110.0, 146.83, 220.0, 293.66, 440.0])
        freqs = [base * (i + 1) for i in range(rng.randint(3, 7))]
        mono = _harmonic_stack(freqs, n_samples, t, np_rng)
    elif ctype == "pink":
        mono = _pink(n_samples, np_rng)
    else:  # filtered_noise
        from scipy.signal import butter, lfilter
        fc = rng.uniform(200.0, 4000.0)
        b, a = butter(2, fc / (sr / 2), btype="low")
        mono = lfilter(b, a, np_rng.standard_normal(n_samples))

    mono = _tilt_filter(mono, tilt_db, sr)
    # Normalize then scale to target loudness band (rough peak normalization).
    peak = np.max(np.abs(mono)) + 1e-12
    mono = mono / peak * loudness_gain

    # Build stereo with the chosen width: L = mid + width*side, R = mid - width*side,
    # where side is a decorrelated version of the signal (phase-ish via filtered noise).
    decorr = _pink(n_samples, np_rng)
    decorr = decorr / (np.max(np.abs(decorr)) + 1e-12)
    side = (mono * 0.0) + width * 0.3 * decorr * (np.max(np.abs(mono)) + 1e-9)
    L = mono + side
    R = mono - side
    # Clip to avoid true-peak overflow on the synthetic signal.
    audio = np.stack([L, R])
    audio = np.clip(audio, -0.99, 0.99).astype(np.float32)

    name = f"{ctype:14s}_t{idx:03d}_ld{int(loudness_gain*100):03d}_ti{int(tilt_db*10):+04d}_w{int(width*100):03d}".replace("+", "p").replace("-", "n")
    return name, audio


def build(args: argparse.Namespace) -> int:
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    rng = random.Random(args.seed)
    n_samples = int(round(args.track_seconds * args.sample_rate))

    for idx in range(args.n_tracks):
        name, audio = gen_track(idx, n_samples, args.sample_rate, rng)
        sf.write(str(out_dir / f"{name}.wav"), audio.T, args.sample_rate, subtype="FLOAT")
        if (idx + 1) % 20 == 0 or idx == 0:
            print(f"  {idx + 1}/{args.n_tracks}: {name}.wav")

    print(f"\nDONE: wrote {args.n_tracks} tracks ({args.track_seconds}s each) to {out_dir}")
    return 0


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out-dir", default="corpus_synth")
    p.add_argument("--n-tracks", type=int, default=60)
    p.add_argument("--track-seconds", type=float, default=30.0)
    p.add_argument("--sample-rate", type=int, default=48000)
    p.add_argument("--seed", type=int, default=1337)
    return p.parse_args()


if __name__ == "__main__":
    raise SystemExit(build(parse_args()))
