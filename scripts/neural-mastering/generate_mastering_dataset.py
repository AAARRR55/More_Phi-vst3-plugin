#!/usr/bin/env python3
"""Generate aligned unmastered/mastered pairs for neural mastering research.

The script is intentionally offline-only. It creates sample-aligned stereo WAV
pairs plus a JSON manifest with provenance, loudness, feature, and augmentation
metadata. It supports synthetic mastering/demastering and explicit paired
manifests for licensed professional material.
"""

from __future__ import annotations

import argparse
import json
import math
import random
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Iterable

import torch
import torch.nn.functional as F
import torchaudio

try:  # Optional but preferred for real LUFS measurement.
    import pyloudnorm as pyln  # type: ignore
except Exception:  # pragma: no cover - optional dependency
    pyln = None


AUDIO_EXTENSIONS = {".wav", ".flac", ".aif", ".aiff", ".ogg", ".mp3"}


@dataclass(frozen=True)
class AugmentParams:
    input_gain_db: float
    input_low_db: float
    input_mid_db: float
    input_high_db: float
    input_transient: float
    input_width: float
    target_low_db: float
    target_mid_db: float
    target_high_db: float
    compressor_threshold_db: float
    compressor_ratio: float
    saturation_drive: float
    target_width: float
    limiter_ceiling_db: float
    target_lufs: float


def list_audio_files(root: Path) -> list[Path]:
    return sorted(path for path in root.rglob("*") if path.suffix.lower() in AUDIO_EXTENSIONS)


def load_audio(path: Path, sample_rate: int) -> torch.Tensor:
    waveform, sr = torchaudio.load(str(path))
    waveform = waveform.to(torch.float32)
    if waveform.numel() == 0:
        raise ValueError(f"{path} contains no samples")
    if sr != sample_rate:
        waveform = torchaudio.transforms.Resample(sr, sample_rate)(waveform)
    return to_stereo(waveform)


def to_stereo(waveform: torch.Tensor) -> torch.Tensor:
    if waveform.ndim != 2:
        raise ValueError(f"expected [channels, samples], got {tuple(waveform.shape)}")
    if waveform.shape[0] == 1:
        return waveform.repeat(2, 1)
    if waveform.shape[0] >= 2:
        return waveform[:2]
    raise ValueError("audio has zero channels")


def crop_or_pad(waveform: torch.Tensor, start: int, length: int) -> torch.Tensor:
    end = start + length
    if waveform.shape[-1] >= end:
        return waveform[:, start:end]
    cropped = waveform[:, start:]
    return F.pad(cropped, (0, length - cropped.shape[-1]))


def safe_peak_normalize(waveform: torch.Tensor, peak: float = 0.95) -> torch.Tensor:
    max_abs = waveform.abs().max().clamp_min(1.0e-8)
    if max_abs <= peak:
        return waveform
    return waveform * (peak / max_abs)


def gain_db(waveform: torch.Tensor, db: float) -> torch.Tensor:
    return waveform * (10.0 ** (db / 20.0))


def rms_db(waveform: torch.Tensor) -> float:
    rms = waveform.pow(2).mean().sqrt().clamp_min(1.0e-12)
    return float(20.0 * torch.log10(rms).item())


def measure_lufs(waveform: torch.Tensor, sample_rate: int) -> tuple[float, str]:
    if pyln is not None:
        meter = pyln.Meter(sample_rate)
        # pyloudnorm expects [samples, channels].
        value = meter.integrated_loudness(waveform.t().cpu().numpy())
        if math.isfinite(value):
            return float(value), "pyloudnorm"
    # Calibrated RMS proxy, not a standards-compliant LUFS measurement.
    return rms_db(waveform), "rms_proxy"


def normalize_loudness(waveform: torch.Tensor, sample_rate: int, target_lufs: float) -> tuple[torch.Tensor, float, str]:
    current, method = measure_lufs(waveform, sample_rate)
    if math.isfinite(current):
        waveform = gain_db(waveform, target_lufs - current)
    return safe_peak_normalize(waveform), current, method


def smooth_eq(waveform: torch.Tensor, sample_rate: int, low_db: float, mid_db: float, high_db: float) -> torch.Tensor:
    spectrum = torch.fft.rfft(waveform, dim=-1)
    freqs = torch.fft.rfftfreq(waveform.shape[-1], d=1.0 / sample_rate).to(waveform.device)
    anchors_hz = torch.tensor([20.0, 180.0, 1000.0, 6000.0, sample_rate / 2.0], device=waveform.device)
    anchors_db = torch.tensor([low_db, low_db, mid_db, high_db, high_db], device=waveform.device)
    log_freq = torch.log10(freqs.clamp_min(20.0))
    log_anchors = torch.log10(anchors_hz)
    curve = torch.empty_like(freqs)
    for i in range(len(log_anchors) - 1):
        mask = (log_freq >= log_anchors[i]) & (log_freq <= log_anchors[i + 1])
        span = (log_anchors[i + 1] - log_anchors[i]).clamp_min(1.0e-6)
        t = (log_freq[mask] - log_anchors[i]) / span
        curve[mask] = torch.lerp(anchors_db[i], anchors_db[i + 1], t)
    curve[log_freq < log_anchors[0]] = anchors_db[0]
    curve[log_freq > log_anchors[-1]] = anchors_db[-1]
    gain = (10.0 ** (curve / 20.0)).view(1, -1)
    return torch.fft.irfft(spectrum * gain, n=waveform.shape[-1], dim=-1)


def transient_shape(waveform: torch.Tensor, amount: float, window: int = 257) -> torch.Tensor:
    if abs(amount) < 1.0e-6:
        return waveform
    kernel = torch.ones((waveform.shape[0], 1, window), dtype=waveform.dtype, device=waveform.device) / window
    low = F.conv1d(waveform.unsqueeze(0), kernel, padding=window // 2, groups=waveform.shape[0]).squeeze(0)
    high = waveform - low[..., : waveform.shape[-1]]
    return safe_peak_normalize(waveform + amount * high, peak=0.98)


def stereo_width(waveform: torch.Tensor, width: float) -> torch.Tensor:
    left, right = waveform[0], waveform[1]
    mid = 0.5 * (left + right)
    side = 0.5 * (left - right) * width
    return torch.stack((mid + side, mid - side), dim=0)


def bus_compress(waveform: torch.Tensor, threshold_db: float, ratio: float, window: int = 2048) -> torch.Tensor:
    mono = waveform.mean(dim=0, keepdim=True)
    env = F.avg_pool1d(mono.abs().unsqueeze(0), kernel_size=window, stride=1, padding=window // 2).squeeze(0)
    env = env[..., : waveform.shape[-1]].clamp_min(1.0e-8)
    env_db = 20.0 * torch.log10(env)
    over = (env_db - threshold_db).clamp_min(0.0)
    gain_reduction_db = over * (1.0 - 1.0 / ratio)
    gain = 10.0 ** (-gain_reduction_db / 20.0)
    return waveform * gain


def saturate(waveform: torch.Tensor, drive: float) -> torch.Tensor:
    if drive <= 1.0:
        return waveform
    return torch.tanh(waveform * drive) / math.tanh(drive)


def limit_ceiling(waveform: torch.Tensor, ceiling_db: float) -> torch.Tensor:
    ceiling = 10.0 ** (ceiling_db / 20.0)
    peak = waveform.abs().max().clamp_min(1.0e-8)
    if peak > ceiling:
        waveform = waveform * (ceiling / peak)
    return waveform.clamp(-ceiling, ceiling)


def spectral_features(waveform: torch.Tensor, sample_rate: int) -> dict[str, float]:
    mono = waveform.mean(dim=0)
    spectrum = torch.fft.rfft(mono)
    mag = spectrum.abs().clamp_min(1.0e-12)
    freqs = torch.fft.rfftfreq(mono.numel(), d=1.0 / sample_rate)
    centroid = float((freqs * mag).sum().div(mag.sum()).item())
    nyquist = sample_rate / 2.0

    def band_energy(lo: float, hi: float) -> float:
        mask = (freqs >= lo) & (freqs < hi)
        return float(mag[mask].pow(2).mean().log10().mul(10.0).item()) if mask.any() else -120.0

    return {
        "spectralCentroidHz": centroid,
        "lowBandDb": band_energy(20.0, 250.0),
        "midBandDb": band_energy(250.0, 4000.0),
        "highBandDb": band_energy(4000.0, nyquist),
    }


def stereo_features(waveform: torch.Tensor) -> dict[str, float]:
    left, right = waveform[0], waveform[1]
    left_z = left - left.mean()
    right_z = right - right.mean()
    corr = (left_z * right_z).mean() / (left_z.std().clamp_min(1.0e-8) * right_z.std().clamp_min(1.0e-8))
    mid = 0.5 * (left + right)
    side = 0.5 * (left - right)
    width = side.pow(2).mean().sqrt() / mid.pow(2).mean().sqrt().clamp_min(1.0e-8)
    return {"stereoCorrelation": float(corr.clamp(-1.0, 1.0).item()), "midSideWidthRatio": float(width.item())}


def audio_features(waveform: torch.Tensor, sample_rate: int) -> dict[str, Any]:
    lufs, method = measure_lufs(waveform, sample_rate)
    peak = float(waveform.abs().max().item())
    rms = float(waveform.pow(2).mean().sqrt().item())
    crest = 20.0 * math.log10(max(peak, 1.0e-8) / max(rms, 1.0e-8))
    return {
        "lufs": lufs,
        "lufsMethod": method,
        "peak": peak,
        "rms": rms,
        "crestFactorDb": crest,
        **spectral_features(waveform, sample_rate),
        **stereo_features(waveform),
    }


def sample_params(rng: random.Random, target_lufs: float) -> AugmentParams:
    return AugmentParams(
        input_gain_db=rng.uniform(-6.0, 2.0),
        input_low_db=rng.uniform(-3.5, 3.5),
        input_mid_db=rng.uniform(-2.0, 2.0),
        input_high_db=rng.uniform(-4.0, 3.0),
        input_transient=rng.uniform(-0.35, 0.35),
        input_width=rng.uniform(0.65, 1.35),
        target_low_db=rng.uniform(-1.5, 1.5),
        target_mid_db=rng.uniform(-1.0, 1.0),
        target_high_db=rng.uniform(-0.5, 2.0),
        compressor_threshold_db=rng.uniform(-24.0, -14.0),
        compressor_ratio=rng.uniform(1.4, 2.6),
        saturation_drive=rng.uniform(1.0, 1.7),
        target_width=rng.uniform(0.85, 1.15),
        limiter_ceiling_db=rng.uniform(-1.2, -0.8),
        target_lufs=target_lufs + rng.uniform(-1.0, 1.0),
    )


def assign_split(rng: random.Random, train_ratio: float, val_ratio: float) -> str:
    value = rng.random()
    if value < train_ratio:
        return "train"
    if value < train_ratio + val_ratio:
        return "val"
    return "test"


def make_pair(source: torch.Tensor, sample_rate: int, params: AugmentParams, mode: str) -> tuple[torch.Tensor, torch.Tensor]:
    source = safe_peak_normalize(source)
    if mode == "demaster-reference":
        target, _, _ = normalize_loudness(source, sample_rate, params.target_lufs)
        unmastered = gain_db(target, params.input_gain_db)
        unmastered = smooth_eq(unmastered, sample_rate, params.input_low_db, params.input_mid_db, params.input_high_db)
        unmastered = transient_shape(unmastered, params.input_transient)
        unmastered = stereo_width(unmastered, params.input_width)
        return safe_peak_normalize(unmastered), limit_ceiling(target, params.limiter_ceiling_db)

    unmastered = gain_db(source, params.input_gain_db)
    unmastered = smooth_eq(unmastered, sample_rate, params.input_low_db, params.input_mid_db, params.input_high_db)
    unmastered = transient_shape(unmastered, params.input_transient)
    unmastered = stereo_width(unmastered, params.input_width)
    unmastered = safe_peak_normalize(unmastered)

    mastered = smooth_eq(unmastered, sample_rate, params.target_low_db, params.target_mid_db, params.target_high_db)
    mastered = bus_compress(mastered, params.compressor_threshold_db, params.compressor_ratio)
    mastered = saturate(mastered, params.saturation_drive)
    mastered = stereo_width(mastered, params.target_width)
    mastered, _, _ = normalize_loudness(mastered, sample_rate, params.target_lufs)
    mastered = limit_ceiling(mastered, params.limiter_ceiling_db)
    return unmastered, mastered


def read_paired_manifest(path: Path) -> Iterable[dict[str, Any]]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    items = payload.get("items", payload if isinstance(payload, list) else [])
    if not isinstance(items, list):
        raise ValueError("paired manifest must be a list or an object with an items list")
    return items


def resolve_manifest_path(base: Path, value: Any) -> Path:
    if value is None:
        raise ValueError("manifest item is missing an audio path")
    path = Path(str(value))
    return path if path.is_absolute() else base / path


def write_pair(input_audio: torch.Tensor, target_audio: torch.Tensor, out_dir: Path, pair_id: str, sample_rate: int) -> tuple[str, str]:
    input_rel = Path("input") / f"{pair_id}.wav"
    target_rel = Path("target") / f"{pair_id}.wav"
    input_path = out_dir / input_rel
    target_path = out_dir / target_rel
    input_path.parent.mkdir(parents=True, exist_ok=True)
    target_path.parent.mkdir(parents=True, exist_ok=True)
    torchaudio.save(str(input_path), input_audio.cpu(), sample_rate)
    torchaudio.save(str(target_path), target_audio.cpu(), sample_rate)
    return input_rel.as_posix(), target_rel.as_posix()


def generate_from_source_dir(args: argparse.Namespace) -> list[dict[str, Any]]:
    rng = random.Random(args.seed)
    files = list_audio_files(args.source_dir)
    if not files:
        raise SystemExit(f"no audio files found under {args.source_dir}")
    split_rng = random.Random(args.seed + 17)
    source_splits = {path: assign_split(split_rng, args.train_ratio, args.val_ratio) for path in files}

    segment_samples = int(round(args.segment_seconds * args.sample_rate))
    items: list[dict[str, Any]] = []
    for file_index, path in enumerate(files):
        waveform = load_audio(path, args.sample_rate)
        max_start = max(0, waveform.shape[-1] - segment_samples)
        for pair_index in range(args.pairs_per_file):
            start = rng.randint(0, max_start) if max_start else 0
            segment = crop_or_pad(waveform, start, segment_samples)
            params = sample_params(rng, args.target_lufs)
            unmastered, mastered = make_pair(segment, args.sample_rate, params, args.mode)
            pair_id = f"{path.stem}_{file_index:04d}_{pair_index:04d}"
            input_rel, target_rel = write_pair(unmastered, mastered, args.output_dir, pair_id, args.sample_rate)
            item = {
                "id": pair_id,
                "split": source_splits[path],
                "sourceId": path.stem,
                "sourcePath": str(path),
                "inputPath": input_rel,
                "targetPath": target_rel,
                "pairingMode": args.mode,
                "sampleRate": args.sample_rate,
                "segmentSamples": segment_samples,
                "startSample": start,
                "provenanceComplete": bool(args.provenance_complete),
                "licenseStatus": args.license_status,
                "referenceQuality": "synthetic" if args.mode != "paired" else "reviewed",
                "unsupportedMaterial": False,
                "augmentation": asdict(params),
                "inputFeatures": audio_features(unmastered, args.sample_rate),
                "targetFeatures": audio_features(mastered, args.sample_rate),
            }
            items.append(item)
    return items


def generate_from_paired_manifest(args: argparse.Namespace) -> list[dict[str, Any]]:
    rng = random.Random(args.seed)
    split_rng = random.Random(args.seed + 23)
    segment_samples = int(round(args.segment_seconds * args.sample_rate))
    items: list[dict[str, Any]] = []
    base = args.paired_manifest.parent
    source_splits: dict[str, str] = {}
    for index, source_item in enumerate(read_paired_manifest(args.paired_manifest)):
        input_path = resolve_manifest_path(base, source_item.get("inputPath") or source_item.get("unmasteredPath"))
        target_path = resolve_manifest_path(base, source_item.get("targetPath") or source_item.get("masteredPath"))
        if not input_path.exists() or not target_path.exists():
            raise FileNotFoundError(f"paired item {index} has missing audio path")
        input_audio = load_audio(input_path, args.sample_rate)
        target_audio = load_audio(target_path, args.sample_rate)
        aligned_len = min(input_audio.shape[-1], target_audio.shape[-1])
        input_audio = input_audio[:, :aligned_len]
        target_audio = target_audio[:, :aligned_len]
        max_start = max(0, aligned_len - segment_samples)
        source_id = str(source_item.get("sourceId") or source_item.get("id") or f"paired_{index:04d}")
        split = str(source_item.get("split") or source_splits.setdefault(source_id, assign_split(split_rng, args.train_ratio, args.val_ratio)))
        for pair_index in range(args.pairs_per_item):
            start = rng.randint(0, max_start) if max_start else 0
            unmastered = crop_or_pad(input_audio, start, segment_samples)
            mastered = crop_or_pad(target_audio, start, segment_samples)
            unmastered = safe_peak_normalize(unmastered)
            mastered, _, _ = normalize_loudness(mastered, args.sample_rate, args.target_lufs)
            pair_id = f"{source_item.get('id', f'paired_{index:04d}')}_{pair_index:04d}"
            input_rel, target_rel = write_pair(unmastered, mastered, args.output_dir, pair_id, args.sample_rate)
            item = {
                **source_item,
                "id": pair_id,
                "split": split,
                "sourceId": source_id,
                "inputPath": input_rel,
                "targetPath": target_rel,
                "pairingMode": "paired",
                "sampleRate": args.sample_rate,
                "segmentSamples": segment_samples,
                "startSample": start,
                "provenanceComplete": source_item.get("provenanceComplete", False),
                "licenseStatus": source_item.get("licenseStatus", "unknown"),
                "referenceQuality": source_item.get("referenceQuality", "unreviewed"),
                "unsupportedMaterial": source_item.get("unsupportedMaterial", False),
                "inputFeatures": audio_features(unmastered, args.sample_rate),
                "targetFeatures": audio_features(mastered, args.sample_rate),
            }
            items.append(item)
    return items


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", type=Path, help="Directory of clean/unmastered or reference audio")
    parser.add_argument("--paired-manifest", type=Path, help="Manifest containing explicit unmastered/mastered pairs")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--mode", choices=["synthesize-mastered", "demaster-reference"], default="synthesize-mastered")
    parser.add_argument("--sample-rate", type=int, default=48000)
    parser.add_argument("--segment-seconds", type=float, default=5.46)
    parser.add_argument("--pairs-per-file", type=int, default=4)
    parser.add_argument("--pairs-per-item", type=int, default=2)
    parser.add_argument("--target-lufs", type=float, default=-14.0)
    parser.add_argument("--train-ratio", type=float, default=0.8)
    parser.add_argument("--val-ratio", type=float, default=0.1)
    parser.add_argument("--seed", type=int, default=1337)
    parser.add_argument("--license-status", default="approved")
    parser.add_argument("--provenance-complete", action="store_true")
    args = parser.parse_args()

    if bool(args.source_dir) == bool(args.paired_manifest):
        raise SystemExit("provide exactly one of --source-dir or --paired-manifest")
    if args.train_ratio <= 0.0 or args.val_ratio < 0.0 or args.train_ratio + args.val_ratio >= 1.0:
        raise SystemExit("--train-ratio must be > 0 and train+val must be < 1")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    if args.paired_manifest:
        items = generate_from_paired_manifest(args)
    else:
        items = generate_from_source_dir(args)

    manifest = {
        "schemaVersion": 1,
        "createdBy": "generate_mastering_dataset.py",
        "sampleRate": args.sample_rate,
        "segmentSeconds": args.segment_seconds,
        "segmentSamples": int(round(args.segment_seconds * args.sample_rate)),
        "lufsDependency": "pyloudnorm" if pyln is not None else "rms_proxy",
        "items": items,
    }
    manifest_path = args.output_dir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({"manifest": str(manifest_path), "items": len(items)}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
