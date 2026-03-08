#!/usr/bin/env python3
"""
MorphSnap Test Audio Generator
Generates broadband test signals for plugin parameter dataset generation.

The Physics:
- Sine waves only contain ONE frequency - useless for testing EQ/filters
- Pink noise contains ALL frequencies at equal power per octave - ideal for DSP testing
- White noise contains ALL frequencies at equal power per Hz - shows precise EQ shapes
- Music/drum loops contain real-world frequency content

Usage:
    python generate_test_audio.py --output test_audio/ --duration 5
"""

import argparse
import os
import numpy as np
from scipy.io import wavfile


def generate_pink_noise(duration_sec: float, sample_rate: int = 48000) -> np.ndarray:
    """
    Generate pink noise (1/f noise).
    Equal power per octave - ideal for hearing filter shapes.

    Uses the Voss-McCartney algorithm for high-quality pink noise.
    """
    num_samples = int(duration_sec * sample_rate)

    # Voss-McCartney algorithm: sum of multiple white noise sources
    # at different sample rates creates 1/f characteristic
    num_sources = 8
    sources = np.zeros(num_sources)
    output = np.zeros(num_samples)

    for i in range(num_samples):
        # Update one random source per sample (based on position)
        for j in range(num_sources):
            if (i >> j) & 1 == 0 or i == 0:
                sources[j] = np.random.uniform(-1, 1)

        # Sum all sources for pink characteristic
        output[i] = np.sum(sources) / num_sources

    # Normalize to -1 to 1 range
    output = output / np.max(np.abs(output)) * 0.9
    return output


def generate_white_noise(duration_sec: float, sample_rate: int = 48000) -> np.ndarray:
    """
    Generate white noise.
    Equal power per Hz frequency - shows precise EQ cuts/boosts.
    """
    num_samples = int(duration_sec * sample_rate)
    output = np.random.uniform(-1, 1, num_samples)

    # Normalize to -1 to 1 range
    output = output / np.max(np.abs(output)) * 0.9
    return output


def generate_brown_noise(duration_sec: float, sample_rate: int = 48000) -> np.ndarray:
    """
    Generate brown/red noise (1/f^2 noise).
    Deep, bass-heavy - good for testing low-frequency response.
    """
    num_samples = int(duration_sec * sample_rate)

    # Integrate white noise to get brown noise
    white = np.random.uniform(-1, 1, num_samples)
    output = np.cumsum(white)

    # Normalize
    output = output / np.max(np.abs(output)) * 0.9
    return output


def generate_sine_sweep(duration_sec: float, sample_rate: int = 48000,
                        start_hz: float = 20.0, end_hz: float = 20000.0) -> np.ndarray:
    """
    Generate exponential sine sweep.
    Every frequency is played - perfect for impulse response measurement.
    """
    num_samples = int(duration_sec * sample_rate)
    t = np.linspace(0, duration_sec, num_samples)

    # Exponential frequency sweep
    r = np.log(end_hz / start_hz)
    phase = 2 * np.pi * start_hz * num_samples / (sample_rate * r) * (np.exp(r * t / duration_sec) - 1)
    output = np.sin(phase)

    # Normalize
    output = output / np.max(np.abs(output)) * 0.9
    return output


def generate_multitone(duration_sec: float, sample_rate: int = 48000,
                       frequencies: list = None) -> np.ndarray:
    """
    Generate multi-tone signal with specific frequencies.
    Good for testing specific EQ bands.
    """
    if frequencies is None:
        # Standard octave bands: 63, 125, 250, 500, 1k, 2k, 4k, 8k, 16k Hz
        frequencies = [63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000]

    num_samples = int(duration_sec * sample_rate)
    t = np.linspace(0, duration_sec, num_samples)

    output = np.zeros(num_samples)
    for freq in frequencies:
        output += np.sin(2 * np.pi * freq * t)

    # Normalize
    output = output / np.max(np.abs(output)) * 0.9
    return output


def generate_click_train(duration_sec: float, sample_rate: int = 48000,
                         bpm: float = 120.0) -> np.ndarray:
    """
    Generate click train (transient test signal).
    Good for testing dynamics processors (compressors, limiters).
    """
    num_samples = int(duration_sec * sample_rate)
    output = np.zeros(num_samples)

    samples_per_beat = int(sample_rate * 60.0 / bpm)
    click_duration = int(sample_rate * 0.01)  # 10ms clicks

    for i in range(0, num_samples, samples_per_beat):
        end = min(i + click_duration, num_samples)
        # Exponential decay click
        click_length = end - i
        decay = np.exp(-np.linspace(0, 10, click_length))
        output[i:end] = decay * 0.9

    return output


def make_stereo(mono: np.ndarray) -> np.ndarray:
    """Convert mono to stereo by duplicating channels."""
    return np.column_stack([mono, mono])


def save_wav(audio: np.ndarray, filename: str, sample_rate: int = 48000):
    """Save audio as 32-bit float WAV file."""
    # Convert to float32
    audio = audio.astype(np.float32)

    # Create directory if needed
    os.makedirs(os.path.dirname(filename) if os.path.dirname(filename) else '.', exist_ok=True)

    wavfile.write(filename, sample_rate, audio)
    print(f"  Created: {filename} ({len(audio) / sample_rate:.1f}s, {audio.shape})")


def main():
    parser = argparse.ArgumentParser(description="Generate test audio files for dataset generation")
    parser.add_argument("--output", "-o", default="test_audio",
                        help="Output directory for generated files")
    parser.add_argument("--duration", "-d", type=float, default=5.0,
                        help="Duration in seconds (default: 5)")
    parser.add_argument("--sample-rate", "-s", type=int, default=48000,
                        help="Sample rate (default: 48000)")
    parser.add_argument("--all", "-a", action="store_true",
                        help="Generate all test signal types")
    parser.add_argument("--pink", action="store_true", help="Generate pink noise")
    parser.add_argument("--white", action="store_true", help="Generate white noise")
    parser.add_argument("--brown", action="store_true", help="Generate brown noise")
    parser.add_argument("--sweep", action="store_true", help="Generate sine sweep")
    parser.add_argument("--multitone", action="store_true", help="Generate multi-tone")
    parser.add_argument("--clicks", action="store_true", help="Generate click train")

    args = parser.parse_args()

    # Default to pink noise if nothing specified
    if not any([args.all, args.pink, args.white, args.brown, args.sweep,
                args.multitone, args.clicks]):
        args.pink = True

    print(f"\nGenerating test audio files ({args.duration}s @ {args.sample_rate}Hz)...")
    print(f"Output directory: {args.output}\n")

    if args.all or args.pink:
        audio = generate_pink_noise(args.duration, args.sample_rate)
        save_wav(make_stereo(audio), os.path.join(args.output, "pink_noise.wav"), args.sample_rate)

    if args.all or args.white:
        audio = generate_white_noise(args.duration, args.sample_rate)
        save_wav(make_stereo(audio), os.path.join(args.output, "white_noise.wav"), args.sample_rate)

    if args.all or args.brown:
        audio = generate_brown_noise(args.duration, args.sample_rate)
        save_wav(make_stereo(audio), os.path.join(args.output, "brown_noise.wav"), args.sample_rate)

    if args.all or args.sweep:
        audio = generate_sine_sweep(args.duration, args.sample_rate)
        save_wav(make_stereo(audio), os.path.join(args.output, "sine_sweep.wav"), args.sample_rate)

    if args.all or args.multitone:
        audio = generate_multitone(args.duration, args.sample_rate)
        save_wav(make_stereo(audio), os.path.join(args.output, "multitone.wav"), args.sample_rate)

    if args.all or args.clicks:
        audio = generate_click_train(args.duration, args.sample_rate)
        save_wav(make_stereo(audio), os.path.join(args.output, "click_train.wav"), args.sample_rate)

    print("\n" + "="*60)
    print("RECOMMENDED FOR EQ/FILTER DATASETS:")
    print("  - pink_noise.wav    : Best for hearing exact filter shapes")
    print("  - white_noise.wav   : Shows precise EQ cuts/boosts")
    print("  - sine_sweep.wav    : Measures impulse response")
    print("\nFOR DYNAMICS PROCESSORS (compressors, limiters):")
    print("  - click_train.wav   : Tests transient response")
    print("="*60 + "\n")


if __name__ == "__main__":
    main()
