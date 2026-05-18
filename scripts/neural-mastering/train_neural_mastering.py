#!/usr/bin/env python3
"""Train a stereo neural mastering model from aligned audio pairs.

The script implements a hybrid waveform CNN-Transformer, multi-resolution STFT
losses, optional VGG-style spectrogram feature matching, mixed precision,
warmup-cosine learning-rate scheduling, high-throughput DataLoader settings,
and perceptual validation hooks for external PEAQ/VISQOL tools.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import random
import shlex
import subprocess
import tempfile
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Iterable

import torch
import torch.nn as nn
import torch.nn.functional as F
import torchaudio
from torch.utils.data import DataLoader, Dataset


def set_seed(seed: int) -> None:
    random.seed(seed)
    torch.manual_seed(seed)
    torch.cuda.manual_seed_all(seed)


def resolve_audio_path(base: Path, value: str) -> Path:
    path = Path(value)
    return path if path.is_absolute() else base / path


def to_stereo(waveform: torch.Tensor) -> torch.Tensor:
    if waveform.ndim != 2:
        raise ValueError(f"expected [channels, samples], got {tuple(waveform.shape)}")
    if waveform.shape[0] == 1:
        return waveform.repeat(2, 1)
    if waveform.shape[0] >= 2:
        return waveform[:2]
    raise ValueError("audio has zero channels")


def peak_normalize(waveform: torch.Tensor, peak: float = 0.98) -> torch.Tensor:
    max_abs = waveform.abs().max().clamp_min(1.0e-8)
    return waveform * torch.minimum(waveform.new_tensor(1.0), waveform.new_tensor(peak) / max_abs)


def load_audio(path: Path, sample_rate: int) -> torch.Tensor:
    waveform, sr = torchaudio.load(str(path))
    waveform = to_stereo(waveform.to(torch.float32))
    if sr != sample_rate:
        waveform = torchaudio.transforms.Resample(sr, sample_rate)(waveform)
    return peak_normalize(waveform)


def crop_pair(
    input_audio: torch.Tensor,
    target_audio: torch.Tensor,
    segment_samples: int,
    training: bool,
    rng: random.Random,
) -> tuple[torch.Tensor, torch.Tensor]:
    length = min(input_audio.shape[-1], target_audio.shape[-1])
    input_audio = input_audio[:, :length]
    target_audio = target_audio[:, :length]

    if length >= segment_samples:
        max_start = length - segment_samples
        start = rng.randint(0, max_start) if training and max_start else max_start // 2
        return input_audio[:, start : start + segment_samples], target_audio[:, start : start + segment_samples]

    pad = segment_samples - length
    return F.pad(input_audio, (0, pad)), F.pad(target_audio, (0, pad))


def rms_db(waveform: torch.Tensor) -> torch.Tensor:
    rms = waveform.pow(2).mean(dim=(-1, -2)).sqrt().clamp_min(1.0e-8)
    return 20.0 * torch.log10(rms)


def stereo_correlation(waveform: torch.Tensor) -> torch.Tensor:
    left = waveform[:, 0] - waveform[:, 0].mean(dim=-1, keepdim=True)
    right = waveform[:, 1] - waveform[:, 1].mean(dim=-1, keepdim=True)
    denom = left.std(dim=-1).clamp_min(1.0e-8) * right.std(dim=-1).clamp_min(1.0e-8)
    return ((left * right).mean(dim=-1) / denom).clamp(-1.0, 1.0)


def group_count(channels: int, preferred: int = 8) -> int:
    for groups in range(min(preferred, channels), 0, -1):
        if channels % groups == 0:
            return groups
    return 1


def mid_side(waveform: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
    mid = 0.5 * (waveform[:, 0:1] + waveform[:, 1:2])
    side = 0.5 * (waveform[:, 0:1] - waveform[:, 1:2])
    return mid, side


class AudioPairDataset(Dataset[tuple[torch.Tensor, torch.Tensor]]):
    def __init__(
        self,
        manifest: Path,
        split: str,
        sample_rate: int,
        segment_samples: int,
        seed: int,
        training: bool,
        input_gain_jitter_db: float = 0.0,
    ) -> None:
        payload = json.loads(manifest.read_text(encoding="utf-8"))
        raw_items = payload.get("items", payload if isinstance(payload, list) else [])
        if not isinstance(raw_items, list):
            raise ValueError("manifest must be a list or an object with an items list")
        self.base = manifest.parent
        self.items = [item for item in raw_items if item.get("split", split) == split]
        if not self.items:
            raise ValueError(f"manifest contains no items for split '{split}'")
        self.sample_rate = sample_rate
        self.segment_samples = segment_samples
        self.training = training
        self.input_gain_jitter_db = input_gain_jitter_db
        self.rng = random.Random(seed + (0 if training else 10_000))

    def __len__(self) -> int:
        return len(self.items)

    def __getitem__(self, index: int) -> tuple[torch.Tensor, torch.Tensor]:
        item = self.items[index]
        input_value = item.get("inputPath") or item.get("unmasteredPath")
        target_value = item.get("targetPath") or item.get("masteredPath")
        if not input_value or not target_value:
            raise KeyError(f"item {item.get('id', index)} missing inputPath/targetPath")
        input_audio = load_audio(resolve_audio_path(self.base, str(input_value)), self.sample_rate)
        target_audio = load_audio(resolve_audio_path(self.base, str(target_value)), self.sample_rate)
        input_audio, target_audio = crop_pair(
            input_audio,
            target_audio,
            self.segment_samples,
            self.training,
            self.rng,
        )
        if self.training and self.input_gain_jitter_db > 0.0:
            jitter = self.rng.uniform(-self.input_gain_jitter_db, self.input_gain_jitter_db)
            input_audio = peak_normalize(input_audio * (10.0 ** (jitter / 20.0)))
        return input_audio.contiguous(), target_audio.contiguous()


class GatedResidualBlock(nn.Module):
    def __init__(self, channels: int, kernel_size: int = 9) -> None:
        super().__init__()
        padding = kernel_size // 2
        self.depthwise = nn.Conv1d(channels, channels * 2, kernel_size, padding=padding, groups=channels)
        self.pointwise = nn.Conv1d(channels, channels, 1)
        self.norm = nn.GroupNorm(num_groups=group_count(channels), num_channels=channels)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        value, gate = self.depthwise(x).chunk(2, dim=1)
        y = value * torch.sigmoid(gate)
        y = self.pointwise(y)
        return x + F.silu(self.norm(y))


class EncoderBlock(nn.Module):
    def __init__(self, in_channels: int, out_channels: int, stride: int) -> None:
        super().__init__()
        self.conv = nn.Conv1d(in_channels, out_channels, kernel_size=9, stride=stride, padding=4)
        self.skip = nn.Conv1d(in_channels, out_channels, kernel_size=1, stride=stride)
        self.norm = nn.GroupNorm(num_groups=group_count(out_channels), num_channels=out_channels)
        self.residual = GatedResidualBlock(out_channels)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        y = F.silu(self.norm(self.conv(x) + self.skip(x)))
        return self.residual(y)


class DecoderBlock(nn.Module):
    def __init__(self, in_channels: int, skip_channels: int, out_channels: int) -> None:
        super().__init__()
        self.conv = nn.Conv1d(in_channels + skip_channels, out_channels, kernel_size=7, padding=3)
        self.norm = nn.GroupNorm(num_groups=group_count(out_channels), num_channels=out_channels)
        self.residual = GatedResidualBlock(out_channels)

    def forward(self, x: torch.Tensor, skip: torch.Tensor) -> torch.Tensor:
        x = F.interpolate(x, size=skip.shape[-1], mode="linear", align_corners=False)
        y = torch.cat((x, skip), dim=1)
        y = F.silu(self.norm(self.conv(y)))
        return self.residual(y)


class SinusoidalPositionEncoding(nn.Module):
    def __init__(self, channels: int, max_len: int = 4096) -> None:
        super().__init__()
        position = torch.arange(max_len).float().unsqueeze(1)
        div_term = torch.exp(torch.arange(0, channels, 2).float() * (-math.log(10000.0) / channels))
        pe = torch.zeros(max_len, channels)
        pe[:, 0::2] = torch.sin(position * div_term)
        pe[:, 1::2] = torch.cos(position * div_term[: pe[:, 1::2].shape[1]])
        self.register_buffer("pe", pe.unsqueeze(0), persistent=False)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        if x.shape[1] > self.pe.shape[1]:
            raise ValueError(f"sequence length {x.shape[1]} exceeds positional capacity {self.pe.shape[1]}")
        return x + self.pe[:, : x.shape[1]].to(dtype=x.dtype)


class HybridMasteringNet(nn.Module):
    def __init__(
        self,
        widths: tuple[int, ...] = (64, 128, 192, 256, 384),
        strides: tuple[int, ...] = (4, 4, 4, 4, 4),
        transformer_layers: int = 6,
        transformer_heads: int = 8,
        residual_scale: float = 0.25,
    ) -> None:
        super().__init__()
        if len(widths) != len(strides):
            raise ValueError("widths and strides must have equal length")
        self.residual_scale = residual_scale
        self.stem = nn.Sequential(
            nn.Conv1d(4, widths[0], kernel_size=7, padding=3),
            nn.GroupNorm(num_groups=group_count(widths[0]), num_channels=widths[0]),
            nn.SiLU(),
            GatedResidualBlock(widths[0]),
        )

        encoders: list[nn.Module] = []
        in_channels = widths[0]
        for width, stride in zip(widths, strides):
            encoders.append(EncoderBlock(in_channels, width, stride))
            in_channels = width
        self.encoders = nn.ModuleList(encoders)

        bottleneck_dim = widths[-1]
        self.position = SinusoidalPositionEncoding(bottleneck_dim)
        layer = nn.TransformerEncoderLayer(
            d_model=bottleneck_dim,
            nhead=transformer_heads,
            dim_feedforward=bottleneck_dim * 4,
            dropout=0.0,
            activation="gelu",
            batch_first=True,
            norm_first=True,
        )
        self.transformer = nn.TransformerEncoder(layer, num_layers=transformer_layers)

        decoders: list[nn.Module] = []
        reversed_widths = tuple(reversed(widths))
        current = reversed_widths[0]
        for skip_channels, out_channels in zip(reversed_widths[1:], reversed_widths[1:]):
            decoders.append(DecoderBlock(current, skip_channels, out_channels))
            current = out_channels
        self.decoders = nn.ModuleList(decoders)
        self.head = nn.Sequential(
            nn.Conv1d(current, current, kernel_size=7, padding=3),
            nn.SiLU(),
            nn.Conv1d(current, 2, kernel_size=7, padding=3),
        )

    def forward(self, waveform: torch.Tensor) -> torch.Tensor:
        mid, side = mid_side(waveform)
        x = torch.cat((waveform, mid, side), dim=1)
        x = self.stem(x)
        skips: list[torch.Tensor] = []
        for encoder in self.encoders:
            x = encoder(x)
            skips.append(x)

        x = x.transpose(1, 2)
        x = self.transformer(self.position(x))
        x = x.transpose(1, 2)

        decoder_skips = list(reversed(skips[:-1]))
        for decoder, skip in zip(self.decoders, decoder_skips):
            x = decoder(x, skip)
        x = F.interpolate(x, size=waveform.shape[-1], mode="linear", align_corners=False)
        delta = torch.tanh(self.head(x))
        return torch.tanh(waveform + self.residual_scale * delta)


class MultiResolutionSTFTLoss(nn.Module):
    def __init__(self, fft_sizes: tuple[int, ...] = (1024, 2048, 4096, 8192)) -> None:
        super().__init__()
        self.fft_sizes = fft_sizes

    def forward(self, prediction: torch.Tensor, target: torch.Tensor) -> tuple[torch.Tensor, dict[str, float]]:
        total_sc = prediction.new_tensor(0.0)
        total_mag = prediction.new_tensor(0.0)
        pred = prediction.float().flatten(0, 1)
        ref = target.float().flatten(0, 1)
        for fft_size in self.fft_sizes:
            hop = fft_size // 4
            window = torch.hann_window(fft_size, device=prediction.device, dtype=torch.float32)
            pred_spec = torch.stft(pred, fft_size, hop_length=hop, win_length=fft_size, window=window, return_complex=True)
            ref_spec = torch.stft(ref, fft_size, hop_length=hop, win_length=fft_size, window=window, return_complex=True)
            pred_mag = pred_spec.abs().clamp_min(1.0e-7)
            ref_mag = ref_spec.abs().clamp_min(1.0e-7)
            total_sc = total_sc + torch.linalg.vector_norm(ref_mag - pred_mag) / torch.linalg.vector_norm(ref_mag).clamp_min(1.0e-7)
            total_mag = total_mag + F.l1_loss(torch.log(pred_mag), torch.log(ref_mag))
        denom = float(len(self.fft_sizes))
        loss = total_sc / denom + total_mag / denom
        return loss, {"stft_spectral_convergence": float((total_sc / denom).detach()), "stft_log_mag": float((total_mag / denom).detach())}


class SpectralVGGFeatureNet(nn.Module):
    """Small spectro-temporal CNN for frozen feature matching.

    This network must be loaded from a trained checkpoint before use. The
    training script rejects positive feature-loss weight without a checkpoint.
    """

    def __init__(self, channels: tuple[int, ...] = (16, 32, 64, 128)) -> None:
        super().__init__()
        layers: list[nn.Module] = []
        in_channels = 1
        for channel in channels:
            layers.extend(
                [
                    nn.Conv2d(in_channels, channel, kernel_size=3, padding=1),
                    nn.GroupNorm(num_groups=group_count(channel), num_channels=channel),
                    nn.SiLU(),
                    nn.Conv2d(channel, channel, kernel_size=3, padding=1),
                    nn.GroupNorm(num_groups=group_count(channel), num_channels=channel),
                    nn.SiLU(),
                    nn.AvgPool2d(kernel_size=2),
                ]
            )
            in_channels = channel
        self.layers = nn.ModuleList(layers)

    def forward(self, spec_image: torch.Tensor) -> list[torch.Tensor]:
        features: list[torch.Tensor] = []
        x = spec_image
        for layer in self.layers:
            x = layer(x)
            if isinstance(layer, nn.AvgPool2d):
                features.append(x)
        return features


class FeatureMatchingLoss(nn.Module):
    def __init__(self, checkpoint: Path, fft_size: int = 2048) -> None:
        super().__init__()
        self.fft_size = fft_size
        self.net = SpectralVGGFeatureNet()
        payload = torch.load(checkpoint, map_location="cpu")
        state = payload.get("model", payload)
        self.net.load_state_dict(state, strict=True)
        self.net.eval()
        for parameter in self.net.parameters():
            parameter.requires_grad_(False)

    def _image(self, waveform: torch.Tensor) -> torch.Tensor:
        mono = waveform.float().mean(dim=1)
        window = torch.hann_window(self.fft_size, device=waveform.device, dtype=torch.float32)
        spec = torch.stft(mono, self.fft_size, hop_length=self.fft_size // 4, window=window, return_complex=True)
        log_mag = torch.log1p(spec.abs())
        return log_mag.unsqueeze(1)

    def forward(self, prediction: torch.Tensor, target: torch.Tensor) -> torch.Tensor:
        self.net.to(prediction.device)
        pred_features = self.net(self._image(prediction))
        target_features = self.net(self._image(target).detach())
        loss = prediction.new_tensor(0.0)
        for pred, ref in zip(pred_features, target_features):
            loss = loss + F.l1_loss(pred, ref.detach())
        return loss / max(1, len(pred_features))


class MasteringLoss(nn.Module):
    def __init__(
        self,
        stft_weight: float,
        waveform_weight: float,
        mid_side_weight: float,
        transient_weight: float,
        loudness_weight: float,
        stereo_weight: float,
        feature_weight: float,
        feature_checkpoint: Path | None,
    ) -> None:
        super().__init__()
        self.stft_weight = stft_weight
        self.waveform_weight = waveform_weight
        self.mid_side_weight = mid_side_weight
        self.transient_weight = transient_weight
        self.loudness_weight = loudness_weight
        self.stereo_weight = stereo_weight
        self.feature_weight = feature_weight
        self.stft = MultiResolutionSTFTLoss()
        if feature_weight > 0.0:
            if feature_checkpoint is None:
                raise ValueError("--feature-checkpoint is required when --feature-loss-weight > 0")
            self.feature = FeatureMatchingLoss(feature_checkpoint)
        else:
            self.feature = None

    def forward(self, prediction: torch.Tensor, target: torch.Tensor) -> tuple[torch.Tensor, dict[str, float]]:
        prediction = prediction.float()
        target = target.float()
        loss = prediction.new_tensor(0.0)
        metrics: dict[str, float] = {}
        stft_loss, stft_metrics = self.stft(prediction, target)
        loss = loss + self.stft_weight * stft_loss
        metrics.update(stft_metrics)

        waveform_l1 = F.l1_loss(prediction, target)
        loss = loss + self.waveform_weight * waveform_l1
        metrics["waveform_l1"] = float(waveform_l1.detach())

        pred_mid, pred_side = mid_side(prediction)
        target_mid, target_side = mid_side(target)
        ms_loss = F.l1_loss(pred_mid, target_mid) + F.l1_loss(pred_side, target_side)
        loss = loss + self.mid_side_weight * ms_loss
        metrics["mid_side_l1"] = float(ms_loss.detach())

        transient = F.l1_loss(prediction.diff(dim=-1), target.diff(dim=-1))
        loss = loss + self.transient_weight * transient
        metrics["transient_l1"] = float(transient.detach())

        loudness = F.l1_loss(rms_db(prediction), rms_db(target))
        loss = loss + self.loudness_weight * loudness
        metrics["loudness_db_error"] = float(loudness.detach())

        corr_error = F.l1_loss(stereo_correlation(prediction), stereo_correlation(target))
        pred_width = pred_side.pow(2).mean(dim=(-1, -2)).sqrt() / pred_mid.pow(2).mean(dim=(-1, -2)).sqrt().clamp_min(1.0e-8)
        target_width = target_side.pow(2).mean(dim=(-1, -2)).sqrt() / target_mid.pow(2).mean(dim=(-1, -2)).sqrt().clamp_min(1.0e-8)
        width_error = F.l1_loss(pred_width, target_width)
        stereo_loss = corr_error + width_error
        loss = loss + self.stereo_weight * stereo_loss
        metrics["stereo_correlation_error"] = float(corr_error.detach())
        metrics["mid_side_width_error"] = float(width_error.detach())

        if self.feature is not None:
            feature_loss = self.feature(prediction, target)
            loss = loss + self.feature_weight * feature_loss
            metrics["feature_matching"] = float(feature_loss.detach())

        metrics["loss"] = float(loss.detach())
        return loss, metrics


def si_sdr(prediction: torch.Tensor, target: torch.Tensor) -> torch.Tensor:
    pred = prediction.float().flatten(1)
    ref = target.float().flatten(1)
    pred = pred - pred.mean(dim=1, keepdim=True)
    ref = ref - ref.mean(dim=1, keepdim=True)
    scale = (pred * ref).sum(dim=1, keepdim=True) / ref.pow(2).sum(dim=1, keepdim=True).clamp_min(1.0e-8)
    signal = scale * ref
    noise = pred - signal
    return 10.0 * torch.log10(signal.pow(2).sum(dim=1).clamp_min(1.0e-8) / noise.pow(2).sum(dim=1).clamp_min(1.0e-8))


def log_spectral_distance(prediction: torch.Tensor, target: torch.Tensor, fft_size: int = 2048) -> torch.Tensor:
    pred = prediction.float().flatten(0, 1)
    ref = target.float().flatten(0, 1)
    window = torch.hann_window(fft_size, device=prediction.device, dtype=torch.float32)
    pred_mag = torch.stft(pred, fft_size, hop_length=fft_size // 4, window=window, return_complex=True).abs().clamp_min(1.0e-7)
    ref_mag = torch.stft(ref, fft_size, hop_length=fft_size // 4, window=window, return_complex=True).abs().clamp_min(1.0e-7)
    return (20.0 * (torch.log10(pred_mag) - torch.log10(ref_mag))).pow(2).mean().sqrt()


@dataclass
class MetricAverages:
    values: dict[str, float]
    counts: dict[str, int]

    @classmethod
    def create(cls) -> "MetricAverages":
        return cls(values={}, counts={})

    def update(self, metrics: dict[str, float], n: int = 1) -> None:
        for key, value in metrics.items():
            if math.isfinite(float(value)):
                self.values[key] = self.values.get(key, 0.0) + float(value) * n
                self.counts[key] = self.counts.get(key, 0) + n

    def mean(self) -> dict[str, float]:
        return {key: self.values[key] / max(1, self.counts[key]) for key in sorted(self.values)}


class ExternalPerceptualMetrics:
    def __init__(self, visqol_command: str | None, peaq_command: str | None, sample_rate: int, max_samples: int) -> None:
        self.visqol_command = visqol_command
        self.peaq_command = peaq_command
        self.sample_rate = sample_rate
        self.max_samples = max_samples

    def enabled(self) -> bool:
        return bool(self.visqol_command or self.peaq_command) and self.max_samples > 0

    def evaluate(self, prediction: torch.Tensor, target: torch.Tensor) -> dict[str, float]:
        if not self.enabled():
            return {}
        prediction = prediction.detach().cpu()
        target = target.detach().cpu()
        count = min(self.max_samples, prediction.shape[0])
        metrics = MetricAverages.create()
        with tempfile.TemporaryDirectory(prefix="neural_mastering_metrics_") as temp:
            temp_dir = Path(temp)
            for index in range(count):
                ref_path = temp_dir / f"ref_{index}.wav"
                deg_path = temp_dir / f"deg_{index}.wav"
                torchaudio.save(str(ref_path), target[index], self.sample_rate)
                torchaudio.save(str(deg_path), prediction[index], self.sample_rate)
                if self.visqol_command:
                    value = self._run_metric(self.visqol_command, ref_path, deg_path, temp_dir / f"visqol_{index}.csv")
                    if value is not None:
                        metrics.update({"visqol": value})
                if self.peaq_command:
                    value = self._run_metric(self.peaq_command, ref_path, deg_path, temp_dir / f"peaq_{index}.csv")
                    if value is not None:
                        metrics.update({"peaq": value})
        return metrics.mean()

    def _run_metric(self, template: str, ref_path: Path, deg_path: Path, out_path: Path) -> float | None:
        command = template.format(ref=str(ref_path), deg=str(deg_path), out=str(out_path))
        try:
            completed = subprocess.run(
                shlex.split(command, posix=os.name != "nt"),
                check=False,
                capture_output=True,
                text=True,
                timeout=120,
            )
        except (OSError, subprocess.TimeoutExpired):
            return None
        if completed.returncode != 0:
            return None
        if out_path.exists():
            parsed = self._parse_first_numeric_csv(out_path)
            if parsed is not None:
                return parsed
        return self._parse_first_float(completed.stdout + "\n" + completed.stderr)

    @staticmethod
    def _parse_first_numeric_csv(path: Path) -> float | None:
        try:
            with path.open("r", encoding="utf-8", newline="") as handle:
                for row in csv.reader(handle):
                    for cell in row:
                        try:
                            return float(cell)
                        except ValueError:
                            continue
        except OSError:
            return None
        return None

    @staticmethod
    def _parse_first_float(text: str) -> float | None:
        for token in text.replace(",", " ").split():
            try:
                return float(token)
            except ValueError:
                continue
        return None


class WarmupCosineScheduler(torch.optim.lr_scheduler.LambdaLR):
    def __init__(self, optimizer: torch.optim.Optimizer, warmup_steps: int, total_steps: int, min_lr_ratio: float) -> None:
        self.warmup_steps = max(1, warmup_steps)
        self.total_steps = max(self.warmup_steps + 1, total_steps)
        self.min_lr_ratio = min_lr_ratio
        super().__init__(optimizer, self._lr_lambda)

    def _lr_lambda(self, step: int) -> float:
        if step < self.warmup_steps:
            return float(step + 1) / float(self.warmup_steps)
        progress = (step - self.warmup_steps) / float(max(1, self.total_steps - self.warmup_steps))
        cosine = 0.5 * (1.0 + math.cos(math.pi * min(1.0, progress)))
        return self.min_lr_ratio + (1.0 - self.min_lr_ratio) * cosine


def make_grad_scaler(device_type: str, enabled: bool) -> torch.amp.GradScaler:
    try:
        return torch.amp.GradScaler(device_type, enabled=enabled)
    except TypeError:  # Older PyTorch accepts enabled but not device_type.
        return torch.amp.GradScaler(enabled=enabled)


def autocast_dtype(precision: str) -> torch.dtype:
    if precision == "fp16":
        return torch.float16
    if precision == "bf16":
        return torch.bfloat16
    return torch.float32


def save_checkpoint(
    path: Path,
    model: nn.Module,
    optimizer: torch.optim.Optimizer,
    scheduler: torch.optim.lr_scheduler.LRScheduler,
    epoch: int,
    global_step: int,
    best_score: float,
    args: argparse.Namespace,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    torch.save(
        {
            "model": model.state_dict(),
            "optimizer": optimizer.state_dict(),
            "scheduler": scheduler.state_dict(),
            "epoch": epoch,
            "globalStep": global_step,
            "bestScore": best_score,
            "args": vars(args),
        },
        path,
    )


def load_checkpoint(
    path: Path,
    model: nn.Module,
    optimizer: torch.optim.Optimizer,
    scheduler: torch.optim.lr_scheduler.LRScheduler,
    device: torch.device,
) -> tuple[int, int, float]:
    payload = torch.load(path, map_location=device)
    model.load_state_dict(payload["model"])
    optimizer.load_state_dict(payload["optimizer"])
    scheduler.load_state_dict(payload["scheduler"])
    return int(payload.get("epoch", 0)) + 1, int(payload.get("globalStep", 0)), float(payload.get("bestScore", math.inf))


def train_one_epoch(
    model: nn.Module,
    loader: DataLoader[tuple[torch.Tensor, torch.Tensor]],
    criterion: MasteringLoss,
    optimizer: torch.optim.Optimizer,
    scheduler: torch.optim.lr_scheduler.LRScheduler,
    scaler: torch.amp.GradScaler,
    device: torch.device,
    precision: str,
    grad_accum_steps: int,
    grad_clip_norm: float,
) -> tuple[dict[str, float], int]:
    model.train()
    averages = MetricAverages.create()
    optimizer.zero_grad(set_to_none=True)
    local_steps = 0
    amp_enabled = precision != "fp32"
    amp_dtype = autocast_dtype(precision)
    for batch_index, (inputs, targets) in enumerate(loader):
        inputs = inputs.to(device, non_blocking=True)
        targets = targets.to(device, non_blocking=True)
        with torch.amp.autocast(device_type=device.type, dtype=amp_dtype, enabled=amp_enabled):
            prediction = model(inputs)
            loss, metrics = criterion(prediction, targets)
            scaled_loss = loss / grad_accum_steps

        if precision == "fp16":
            scaler.scale(scaled_loss).backward()
        else:
            scaled_loss.backward()

        if (batch_index + 1) % grad_accum_steps == 0 or (batch_index + 1) == len(loader):
            if precision == "fp16":
                scaler.unscale_(optimizer)
            if grad_clip_norm > 0.0:
                torch.nn.utils.clip_grad_norm_(model.parameters(), grad_clip_norm)
            if precision == "fp16":
                scaler.step(optimizer)
                scaler.update()
            else:
                optimizer.step()
            optimizer.zero_grad(set_to_none=True)
            scheduler.step()
            local_steps += 1

        averages.update(metrics, n=inputs.shape[0])
    return averages.mean(), local_steps


@torch.no_grad()
def validate(
    model: nn.Module,
    loader: DataLoader[tuple[torch.Tensor, torch.Tensor]],
    criterion: MasteringLoss,
    device: torch.device,
    precision: str,
    external_metrics: ExternalPerceptualMetrics,
    max_batches: int | None,
) -> dict[str, float]:
    model.eval()
    averages = MetricAverages.create()
    amp_enabled = precision != "fp32"
    amp_dtype = autocast_dtype(precision)
    for batch_index, (inputs, targets) in enumerate(loader):
        if max_batches is not None and batch_index >= max_batches:
            break
        inputs = inputs.to(device, non_blocking=True)
        targets = targets.to(device, non_blocking=True)
        with torch.amp.autocast(device_type=device.type, dtype=amp_dtype, enabled=amp_enabled):
            prediction = model(inputs)
            _, loss_metrics = criterion(prediction, targets)
        batch_metrics = {
            **loss_metrics,
            "mse": float(F.mse_loss(prediction, targets).detach()),
            "si_sdr": float(si_sdr(prediction, targets).mean().detach()),
            "log_spectral_distance": float(log_spectral_distance(prediction, targets).detach()),
            "loudness_abs_error_db": float(F.l1_loss(rms_db(prediction), rms_db(targets)).detach()),
            "stereo_correlation_abs_error": float(F.l1_loss(stereo_correlation(prediction), stereo_correlation(targets)).detach()),
        }
        averages.update(batch_metrics, n=inputs.shape[0])
        if external_metrics.enabled() and batch_index == 0:
            averages.update(external_metrics.evaluate(prediction, targets), n=1)
    return averages.mean()


def build_loaders(args: argparse.Namespace) -> tuple[DataLoader[tuple[torch.Tensor, torch.Tensor]], DataLoader[tuple[torch.Tensor, torch.Tensor]]]:
    segment_samples = int(round(args.segment_seconds * args.sample_rate))
    train_dataset = AudioPairDataset(
        args.manifest,
        args.train_split,
        args.sample_rate,
        segment_samples,
        args.seed,
        training=True,
        input_gain_jitter_db=args.input_gain_jitter_db,
    )
    val_dataset = AudioPairDataset(
        args.manifest,
        args.val_split,
        args.sample_rate,
        segment_samples,
        args.seed,
        training=False,
    )
    common: dict[str, Any] = {
        "batch_size": args.batch_size,
        "num_workers": args.num_workers,
        "pin_memory": args.pin_memory,
        "persistent_workers": args.num_workers > 0 and args.persistent_workers,
    }
    if args.num_workers > 0:
        common["prefetch_factor"] = args.prefetch_factor
    train_loader = DataLoader(train_dataset, shuffle=True, drop_last=True, **common)
    val_loader = DataLoader(val_dataset, shuffle=False, drop_last=False, **common)
    return train_loader, val_loader


def parse_widths(value: str) -> tuple[int, ...]:
    widths = tuple(int(part.strip()) for part in value.split(",") if part.strip())
    if not widths:
        raise argparse.ArgumentTypeError("width list cannot be empty")
    return widths


def write_jsonl(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(payload, sort_keys=True) + "\n")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--resume", type=Path)
    parser.add_argument("--sample-rate", type=int, default=48000)
    parser.add_argument("--segment-seconds", type=float, default=5.46)
    parser.add_argument("--train-split", default="train")
    parser.add_argument("--val-split", default="val")
    parser.add_argument("--epochs", type=int, default=80)
    parser.add_argument("--batch-size", type=int, default=4)
    parser.add_argument("--learning-rate", type=float, default=2.0e-4)
    parser.add_argument("--weight-decay", type=float, default=1.0e-4)
    parser.add_argument("--warmup-steps", type=int, default=2000)
    parser.add_argument("--min-lr-ratio", type=float, default=0.05)
    parser.add_argument("--grad-accum-steps", type=int, default=1)
    parser.add_argument("--grad-clip-norm", type=float, default=1.0)
    parser.add_argument("--precision", choices=["fp32", "fp16", "bf16"], default="bf16")
    parser.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
    parser.add_argument("--num-workers", type=int, default=4)
    parser.add_argument("--prefetch-factor", type=int, default=4)
    parser.add_argument("--pin-memory", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--persistent-workers", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--input-gain-jitter-db", type=float, default=0.5)
    parser.add_argument("--widths", type=parse_widths, default=(64, 128, 192, 256, 384))
    parser.add_argument("--transformer-layers", type=int, default=6)
    parser.add_argument("--transformer-heads", type=int, default=8)
    parser.add_argument("--residual-scale", type=float, default=0.25)
    parser.add_argument("--stft-loss-weight", type=float, default=1.0)
    parser.add_argument("--waveform-loss-weight", type=float, default=0.25)
    parser.add_argument("--mid-side-loss-weight", type=float, default=0.25)
    parser.add_argument("--transient-loss-weight", type=float, default=0.15)
    parser.add_argument("--loudness-loss-weight", type=float, default=0.05)
    parser.add_argument("--stereo-loss-weight", type=float, default=0.1)
    parser.add_argument("--feature-loss-weight", type=float, default=0.0)
    parser.add_argument("--feature-checkpoint", type=Path)
    parser.add_argument("--visqol-command")
    parser.add_argument("--peaq-command")
    parser.add_argument("--external-metric-samples", type=int, default=0)
    parser.add_argument("--validate-batches", type=int)
    parser.add_argument("--compile", action="store_true")
    parser.add_argument("--seed", type=int, default=1337)
    parser.add_argument("--save-every", type=int, default=1)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    set_seed(args.seed)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    device = torch.device(args.device)
    if args.precision in {"fp16", "bf16"} and device.type == "cpu":
        print("mixed precision requested on CPU; continuing with autocast support where available")

    train_loader, val_loader = build_loaders(args)
    model = HybridMasteringNet(
        widths=args.widths,
        transformer_layers=args.transformer_layers,
        transformer_heads=args.transformer_heads,
        residual_scale=args.residual_scale,
    ).to(device)
    if args.compile:
        model = torch.compile(model)  # type: ignore[assignment]

    criterion = MasteringLoss(
        stft_weight=args.stft_loss_weight,
        waveform_weight=args.waveform_loss_weight,
        mid_side_weight=args.mid_side_loss_weight,
        transient_weight=args.transient_loss_weight,
        loudness_weight=args.loudness_loss_weight,
        stereo_weight=args.stereo_loss_weight,
        feature_weight=args.feature_loss_weight,
        feature_checkpoint=args.feature_checkpoint,
    ).to(device)

    optimizer = torch.optim.AdamW(model.parameters(), lr=args.learning_rate, weight_decay=args.weight_decay, betas=(0.9, 0.95))
    total_update_steps = math.ceil(len(train_loader) / max(1, args.grad_accum_steps)) * args.epochs
    scheduler = WarmupCosineScheduler(optimizer, args.warmup_steps, total_update_steps, args.min_lr_ratio)
    scaler = make_grad_scaler(device.type, enabled=args.precision == "fp16")
    external_metrics = ExternalPerceptualMetrics(
        visqol_command=args.visqol_command,
        peaq_command=args.peaq_command,
        sample_rate=args.sample_rate,
        max_samples=args.external_metric_samples,
    )

    start_epoch = 0
    global_step = 0
    best_score = math.inf
    if args.resume:
        start_epoch, global_step, best_score = load_checkpoint(args.resume, model, optimizer, scheduler, device)

    log_path = args.output_dir / "train_log.jsonl"
    (args.output_dir / "config.json").write_text(json.dumps(vars(args), indent=2, sort_keys=True, default=str) + "\n", encoding="utf-8")

    for epoch in range(start_epoch, args.epochs):
        train_metrics, steps = train_one_epoch(
            model,
            train_loader,
            criterion,
            optimizer,
            scheduler,
            scaler,
            device,
            args.precision,
            max(1, args.grad_accum_steps),
            args.grad_clip_norm,
        )
        global_step += steps
        val_metrics = validate(model, val_loader, criterion, device, args.precision, external_metrics, args.validate_batches)
        score = val_metrics.get("loss", math.inf)
        record = {
            "epoch": epoch,
            "globalStep": global_step,
            "learningRate": optimizer.param_groups[0]["lr"],
            "train": train_metrics,
            "validation": val_metrics,
        }
        write_jsonl(log_path, record)
        print(json.dumps(record, indent=2, sort_keys=True))

        save_checkpoint(args.output_dir / "last.pt", model, optimizer, scheduler, epoch, global_step, best_score, args)
        if score < best_score:
            best_score = score
            save_checkpoint(args.output_dir / "best.pt", model, optimizer, scheduler, epoch, global_step, best_score, args)
        if args.save_every > 0 and (epoch + 1) % args.save_every == 0:
            save_checkpoint(args.output_dir / "checkpoints" / f"epoch_{epoch:04d}.pt", model, optimizer, scheduler, epoch, global_step, best_score, args)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
