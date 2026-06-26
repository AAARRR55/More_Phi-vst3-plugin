#!/usr/bin/env python3
"""Differentiable proxy DSP for the More-Phi control regressor.

This module is intentionally training-only. It approximates the deployed
mastering chain with smooth PyTorch operations so the 63->72 parameter policy
can receive audio-domain loss without changing plugin runtime behavior.
"""

from __future__ import annotations

import math

import torch
from torch import nn

from codec import (
    DYNAMICS_SLICE,
    EQ_COUNT,
    EQ_SLICE,
    HARMONIC_SLICE,
    LIMITER_SLICE,
    LOUDNESS_SLICE,
    OUTPUT_DELTA_COUNT,
    STEREO_OUT_SLICE,
)

EPS = 1.0e-8


def db_to_linear(db: torch.Tensor | float) -> torch.Tensor:
    return torch.pow(torch.as_tensor(10.0, device=db.device if isinstance(db, torch.Tensor) else None), db / 20.0)


def linear_to_db(value: torch.Tensor, eps: float = EPS) -> torch.Tensor:
    return 20.0 * torch.log10(value.clamp_min(eps))


def ensure_stereo_batch(audio: torch.Tensor) -> torch.Tensor:
    """Return audio as [batch, 2, samples], preserving gradients."""
    if audio.dim() == 1:
        audio = audio.view(1, 1, -1)
    elif audio.dim() == 2:
        # Ambiguous [channels, samples] or [batch, samples]. Treat 1/2 leading
        # dimensions as channels; otherwise it is a mono batch.
        if audio.shape[0] in (1, 2):
            audio = audio.unsqueeze(0)
        else:
            audio = audio.unsqueeze(1)
    elif audio.dim() != 3:
        raise ValueError(f"expected audio with 1, 2, or 3 dims; got {tuple(audio.shape)}")

    if audio.shape[1] == 1:
        audio = audio.repeat(1, 2, 1)
    elif audio.shape[1] > 2:
        audio = audio[:, :2, :]
    return audio


def rms_lufs_proxy(audio: torch.Tensor) -> torch.Tensor:
    """Cheap differentiable LUFS proxy used only for training loss.

    This is not BS.1770. The acceptance oracle remains the headless renderer.
    """
    audio = ensure_stereo_batch(audio)
    ms = audio.pow(2).mean(dim=(1, 2)).clamp_min(EPS)
    return -0.691 + 10.0 * torch.log10(ms)


def smooth_true_peak_db(audio: torch.Tensor, p: float = 16.0) -> torch.Tensor:
    """Differentiable peak proxy via p-norm over samples/channels."""
    audio = ensure_stereo_batch(audio)
    peak = audio.abs().pow(p).mean(dim=(1, 2)).clamp_min(EPS).pow(1.0 / p)
    return linear_to_db(peak)


class DifferentiableMasteringChain(nn.Module):
    """Smooth training proxy for EQ, dynamics, stereo width, saturation, limiter.

    The mapping consumes the existing 72-delta output contract. It is deliberately
    conservative and parameter-bounded; it is not a replacement for the C++ DSP.
    """

    def __init__(self, sample_rate: float = 48000.0, eq_gain_db: float = 12.0) -> None:
        super().__init__()
        self.sample_rate = float(sample_rate)
        self.eq_gain_db = float(eq_gain_db)

        centers = torch.logspace(math.log10(30.0), math.log10(18000.0), EQ_COUNT)
        self.register_buffer("eq_centers_hz", centers)

    def _eq_response(self, eq_delta: torch.Tensor, n_bins: int, dtype: torch.dtype, device: torch.device) -> torch.Tensor:
        freqs = torch.linspace(0.0, self.sample_rate / 2.0, n_bins, dtype=dtype, device=device)
        safe_freqs = freqs.clamp_min(20.0)
        log_freqs = torch.log2(safe_freqs)
        centers = self.eq_centers_hz.to(device=device, dtype=dtype)
        log_centers = torch.log2(centers)

        # 32 overlapping log-frequency bands. Normalize the overlap per bin so a
        # broad all-band move stays near the intended dB range.
        octave_span = float(math.log2(18000.0 / 30.0))
        sigma = max(0.15, octave_span / EQ_COUNT * 0.9)
        basis = torch.exp(-0.5 * ((log_freqs[:, None] - log_centers[None, :]) / sigma) ** 2)
        basis = basis / basis.sum(dim=1, keepdim=True).clamp_min(EPS)

        gain_db = eq_delta * self.eq_gain_db
        response_db = gain_db @ basis.transpose(0, 1)
        return torch.pow(torch.as_tensor(10.0, dtype=dtype, device=device), response_db / 20.0)

    def _apply_eq(self, audio: torch.Tensor, deltas: torch.Tensor) -> torch.Tensor:
        eq_delta = deltas[:, EQ_SLICE[0] : EQ_SLICE[1]]
        spectrum = torch.fft.rfft(audio, dim=-1)
        response = self._eq_response(eq_delta, spectrum.shape[-1], audio.dtype, audio.device)
        filtered = torch.fft.irfft(spectrum * response[:, None, :], n=audio.shape[-1], dim=-1)
        return filtered

    def _apply_width(self, audio: torch.Tensor, deltas: torch.Tensor) -> torch.Tensor:
        width_delta = deltas[:, STEREO_OUT_SLICE[0]]
        width = 0.25 + 1.5 * torch.sigmoid(2.0 * width_delta)
        mid = 0.5 * (audio[:, 0, :] + audio[:, 1, :])
        side = 0.5 * (audio[:, 0, :] - audio[:, 1, :]) * width[:, None]
        return torch.stack((mid + side, mid - side), dim=1)

    def _apply_compressor(self, audio: torch.Tensor, deltas: torch.Tensor) -> torch.Tensor:
        dyn = deltas[:, DYNAMICS_SLICE[0] : DYNAMICS_SLICE[1]]
        amount = torch.sigmoid(2.0 * dyn[:, 0])
        threshold_db = -24.0 + 12.0 * torch.tanh(dyn[:, 1])
        ratio = 1.0 + 5.0 * torch.sigmoid(2.0 * dyn[:, 2])
        knee_db = 6.0

        level = audio.pow(2).mean(dim=1).clamp_min(EPS).sqrt()
        level_db = linear_to_db(level)
        over = torch.nn.functional.softplus((level_db - threshold_db[:, None]) / knee_db) * knee_db
        reduction_db = over * (1.0 - 1.0 / ratio[:, None]) * amount[:, None]
        gain = torch.pow(torch.as_tensor(10.0, dtype=audio.dtype, device=audio.device), -reduction_db / 20.0)
        return audio * gain[:, None, :]

    def _apply_saturation(self, audio: torch.Tensor, deltas: torch.Tensor) -> torch.Tensor:
        harmonic = deltas[:, HARMONIC_SLICE[0]]
        drive = torch.exp(harmonic).clamp_min(0.1)
        denom = torch.tanh(drive).clamp_min(EPS)
        return torch.tanh(audio * drive[:, None, None]) / denom[:, None, None]

    def _apply_loudness_gain(self, audio: torch.Tensor, deltas: torch.Tensor) -> torch.Tensor:
        loudness = deltas[:, LOUDNESS_SLICE[0]]
        gain_db = 12.0 * torch.tanh(loudness)
        gain = torch.pow(torch.as_tensor(10.0, dtype=audio.dtype, device=audio.device), gain_db / 20.0)
        return audio * gain[:, None, None]

    def _apply_limiter(self, audio: torch.Tensor, deltas: torch.Tensor) -> torch.Tensor:
        limiter = deltas[:, LIMITER_SLICE[0]]
        ceiling_db = -1.0 + 0.9 * torch.tanh(limiter)
        ceiling = torch.pow(torch.as_tensor(10.0, dtype=audio.dtype, device=audio.device), ceiling_db / 20.0)
        return ceiling[:, None, None] * torch.tanh(audio / ceiling[:, None, None].clamp_min(EPS))

    def forward(self, audio: torch.Tensor, deltas: torch.Tensor) -> torch.Tensor:
        if deltas.dim() != 2 or deltas.shape[1] != OUTPUT_DELTA_COUNT:
            raise ValueError(f"expected deltas [batch, {OUTPUT_DELTA_COUNT}], got {tuple(deltas.shape)}")

        x = ensure_stereo_batch(audio)
        if x.shape[0] != deltas.shape[0]:
            if x.shape[0] == 1:
                x = x.expand(deltas.shape[0], -1, -1)
            else:
                raise ValueError(f"audio batch {x.shape[0]} != delta batch {deltas.shape[0]}")

        x = self._apply_loudness_gain(x, deltas)
        x = self._apply_eq(x, deltas)
        x = self._apply_compressor(x, deltas)
        x = self._apply_width(x, deltas)
        x = self._apply_saturation(x, deltas)
        x = self._apply_limiter(x, deltas)
        return torch.nan_to_num(x, nan=0.0, posinf=1.0, neginf=-1.0)
