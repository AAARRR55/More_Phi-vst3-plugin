"""Mastering control regressor (More-Phi neural mastering "Model A").

Architecture (per the design doc): a small 1D-CNN front-end over the 32
spectral bands (their contour carries mastering-relevant signal — tilt and
spectral balance) concatenated with the scalar features, feeding an MLP
head whose final layer is tanh-bounded so every output delta is in [-1, 1].

Why this shape and not a transformer / waveform net:
  - Input is a 63-float feature vector, not audio. A transformer is overkill
    and a liability (size/latency). An MLP + tiny conv head is the right
    inductive bias for compact tabular+contour features.
  - Outputs are 72 control deltas advising an existing DSP chain at 1-5 Hz on
    the message thread, not per-sample audio. There is no latency budget on
    the audio path; inference is sub-millisecond on CPU.
  - ~150k params -> ~0.6 MB fp32, ~150 KB int8 — far inside the <100 MB / <50%
    CPU budget, and fits the OnnxNeuralMasteringRunner seam (63 -> 72) exactly.

Input normalization:
  The raw feature tensor (from serialize_featureFrame) mixes dB-scale loudness
  (~[-30,0]), ratios (~[0,1]), and raw meta (sampleRate ~48000, frameIndex up
  to 1e6). Feeding those directly saturates the tanh head. Normalization is
  applied INSIDE the model as the first op, with fixed per-feature scales
  derived from each field's expected range. This keeps the ONNX self-contained
  — the C++ runner feeds raw serializeFeatureFrame() values unchanged, and
  train/serve parity is structural (the same scales are baked into the graph).

Determinism: the model is fully deterministic at eval/export time. Set
torch.manual_seed at train time for reproducibility; inference has no
stochastic ops.
"""

from __future__ import annotations

import torch
from torch import nn

from codec import (
    INPUT_FEATURE_COUNT,
    OUTPUT_DELTA_COUNT,
    SCALAR_FEATURE_COUNT,
    SPECTRAL_BAND_COUNT,
    STEREO_BAND_COUNT,
)


def _build_input_scales() -> torch.Tensor:
    """Fixed per-feature divisor used to normalize the raw input tensor.

    Derived from each NeuralMasteringFeatureFrame field's expected range.
    Indexes match serialize_feature_frame() exactly:
      0:10  loudness fields (dB)        -> /12 (LRA/tilt/crest live here too)
      11    spectral tilt (dB-ish)      -> /6
      7     mono fold-down (dB)         -> /3   (handled via index below)
    Band/meta scales follow. These are hand-tuned ranges, not learned stats,
    so the normalization is stable across train and serve without a dataset.
    """
    s = torch.ones(INPUT_FEATURE_COUNT)
    # [0] integratedLUFS, [1] shortTerm, [2] momentary  ~[-30,0] dB
    s[0] = s[1] = s[2] = 12.0
    # [3] loudnessRange ~[0,20]
    s[3] = 7.0
    # [4] truePeakDbTp ~[-3,0]
    s[4] = 1.5
    # [5] crestFactorDb ~[6,18]
    s[5] = 12.0
    # [6] spectralTilt ~[-6,6]
    s[6] = 3.0
    # [7] monoFoldDownDeltaDb ~[-3,3]
    s[7] = 1.5
    # [8] transientDensity ~[0,1], [9] harmonicRisk ~[0,1], [10] sourceQuality ~[0,1]
    s[8] = s[9] = s[10] = 0.5
    # [11:43] spectralBands — log-magnitude-ish, treat as ~[0,1]
    s[11 : 11 + SPECTRAL_BAND_COUNT] = 0.5
    # [43:51] stereoCorrelation ~[-1,1]
    s[43 : 43 + STEREO_BAND_COUNT] = 0.5
    # [51:59] midSideRatio ~[0,1]
    s[51 : 51 + STEREO_BAND_COUNT] = 0.5
    # [59] sampleRate — 44100/48000/88200/96000; normalize to typical
    s[59] = 48000.0
    # [60] channelCount — 1 or 2
    s[60] = 2.0
    # [61] blockSize — 128..2048
    s[61] = 512.0
    # [62] frameIndex (low 32 bits) — up to ~4.3e9
    s[62] = 2.0e9
    return s


class _Normalize(nn.Module):
    """Per-feature scale normalization, baked into the exported graph.

    Divides the raw input by fixed scales and applies tanh to squash to ~[-1,1]
    before the rest of the network sees it. No learned params — fixed ops only,
    so ONNX constant-folds them.
    """

    def __init__(self) -> None:
        super().__init__()
        self.register_buffer("scales", _build_input_scales())

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return torch.tanh(x / self.scales)


class _SpectralFrontEnd(nn.Module):
    """Tiny 1D-CNN over the 32 spectral bands -> a compact contour embedding.

    Two depthwise-style conv layers extract local spectral shape (tilt,
    peaks/notches) which a pure MLP would have to learn expensively.
    """

    def __init__(self, out_dim: int = 32) -> None:
        super().__init__()
        # Treat the 32 bands as a 1-channel "sequence"; k=5 captures ~1/3-octave
        # neighbourhood relationships, matching how mastering EQ is reasoned about.
        self.conv1 = nn.Conv1d(1, 16, kernel_size=5, padding=2)
        self.conv2 = nn.Conv1d(16, 32, kernel_size=5, padding=2)
        self.act = nn.SiLU()
        self.pool = nn.AdaptiveAvgPool1d(1)
        self.proj = nn.Linear(32, out_dim)

    def forward(self, spectral: torch.Tensor) -> torch.Tensor:
        # spectral: [batch, SPECTRAL_BAND_COUNT]
        x = spectral.unsqueeze(1)  # [batch, 1, bands]
        x = self.act(self.conv1(x))
        x = self.act(self.conv2(x))
        x = self.pool(x).squeeze(-1)  # [batch, 32]
        return self.act(self.proj(x))


class MasteringControlRegressor(nn.Module):
    """Feature frame (63) -> control deltas (72), tanh-bounded.

    The input is normalized internally (see _Normalize) before being split at
    the C++ layout boundary:
      [0:11]   scalar features
      [11:43]  spectral bands  -> routed through the conv front-end
      [43:51]  stereo correlation
      [51:59]  mid/side ratio
      [59:63]  meta (sample rate, channel count, block size, frame index)
    """

    def __init__(
        self,
        scalar_hidden: int = 192,
        joint_hidden: int = 192,
        spectral_embed: int = 48,
        dropout: float = 0.1,
    ) -> None:
        super().__init__()
        self.normalize = _Normalize()
        self.spectral = _SpectralFrontEnd(out_dim=spectral_embed)

        # Scalar + band + meta context branch.
        scalar_in = (INPUT_FEATURE_COUNT - SPECTRAL_BAND_COUNT) + spectral_embed
        self.scalar_block = nn.Sequential(
            nn.Linear(scalar_in, scalar_hidden),
            nn.SiLU(),
            nn.Dropout(dropout),
            nn.Linear(scalar_hidden, scalar_hidden),
            nn.SiLU(),
        )

        self.joint = nn.Sequential(
            nn.Linear(scalar_hidden, joint_hidden),
            nn.SiLU(),
            nn.Dropout(dropout),
            nn.Linear(joint_hidden, joint_hidden),
            nn.SiLU(),
        )

        # Final tanh head: structural [-1, 1] bound matching sanitizePlanCandidate.
        self.head = nn.Sequential(
            nn.Linear(joint_hidden, OUTPUT_DELTA_COUNT),
            nn.Tanh(),
        )

    def forward(self, feature_tensor: torch.Tensor) -> torch.Tensor:
        if feature_tensor.dim() != 2 or feature_tensor.shape[1] != INPUT_FEATURE_COUNT:
            raise ValueError(
                f"expected [batch, {INPUT_FEATURE_COUNT}], got {tuple(feature_tensor.shape)}"
            )

        feature_tensor = self.normalize(feature_tensor)

        spectral = feature_tensor[:, SCALAR_FEATURE_COUNT : SCALAR_FEATURE_COUNT + SPECTRAL_BAND_COUNT]
        rest = torch.cat(
            (
                feature_tensor[:, :SCALAR_FEATURE_COUNT],
                feature_tensor[:, SCALAR_FEATURE_COUNT + SPECTRAL_BAND_COUNT :],
            ),
            dim=1,
        )

        spectral_embed = self.spectral(spectral)
        context = torch.cat((rest, spectral_embed), dim=1)
        context = self.scalar_block(context)
        joint = self.joint(context)
        return self.head(joint)


def count_parameters(model: nn.Module) -> int:
    """Total trainable parameter count — used to verify the ~150k budget."""
    return sum(p.numel() for p in model.parameters() if p.requires_grad)


def build_model() -> MasteringControlRegressor:
    """Construct the model with the reference hyperparameters."""
    torch.manual_seed(1337)
    return MasteringControlRegressor()
