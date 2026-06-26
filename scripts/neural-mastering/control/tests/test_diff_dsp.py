#!/usr/bin/env python3
from __future__ import annotations

import sys
from pathlib import Path

import torch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from codec import OUTPUT_DELTA_COUNT  # noqa: E402
from diff_dsp import DifferentiableMasteringChain, rms_lufs_proxy, smooth_true_peak_db  # noqa: E402


def test_chain_shape_finite_and_gradients_on_silence() -> None:
    chain = DifferentiableMasteringChain(sample_rate=48000)
    audio = torch.zeros(2, 2, 2048, requires_grad=True)
    deltas = torch.zeros(2, OUTPUT_DELTA_COUNT, requires_grad=True)

    out = chain(audio, deltas)
    assert out.shape == audio.shape
    assert torch.isfinite(out).all()
    assert torch.isfinite(rms_lufs_proxy(out)).all()
    assert torch.isfinite(smooth_true_peak_db(out)).all()

    loss = out.pow(2).mean() + deltas.pow(2).mean()
    loss.backward()
    assert audio.grad is not None and torch.isfinite(audio.grad).all()
    assert deltas.grad is not None and torch.isfinite(deltas.grad).all()


def test_chain_preserves_stereo_shape_and_has_smooth_parameter_response() -> None:
    torch.manual_seed(7)
    chain = DifferentiableMasteringChain(sample_rate=48000)
    audio = torch.randn(1, 2, 4096) * 0.05
    deltas = torch.zeros(1, OUTPUT_DELTA_COUNT, requires_grad=True)
    deltas.data[0, 8] = 0.1
    deltas.data[0, 64] = 0.2

    out_a = chain(audio, deltas)
    objective = out_a.abs().mean()
    objective.backward()
    assert deltas.grad is not None
    assert torch.isfinite(deltas.grad).all()
    assert deltas.grad.abs().sum() > 0.0

    nudged = deltas.detach().clone()
    nudged[0, 8] += 1.0e-3
    out_b = chain(audio, nudged)
    assert out_b.shape == (1, 2, 4096)
    assert torch.isfinite(out_b).all()
    assert float((out_b - out_a.detach()).abs().mean()) < 1.0e-3
