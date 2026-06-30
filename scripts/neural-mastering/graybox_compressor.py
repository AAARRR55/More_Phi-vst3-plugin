#!/usr/bin/env python3
"""F2 gray-box differentiable solid-state bus compressor.

A strong, interpretable baseline for the SolidStateBusComp waveform line. The
topology is FIXED (linked-stereo detector -> soft-knee gain computer -> log-domain
attack/release smoother -> auto-makeup) and CONDITIONED on the dataset's
(threshold, attack, release, ratio). A handful of LEARNABLE global parameters
calibrate device-label semantics (knee width, attack/release unit scaling,
detector peak/RMS blend, auto-makeup fraction).

Trained across all 220 settings on input->output pairs, it is the lower bound
the black-box HybridMasteringNet (F1) must beat. Limitation (by design): models
compression only — no EQ/imaging. See specs/006 plan.md §3.2 (F2).

P3 note (ponytail): EQ augmentation (eq_augment.py) is intentionally NOT applied
to F2 — its topology has no EQ parameters, so input-only EQ would spike the loss
from topology mismatch (measuring the wrong thing, not robustness). The P3
robustness gate (held-out-song SI-SDR under +-6 dB EQ) is F1-only. F2 is reused
by F1 only as (a) device_gr_curve for the GR-matching loss and (b) an optional
eval-only proxy D(A(x)) behind --eq-proxy. P4 may revisit F2 augmentation once a
synthetic chain lets us re-render targets (plan §1.2).

Run the self-check for per-param monotonicity + differentiability evidence:
    python graybox_compressor.py
"""

from __future__ import annotations

import argparse
import json
import math
import sys

import torch
import torch.nn as nn
import torch.nn.functional as F


def _block_stats(x: torch.Tensor, hop: int) -> tuple[torch.Tensor, torch.Tensor, int]:
    """x [B,2,T] -> (peak[B,n], rms[B,n], n), trimming to a whole multiple of hop."""
    n = x.shape[-1] // hop
    blk = x[:, :, : n * hop].reshape(x.shape[0], 2, n, hop)
    peak = blk.abs().amax(dim=(1, 3))                       # [B,n]
    rms = (blk.pow(2).mean(dim=(1, 3)) + 1e-12).sqrt()       # [B,n]
    return peak, rms, n


def _soft_knee_gr(det_db: torch.Tensor, thr: torch.Tensor, ratio: torch.Tensor, knee: torch.Tensor) -> torch.Tensor:
    """Instantaneous gain reduction (dB, >=0). det_db[B,n]; thr/ratio[B,1]; knee scalar.

    Hard knee above thr+knee/2, quadratic blend inside the knee, zero below
    thr-knee/2. Continuous at both knee edges (Zolzer soft knee)."""
    eps = 1e-6
    slope = (1.0 - 1.0 / ratio.clamp_min(1.0 + eps))         # [B,1], >=0
    lo = thr - knee / 2.0
    hi = thr + knee / 2.0
    gr = torch.zeros_like(det_db)
    above = det_db >= hi
    in_knee = (det_db > lo) & (~above)
    gr = torch.where(above, slope * (det_db - thr), gr)
    gr = torch.where(in_knee, slope * (det_db - lo).pow(2) / (2.0 * knee.clamp_min(eps)), gr)
    return gr.clamp_min(0.0)


def _smooth_gr(gr_inst: torch.Tensor, alpha_a: torch.Tensor, alpha_r: torch.Tensor) -> torch.Tensor:
    """Log-domain one-pole attack/release smoother over blocks.

    gr_inst[B,n]; alpha_a/alpha_r[B] in (0,1). Attack (rising GR) uses alpha_a,
    release (falling GR) uses alpha_r. The rising/falling switch is a detached
    discrete mask; gradients flow through the continuous smoothing recurrence.
    ponytail: Python loop over blocks (control rate). Upgrade path: a fused
    associative-scan custom kernel if block counts make this a training bottleneck.
    """
    B, n = gr_inst.shape
    aa = alpha_a.unsqueeze(1)
    ar = alpha_r.unsqueeze(1)
    prev = gr_inst.new_zeros(B, 1)
    out = []
    for t in range(n):
        cur = gr_inst[:, t : t + 1]
        alpha = torch.where(cur > prev, aa, ar)              # detached mask
        prev = prev + alpha * (cur - prev)
        out.append(prev)
    return torch.cat(out, dim=1)                             # [B,n]


class GrayBoxBusCompressor(nn.Module):
    def __init__(self, hop: int = 256) -> None:
        super().__init__()
        self.hop = hop
        # Five learnable global calibrations (the "gray" in gray-box).
        self.log_knee = nn.Parameter(torch.tensor(2.0))      # knee_db = softplus -> ~2 dB
        self.attack_log_scale = nn.Parameter(torch.tensor(-7.0))   # ~ms-to-s
        self.release_log_scale = nn.Parameter(torch.tensor(0.5))   # ~seconds
        self.detector_raw = nn.Parameter(torch.tensor(0.0))  # sigmoid -> peak/RMS blend
        self.makeup_raw = nn.Parameter(torch.tensor(-2.0))   # sigmoid*0.5 -> auto-makeup frac

    def forward(
        self,
        x: torch.Tensor,
        cond: dict[str, torch.Tensor],
        fs: float = 48000.0,
    ) -> tuple[torch.Tensor, torch.Tensor]:
        """x [B,2,T]; cond values are [B] tensors: threshold(dB), ratio, attack, release.

        Returns (y [B,2,T], gr_smooth [B,n]) — gr_smooth is the model's implicit
        gain-reduction curve in dB (>=0), exposed for the GR-matching loss/eval."""
        eps = 1e-8
        thr = cond["threshold"].unsqueeze(-1)               # [B,1]
        ratio = cond["ratio"].unsqueeze(-1)
        attack_label = cond["attack"].clamp_min(1e-3)
        release_label = cond["release"].clamp_min(1e-3)

        peak, rms, n = _block_stats(x, self.hop)
        blend = torch.sigmoid(self.detector_raw)
        det_db = 20.0 * torch.log10(((1.0 - blend) * peak + blend * rms) + eps)  # [B,n]

        knee = F.softplus(self.log_knee)
        gr_inst = _soft_knee_gr(det_db, thr, ratio, knee)   # [B,n]

        att_sec = F.softplus(self.attack_log_scale) * attack_label
        rel_sec = F.softplus(self.release_log_scale) * release_label
        alpha_a = torch.exp(-1.0 / (att_sec * fs).clamp_min(eps))
        alpha_r = torch.exp(-1.0 / (rel_sec * fs).clamp_min(eps))
        gr_smooth = _smooth_gr(gr_inst, alpha_a, alpha_r)   # [B,n]

        makeup = torch.sigmoid(self.makeup_raw) * 0.5 * gr_smooth.mean(dim=1, keepdim=True)  # [B,1]
        gain_db = makeup - gr_smooth                        # [B,n]
        gain_lin = torch.pow(10.0, gain_db / 20.0)          # [B,n]

        gain_sr = F.interpolate(gain_lin.unsqueeze(1), size=n * self.hop, mode="linear", align_corners=False).squeeze(1)
        if n * self.hop < x.shape[-1]:                       # hold last gain over the tail (< one hop)
            tail = gain_lin[:, -1:].expand(-1, x.shape[-1] - n * self.hop)
            gain_sr = torch.cat([gain_sr, tail], dim=1)
        y = x * gain_sr.unsqueeze(1)
        return y, gr_smooth


def _hot_signal(batch: int, samples: int, fs: float, seed: int = 0) -> torch.Tensor:
    """Deterministic stereo test signal, RMS ~ -6 dBFS (hot enough to sit above
    tested thresholds)."""
    g = torch.Generator().manual_seed(seed)
    t = torch.arange(samples, dtype=torch.float32) / fs
    freqs = torch.tensor([110.0, 220.0, 880.0])
    fundamental = torch.sin(2 * math.pi * freqs.unsqueeze(1) * t.unsqueeze(0)).mean(dim=0)
    sig = fundamental + 0.3 * torch.randn(samples, generator=g)
    sig = sig / (sig.abs().max() + 1e-8) * 0.6
    stereo = torch.stack([sig, sig * 0.95], dim=0).unsqueeze(0).repeat(batch, 1, 1)
    return stereo


def selfcheck() -> int:
    fs = 48000.0
    x = _hot_signal(2, 8192, fs)
    model = GrayBoxBusCompressor(hop=64)

    def cond(thr: float, ratio: float, attack: float, release: float) -> dict:
        b = x.shape[0]
        return {
            "threshold": torch.full((b,), thr),
            "ratio": torch.full((b,), ratio),
            "attack": torch.full((b,), attack),
            "release": torch.full((b,), release),
        }

    failures: list[str] = []

    # 1. Differentiable: backward populates finite grads on all learnables.
    y, gr = model(x, cond(-12.0, 4.0, 10.0, 1.0), fs)
    y.mean().backward()
    for name, p in model.named_parameters():
        if p.grad is None or not torch.isfinite(p.grad).all():
            failures.append(f"grad bad for {name}")

    # 2. Threshold monotonicity: lower threshold -> more gain reduction.
    _, gr_lo = model(x, cond(-24.0, 4.0, 10.0, 1.0), fs)
    _, gr_hi = model(x, cond(-12.0, 4.0, 10.0, 1.0), fs)
    if not (gr_lo.mean() > gr_hi.mean()):
        failures.append(f"threshold not monotone: GR(-24)={gr_lo.mean():.3f} <= GR(-12)={gr_hi.mean():.3f}")

    # 3. Ratio monotonicity: higher ratio -> more gain reduction.
    _, gr_r10 = model(x, cond(-12.0, 10.0, 10.0, 1.0), fs)
    _, gr_r2 = model(x, cond(-12.0, 2.0, 10.0, 1.0), fs)
    if not (gr_r10.mean() > gr_r2.mean()):
        failures.append(f"ratio not monotone: GR(r10)={gr_r10.mean():.3f} <= GR(r2)={gr_r2.mean():.3f}")

    # 4. Identity at bypass (ratio=1): no compression -> output == input.
    model.zero_grad()
    y_id, gr_id = model(x, cond(0.0, 1.0, 10.0, 1.0), fs)
    if not torch.allclose(y_id, x, atol=1e-5):
        failures.append(f"identity fails: max|y-x|={( y_id - x).abs().max():.2e}")
    if gr_id.abs().max() > 1e-5:
        failures.append(f"bypass leaks GR: {gr_id.abs().max():.2e}")

    # 5. Stereo-linked: identical per-sample gain on L and R.
    y_l, _ = model(x, cond(-12.0, 4.0, 10.0, 1.0), fs)
    gain_l = y_l[:, 0, :] / (x[:, 0, :].abs() + 1e-8)
    gain_r = y_l[:, 1, :] / (x[:, 1, :].abs() + 1e-8)
    mask = (x[:, 0, :].abs() > 1e-3) & (x[:, 1, :].abs() > 1e-3)
    if mask.any() and (gain_l[mask] - gain_r[mask]).abs().max() > 1e-4:
        failures.append("stereo channels diverge (not linked)")

    # 6. No NaNs; tiny param count (the interpretability claim).
    if not torch.isfinite(y).all():
        failures.append("NaN/Inf in output")
    n_params = sum(p.numel() for p in model.parameters())

    report = {
        "paramCount": n_params,
        "GR(thr=-24,r=4)": float(gr_lo.detach().mean()),
        "GR(thr=-12,r=4)": float(gr_hi.detach().mean()),
        "GR(thr=-12,r=10)": float(gr_r10.detach().mean()),
        "GR(thr=-12,r=2)": float(gr_r2.detach().mean()),
        "identityMaxError": float((y_id - x).abs().max().detach()),
        "failures": failures,
    }
    print(json.dumps(report, indent=2, sort_keys=True))
    if failures or n_params > 16:
        print("SELF-CHECK FAIL", file=sys.stderr)
        return 1
    print("SELF-CHECK PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(selfcheck())
