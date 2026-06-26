#!/usr/bin/env python3
"""Hybrid parameter + audio-domain loss for the control regressor."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable

import torch
from torch import nn

from codec import EQ_SLICE
from diff_dsp import DifferentiableMasteringChain, rms_lufs_proxy, smooth_true_peak_db


@dataclass(frozen=True)
class HybridLossWeights:
    param: float = 1.0
    spectral: float = 1.0
    lufs: float = 0.5
    true_peak: float = 5.0
    eq_smooth: float = 0.05
    restraint: float = 0.02

    def as_dict(self) -> dict[str, float]:
        return {
            "param": self.param,
            "spectral": self.spectral,
            "lufs": self.lufs,
            "true_peak": self.true_peak,
            "eq_smooth": self.eq_smooth,
            "restraint": self.restraint,
        }


class MultiScaleSpectralLoss(nn.Module):
    def __init__(self, fft_sizes: Iterable[int] = (256, 512, 1024), log_weight: float = 1.0) -> None:
        super().__init__()
        self.fft_sizes = tuple(int(v) for v in fft_sizes)
        self.log_weight = float(log_weight)

    def forward(self, predicted: torch.Tensor, target: torch.Tensor) -> torch.Tensor:
        if predicted.shape != target.shape:
            raise ValueError(f"spectral loss shape mismatch: {tuple(predicted.shape)} vs {tuple(target.shape)}")

        flat_pred = predicted.reshape(-1, predicted.shape[-1])
        flat_target = target.reshape(-1, target.shape[-1])
        losses: list[torch.Tensor] = []

        for n_fft in self.fft_sizes:
            if flat_pred.shape[-1] < n_fft:
                continue
            hop = max(1, n_fft // 4)
            window = torch.hann_window(n_fft, device=predicted.device, dtype=predicted.dtype)
            pred_spec = torch.stft(flat_pred, n_fft=n_fft, hop_length=hop, window=window, return_complex=True).abs()
            target_spec = torch.stft(flat_target, n_fft=n_fft, hop_length=hop, window=window, return_complex=True).abs()
            linear = (pred_spec - target_spec).abs().mean()
            log_term = (torch.log(pred_spec + 1.0e-7) - torch.log(target_spec + 1.0e-7)).abs().mean()
            losses.append(linear + self.log_weight * log_term)

        if not losses:
            return (predicted - target).abs().mean()
        return torch.stack(losses).mean()


class HybridMasteringLoss(nn.Module):
    """Composite loss for hybrid control-policy training.

    If explicit target audio is unavailable, the labeled target deltas are
    rendered through the same differentiable proxy and detached. That keeps the
    audio-domain term useful for existing manifests while still requiring audio
    references for every row.
    """

    def __init__(
        self,
        sample_rate: float = 48000.0,
        weights: HybridLossWeights | None = None,
        ceiling_dbtp: float = -1.0,
    ) -> None:
        super().__init__()
        self.weights = weights or HybridLossWeights()
        self.ceiling_dbtp = float(ceiling_dbtp)
        self.chain = DifferentiableMasteringChain(sample_rate=sample_rate)
        self.spectral = MultiScaleSpectralLoss()

    def forward(
        self,
        audio: torch.Tensor,
        predicted_delta: torch.Tensor,
        target_delta: torch.Tensor,
        target_audio: torch.Tensor | None = None,
        has_target_audio: torch.Tensor | None = None,
    ) -> tuple[torch.Tensor, dict[str, float]]:
        rendered = self.chain(audio, predicted_delta)

        with torch.no_grad():
            proxy_target = self.chain(audio, target_delta)
            if target_audio is not None and has_target_audio is not None:
                mask = has_target_audio.to(device=rendered.device, dtype=rendered.dtype).view(-1, 1, 1)
                audio_target = target_audio.to(device=rendered.device, dtype=rendered.dtype)
                proxy_target = mask * audio_target + (1.0 - mask) * proxy_target

        param = nn.functional.mse_loss(predicted_delta, target_delta)
        spectral = self.spectral(rendered, proxy_target)
        lufs = (rms_lufs_proxy(rendered) - rms_lufs_proxy(proxy_target)).abs().mean()
        peak_over = torch.relu(smooth_true_peak_db(rendered) - self.ceiling_dbtp).pow(2).mean()

        eq = predicted_delta[:, EQ_SLICE[0] : EQ_SLICE[1]]
        eq_smooth = (eq[:, 1:] - eq[:, :-1]).abs().mean() if eq.shape[1] > 1 else eq.abs().mean()
        restraint = predicted_delta.abs().mean()

        total = (
            self.weights.param * param
            + self.weights.spectral * spectral
            + self.weights.lufs * lufs
            + self.weights.true_peak * peak_over
            + self.weights.eq_smooth * eq_smooth
            + self.weights.restraint * restraint
        )

        metrics = {
            "loss": float(total.detach()),
            "param": float(param.detach()),
            "spectral": float(spectral.detach()),
            "lufs": float(lufs.detach()),
            "true_peak": float(peak_over.detach()),
            "eq_smooth": float(eq_smooth.detach()),
            "restraint": float(restraint.detach()),
        }
        return total, metrics
