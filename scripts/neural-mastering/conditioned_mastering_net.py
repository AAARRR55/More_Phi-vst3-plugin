#!/usr/bin/env python3
"""F1 black-box waveform forward model, controllable via FiLM conditioning.

Extends HybridMasteringNet (train_neural_mastering.py) WITHOUT modifying it:
the encoder/decoder/transformer blocks are imported and reused, and a FiLM
projector modulates each encoder stage by the normalized conditioning vector
cond = [c, e] (P4: 8-dim — compressor c = threshold/attack/release/ratio, PLUS
EQ e = low/mid/high/q; pre-P4 just c, 4-dim). This is what makes the black-box
model CONTROLLABLE — the defining property of the SolidStateBusComp virtual-analog
task — and the model the 5-param gray-box floor (F2) must beat.

FiLM is initialized to identity (gamma=1, beta=0) so the net starts as the
unconditioned model and conditioning is learned stably.

An inverse head performs parameter inference: audio -> predicted controls. This
is the "predict the mastering move from the audio" capability, supervised as an
auxiliary MSE against the true normalized params.

Self-check: python conditioned_mastering_net.py
"""

from __future__ import annotations

import json
import sys

import torch
import torch.nn as nn
import torch.nn.functional as F

import train_neural_mastering as tm  # reuse blocks; do not fork

EncoderBlock = tm.EncoderBlock
DecoderBlock = tm.DecoderBlock
SinusoidalPositionEncoding = tm.SinusoidalPositionEncoding
mid_side = tm.mid_side
group_count = tm.group_count


class FiLMProjector(nn.Module):
    """cond_dim vector -> per-stage (gamma, beta) channel-affine modulations."""

    def __init__(self, cond_dim: int, stage_channels: list[int], hidden: int = 128) -> None:
        super().__init__()
        self.two_c = [2 * c for c in stage_channels]
        total = sum(self.two_c)
        self.mlp = nn.Sequential(nn.Linear(cond_dim, hidden), nn.SiLU(), nn.Linear(hidden, total))
        self._identity_init()

    def _identity_init(self) -> None:
        last = self.mlp[-1]
        last.weight.data.zero_()
        bias = torch.zeros_like(last.bias)
        off = 0
        for c in (s // 2 for s in self.two_c):
            bias[off : off + c] = 1.0          # gamma = 1
            off += 2 * c                        # beta = 0
        last.bias.data.copy_(bias)

    def forward(self, cond: torch.Tensor) -> list[tuple[torch.Tensor, torch.Tensor]]:
        out = self.mlp(cond)
        films, pos = [], 0
        for two_c in self.two_c:
            chunk = out[:, pos : pos + two_c]
            c = two_c // 2
            films.append((chunk[:, :c].unsqueeze(-1), chunk[:, c:].unsqueeze(-1)))  # gamma,beta [B,c,1]
            pos += two_c
        return films


class InverseParamHead(nn.Module):
    """Parameter inference: stereo audio -> n_params normalized controls."""

    def __init__(self, n_params: int = 8) -> None:
        super().__init__()
        layers, prev = [], 32
        for c in (32, 64, 128):
            layers += [nn.Conv1d(prev, c, 9, stride=4, padding=4),
                       nn.GroupNorm(group_count(c), c), nn.SiLU()]
            prev = c
        self.stem = nn.Conv1d(4, 32, 7, padding=3)
        self.enc = nn.Sequential(*layers)
        self.head = nn.Sequential(nn.SiLU(), nn.Linear(128, n_params))

    def forward(self, audio: torch.Tensor) -> torch.Tensor:
        mid, side = mid_side(audio)
        x = torch.cat((audio, mid, side), dim=1)
        x = self.enc(F.silu(self.stem(x)))
        return self.head(x.mean(dim=-1))


class ConditionedHybridMasteringNet(nn.Module):
    def __init__(
        self,
        widths: tuple[int, ...] = (64, 128, 192, 256, 384),
        strides: tuple[int, ...] = (4, 4, 4, 4, 4),
        transformer_layers: int = 6,
        transformer_heads: int = 8,
        residual_scale: float = 0.25,
        cond_dim: int = 8,
    ) -> None:
        super().__init__()
        if len(widths) != len(strides):
            raise ValueError("widths and strides must have equal length")
        self.residual_scale = residual_scale
        self.stem = nn.Sequential(
            nn.Conv1d(4, widths[0], kernel_size=7, padding=3),
            nn.GroupNorm(group_count(widths[0]), widths[0]),
            nn.SiLU(),
            tm.GatedResidualBlock(widths[0]),
        )
        encoders, in_ch = [], widths[0]
        for w, s in zip(widths, strides):
            encoders.append(EncoderBlock(in_ch, w, s))
            in_ch = w
        self.encoders = nn.ModuleList(encoders)
        self.film = FiLMProjector(cond_dim, list(widths))

        d = widths[-1]
        self.position = SinusoidalPositionEncoding(d)
        layer = nn.TransformerEncoderLayer(d, transformer_heads, d * 4, dropout=0.0,
                                           activation="gelu", batch_first=True, norm_first=True)
        self.transformer = nn.TransformerEncoder(layer, transformer_layers)

        decoders, rev = [], tuple(reversed(widths))
        current = rev[0]
        for sc, oc in zip(rev[1:], rev[1:]):
            decoders.append(DecoderBlock(current, sc, oc))
            current = oc
        self.decoders = nn.ModuleList(decoders)
        self.head = nn.Sequential(nn.Conv1d(current, current, 7, padding=3), nn.SiLU(),
                                  nn.Conv1d(current, 2, 7, padding=3))
        self.inverse = InverseParamHead(n_params=cond_dim)

    def forward(self, waveform: torch.Tensor, cond: torch.Tensor) -> torch.Tensor:
        mid, side = mid_side(waveform)
        x = torch.cat((waveform, mid, side), dim=1)
        x = self.stem(x)
        films = self.film(cond)
        skips = []
        for i, enc in enumerate(self.encoders):
            x = enc(x)
            gamma, beta = films[i]
            x = gamma * x + beta                       # FiLM per encoder stage
            skips.append(x)
        x = x.transpose(1, 2)
        x = self.transformer(self.position(x))
        x = x.transpose(1, 2)
        for dec, skip in zip(self.decoders, reversed(skips[:-1])):
            x = dec(x, skip)
        x = F.interpolate(x, size=waveform.shape[-1], mode="linear", align_corners=False)
        delta = torch.tanh(self.head(x))
        return torch.tanh(waveform + self.residual_scale * delta)


def selfcheck() -> int:
    torch.manual_seed(0)
    model = ConditionedHybridMasteringNet()
    n_params = sum(p.numel() for p in model.parameters())
    x = torch.randn(2, 2, 8192)
    c1 = torch.randn(2, 8)
    c2 = torch.randn(2, 8)

    pred = model(x, c1)
    inv = model.inverse(x)
    failures: list[str] = []
    if pred.shape != x.shape:
        failures.append(f"pred shape {tuple(pred.shape)} != {tuple(x.shape)}")
    if inv.shape != (2, 8):
        failures.append(f"inverse shape {tuple(inv.shape)} != (2, 8)")
    if not torch.isfinite(pred).all() or not torch.isfinite(inv).all():
        failures.append("non-finite output")

    # FiLM identity at init: two different conds -> identical output (gamma=1, beta=0).
    if not torch.allclose(model(x, c1), model(x, c2), atol=1e-5):
        failures.append("FiLM not identity at init (unstable conditioning start)")

    # Differentiable through both heads.
    (pred.mean() + inv.mean()).backward()
    for name, p in model.named_parameters():
        if p.grad is None or not torch.isfinite(p.grad).all():
            failures.append(f"bad grad: {name}")

    print(json.dumps({"paramCount_M": round(n_params / 1e6, 3), "failures": failures}, indent=2, sort_keys=True))
    if failures:
        print("SELF-CHECK FAIL", file=sys.stderr)
        return 1
    print("SELF-CHECK PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(selfcheck())
