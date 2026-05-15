#!/usr/bin/env python3
"""
Generate a stereo pink noise WAV file for use as source audio in FL Studio.

Pink noise has equal power per octave across the entire frequency spectrum,
making it ideal for testing EQ plugins — every frequency band will have
energy for the filter to act on.

Usage:
    python generate_pink_noise.py                    # Default: 10s, 44100Hz
    python generate_pink_noise.py --duration 30      # 30 seconds
    python generate_pink_noise.py --output my_noise.wav
"""

import argparse
import struct
import wave
import numpy as np


def generate_pink_noise(num_samples: int, rng: np.random.Generator) -> np.ndarray:
    """
    Generate pink noise using the Voss-McCartney algorithm.
    Pink noise has a 1/f power spectrum (equal energy per octave).
    """
    # Number of random sources (octave layers)
    num_sources = 16
    # Output array
    pink = np.zeros(num_samples, dtype=np.float64)

    # Voss-McCartney: maintain running sum of octave-spaced random sources
    values = rng.standard_normal(num_sources)
    running_sum = np.sum(values)

    for i in range(num_samples):
        # Find which source to update (lowest set bit position)
        # This gives us the octave-spaced update pattern
        k = 0
        idx = i
        while idx > 0 and (idx & 1) == 0 and k < num_sources - 1:
            k += 1
            idx >>= 1

        # Update one source and adjust running sum
        running_sum -= values[k]
        values[k] = rng.standard_normal()
        running_sum += values[k]

        # Add a white noise component for high-frequency detail
        pink[i] = running_sum + rng.standard_normal()

    # Normalize to [-1, 1] with headroom
    peak = np.max(np.abs(pink))
    if peak > 0:
        pink = pink / peak * 0.9  # -0.9 dBFS headroom

    return pink


def main():
    parser = argparse.ArgumentParser(description="Generate pink noise WAV for dataset testing")
    parser.add_argument("--duration", type=float, default=10.0,
                        help="Duration in seconds (default: 10)")
    parser.add_argument("--sample-rate", type=int, default=44100,
                        help="Sample rate (default: 44100)")
    parser.add_argument("--output", type=str, default="pink_noise.wav",
                        help="Output file path (default: pink_noise.wav)")
    args = parser.parse_args()

    num_samples = int(args.duration * args.sample_rate)
    rng = np.random.default_rng(42)  # Deterministic seed for reproducibility

    print(f"Generating {args.duration}s of stereo pink noise at {args.sample_rate}Hz...")

    # Generate two independent channels for stereo
    left = generate_pink_noise(num_samples, rng)
    right = generate_pink_noise(num_samples, rng)

    # Interleave stereo samples and convert to 16-bit PCM
    stereo = np.column_stack((left, right))
    pcm_data = (stereo * 32767).astype(np.int16)

    with wave.open(args.output, "w") as wf:
        wf.setnchannels(2)
        wf.setsampwidth(2)  # 16-bit
        wf.setframerate(args.sample_rate)
        wf.writeframes(pcm_data.tobytes())

    file_size_kb = num_samples * 2 * 2 / 1024  # stereo, 16-bit
    print(f"Saved: {args.output} ({file_size_kb:.0f} KB, {num_samples} frames)")
    print()
    print("Next step: Load this WAV file into an FL Studio playlist track")
    print("and route it through MorePhi hosting Pro-Q 4.")


if __name__ == "__main__":
    main()
