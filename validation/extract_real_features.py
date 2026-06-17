#!/usr/bin/env python3
"""
Extract real audio features from rendered WAV files using librosa/numpy/scipy.
Replaces placeholder features.json files with measured FeatureExtractor output.

Output schema matches the C++ FeatureExtractor (FeatureExtractor.h):
  spectral: mfcc[13], chroma[12], spectralCentroid, spectralRolloff,
            spectralFlux, spectralSpread, spectralFlatness
  temporal: rmsEnergy (dB), peakAmplitude (dB), crestFactor,
            attackTime (ms), transientDensity, zeroCrossingRate
  perceptual: lufs (approx), truePeakDb, roughness, sharpness,
              brightness, dynamicRange
"""
import json
import sys
from pathlib import Path

import librosa
import numpy as np
import soundfile as sf


def extract_features(audio_path: Path, sr: int = 48000) -> dict:
    """Extract a full 42-dimension feature vector from a WAV file."""
    y, file_sr = sf.read(str(audio_path), dtype='float32')

    # Mix to mono if stereo
    if y.ndim == 2:
        y_mono = np.mean(y, axis=1)
    else:
        y_mono = y

    # Resample if needed
    if file_sr != sr:
        y_mono = librosa.resample(y_mono, orig_sr=file_sr, target_sr=sr)

    n_fft = 2048
    hop_length = 512

    # ── Spectral Features ──────────────────────────────────────────────
    # MFCC (13 coefficients)
    mfcc = librosa.feature.mfcc(y=y_mono, sr=sr, n_mfcc=13,
                                 n_fft=n_fft, hop_length=hop_length)
    mfcc_mean = mfcc.mean(axis=1).tolist()

    # Chroma (12 semitones)
    chroma = librosa.feature.chroma_stft(y=y_mono, sr=sr,
                                          n_fft=n_fft, hop_length=hop_length)
    chroma_mean = chroma.mean(axis=1).tolist()

    # Spectral centroid (Hz)
    cent = librosa.feature.spectral_centroid(y=y_mono, sr=sr,
                                              n_fft=n_fft, hop_length=hop_length)
    spectral_centroid = float(np.mean(cent))

    # Spectral rolloff (Hz, 85% energy)
    rolloff = librosa.feature.spectral_rolloff(y=y_mono, sr=sr,
                                                n_fft=n_fft, hop_length=hop_length,
                                                roll_percent=0.85)
    spectral_rolloff = float(np.mean(rolloff))

    # Spectral flux (L2 norm of spectral difference between frames)
    S = np.abs(librosa.stft(y_mono, n_fft=n_fft, hop_length=hop_length))
    diff = np.diff(S, axis=1)
    flux_per_frame = np.linalg.norm(diff, axis=0)
    spectral_flux = float(np.mean(flux_per_frame)) if len(flux_per_frame) > 0 else 0.0

    # Spectral spread (std deviation around centroid)
    freqs = librosa.fft_frequencies(sr=sr, n_fft=n_fft)
    S_norm = S / (S.sum(axis=0, keepdims=True) + 1e-10)
    centroid_per_frame = (freqs[:, None] * S_norm).sum(axis=0)
    spread_per_frame = np.sqrt(((freqs[:, None] - centroid_per_frame) ** 2 * S_norm).sum(axis=0))
    spectral_spread = float(np.mean(spread_per_frame))

    # Spectral flatness (Wiener entropy, 0-1)
    flatness = librosa.feature.spectral_flatness(y=y_mono, n_fft=n_fft,
                                                  hop_length=hop_length)
    spectral_flatness = float(np.mean(flatness))

    # ── Temporal Features ──────────────────────────────────────────────
    # RMS energy in dB
    rms_linear = float(np.sqrt(np.mean(y_mono ** 2)))
    rms_db = float(20.0 * np.log10(max(rms_linear, 1e-10)))

    # Peak amplitude in dB
    peak_linear = float(np.max(np.abs(y_mono)))
    peak_db = float(20.0 * np.log10(max(peak_linear, 1e-10)))

    # Crest factor (peak / RMS, linear)
    crest_factor = float(peak_linear / max(rms_linear, 1e-10))

    # Attack time (ms): time from 10% to 90% of peak envelope
    envelope = np.abs(librosa.onset.onset_strength(y=y_mono, sr=sr,
                                                    hop_length=hop_length))
    if len(envelope) > 0 and np.max(envelope) > 0:
        env_norm = envelope / np.max(envelope)
        t10 = np.argmax(env_norm >= 0.1)
        t90 = np.argmax(env_norm >= 0.9)
        attack_samples = max(t90 - t10, 0) * hop_length
        attack_time_ms = float(attack_samples / sr * 1000.0)
    else:
        attack_time_ms = 0.0

    # Transient density (onsets per second)
    onsets = librosa.onset.onset_detect(y=y_mono, sr=sr, hop_length=hop_length)
    duration = len(y_mono) / sr
    transient_density = float(len(onsets) / max(duration, 1e-6))

    # Zero crossing rate
    zcr = librosa.feature.zero_crossing_rate(y_mono, frame_length=n_fft,
                                              hop_length=hop_length)
    zcr_per_sec = float(np.mean(zcr) * sr / hop_length)

    # ── Perceptual Features ────────────────────────────────────────────
    # LUFS approximation (K-weighted RMS with channel weighting)
    # True ITU-R BS.1770-4 requires K-weighting filter; approximate with RMS - 0.691 dB offset
    lufs_approx = rms_db - 0.691

    # True peak (4x oversampled)
    y_up = librosa.resample(y_mono, orig_sr=sr, target_sr=sr * 4)
    true_peak_linear = float(np.max(np.abs(y_up)))
    true_peak_db = float(20.0 * np.log10(max(true_peak_linear, 1e-10)))

    # Roughness (simplified: energy in critical-band beating range 15-300 Hz modulation)
    # Use spectral flux variance as a proxy
    roughness = float(np.std(flux_per_frame)) if len(flux_per_frame) > 0 else 0.0

    # Sharpness (Aures model approximation: high-freq weighted centroid)
    bark_weights = np.minimum(freqs / 1500.0, 1.0) + 0.5 * np.maximum((freqs - 1500.0) / 1500.0, 0.0)
    weighted_S = S * bark_weights[:, None]
    sharpness = float(np.mean(weighted_S) / (np.mean(S) + 1e-10))

    # Brightness (ratio of energy above 1500 Hz)
    freq_mask = freqs >= 1500.0
    high_energy = float(np.mean(S[freq_mask, :] ** 2))
    total_energy = float(np.mean(S ** 2))
    brightness = float(high_energy / max(total_energy, 1e-10))

    # Dynamic range (difference between loud and quiet sections in dB)
    rms_frames = librosa.feature.rms(y=y_mono, frame_length=n_fft,
                                      hop_length=hop_length)[0]
    rms_frames_db = 20.0 * np.log10(np.maximum(rms_frames, 1e-10))
    if len(rms_frames_db) >= 2:
        dynamic_range = float(np.percentile(rms_frames_db, 95) - np.percentile(rms_frames_db, 5))
    else:
        dynamic_range = 0.0

    return {
        "spectral": {
            "mfcc": mfcc_mean,
            "chroma": chroma_mean,
            "spectralCentroid": spectral_centroid,
            "spectralRolloff": spectral_rolloff,
            "spectralFlux": spectral_flux,
            "spectralSpread": spectral_spread,
            "spectralFlatness": spectral_flatness,
        },
        "temporal": {
            "rmsEnergy": rms_db,
            "peakAmplitude": peak_db,
            "crestFactor": crest_factor,
            "attackTime": attack_time_ms,
            "transientDensity": transient_density,
            "zeroCrossingRate": zcr_per_sec,
        },
        "perceptual": {
            "lufs": lufs_approx,
            "truePeakDb": true_peak_db,
            "roughness": roughness,
            "sharpness": sharpness,
            "brightness": brightness,
            "dynamicRange": dynamic_range,
        },
    }


def main():
    if len(sys.argv) < 2:
        print("Usage: python extract_real_features.py <dataset_dir>")
        sys.exit(1)

    dataset_dir = Path(sys.argv[1])
    if not dataset_dir.exists():
        print(f"ERROR: {dataset_dir} not found")
        sys.exit(1)

    sample_dirs = sorted([
        d for d in dataset_dir.iterdir()
        if d.is_dir() and d.name.startswith("sample_")
    ])

    print(f"Extracting real features from {len(sample_dirs)} samples...")

    for sample_dir in sample_dirs:
        audio_path = sample_dir / "audio.wav"
        features_path = sample_dir / "features.json"

        if not audio_path.exists():
            print(f"  SKIP {sample_dir.name}: no audio.wav")
            continue

        print(f"  {sample_dir.name}: ", end="", flush=True)
        features = extract_features(audio_path)

        with open(features_path, "w") as f:
            json.dump(features, f, indent=2)

        mfcc0 = features["spectral"]["mfcc"][0]
        centroid = features["spectral"]["spectralCentroid"]
        rms = features["temporal"]["rmsEnergy"]
        print(f"MFCC[0]={mfcc0:.1f}, centroid={centroid:.0f} Hz, RMS={rms:.1f} dB")

    print(f"\nDone. {len(sample_dirs)} features.json files written with real measurements.")


if __name__ == "__main__":
    main()
