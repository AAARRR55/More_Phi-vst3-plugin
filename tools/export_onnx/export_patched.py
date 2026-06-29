"""Export masteringbrainv2 to ONNX using a patched, ONNX-compatible architecture.

The original model uses two ops that block ONNX export:
  nn.TransformerEncoder  →  aten::_transformer_encoder_layer_fwd  (fused, unsupported)
  torch.fft.rfftfreq     →  aten.fft_rfftfreq                     (no ONNX mapping)

This script:
  1. Loads the original checkpoint
  2. Replaces nn.TransformerEncoder with ManualTransformerEncoder — identical
     state-dict structure (loads original weights) but manual attention in forward()
  3. Pre-computes rfftfreq bins as a constant buffer
  4. Patches compute_input_band_log_energies to use the pre-computed buffer
  5. Exports via torch.onnx.export (TorchScript path, opset 17)
  6. Runs parity check: PyTorch vs ONNX on the same synthetic seed

Usage:
  python tools/export_onnx/export_patched.py \
      --package "C:/Users/HP/Downloads/sonicmaster-v3-decision-engine-20260530T121536Z" \
      --output-dir build/sonicmaster
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

# Force UTF-8 stdout on Windows.
if sys.stdout.encoding != 'utf-8':
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
if sys.stderr.encoding != 'utf-8':
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

SEGMENT_FRAMES = 262138
DECISION_WIDTH = 44


# ── ONNX-compatible TransformerEncoder replacement ──────────────────────────
#
# nn.TransformerEncoder exports as aten::_transformer_encoder_layer_fwd, a fused
# CUDA op that ONNX doesn't understand. This replacement has the IDENTICAL state-
# dict structure (same submodule names, same parameter shapes) so the original
# checkpoint weights load without any mapping. The forward() uses manual
# multi-head attention (Linear + reshape + matmul + softmax) — basic ops ONNX
# fully supports.

class ManualMultiheadAttention(nn.Module):
    """Drop-in for nn.MultiheadAttention with ONNX-traceable forward."""

    def __init__(self, embed_dim: int, num_heads: int, dropout: float = 0.0,
                 batch_first: bool = True):
        super().__init__()
        self.embed_dim = embed_dim
        self.num_heads = num_heads
        self.head_dim = embed_dim // num_heads
        self.batch_first = batch_first
        # nn.MultiheadAttention uses a fused in_proj_weight [3*E, E].
        self.in_proj_weight = nn.Parameter(torch.empty(3 * embed_dim, embed_dim))
        self.in_proj_bias   = nn.Parameter(torch.empty(3 * embed_dim))
        self.out_proj = nn.Linear(embed_dim, embed_dim)
        self.dropout = dropout
        self._reset_parameters()

    def _reset_parameters(self):
        nn.init.xavier_uniform_(self.in_proj_weight)
        nn.init.constant_(self.in_proj_bias, 0.0)

    def forward(self, query, key, value,
                attn_mask=None, key_padding_mask=None):
        # query, key, value all have shape [B, T, E] when batch_first=True.
        B, T, E = query.shape
        H = self.num_heads
        D = self.head_dim

        # Fused QKV projection matching nn.MultiheadAttention's in_proj.
        # in_proj_weight shape is [3*E, E]; we do one matmul then split.
        qkv = F.linear(query, self.in_proj_weight, self.in_proj_bias)  # [B, T, 3*E]
        q, k, v = qkv.chunk(3, dim=-1)

        # Reshape to [B, H, T, D].
        q = q.view(B, T, H, D).transpose(1, 2)
        k = k.view(B, T, H, D).transpose(1, 2)
        v = v.view(B, T, H, D).transpose(1, 2)

        # Scaled dot-product attention.
        scale = D ** -0.5
        attn = (q @ k.transpose(-2, -1)) * scale
        attn = F.softmax(attn, dim=-1)
        if self.dropout > 0 and self.training:
            attn = F.dropout(attn, p=self.dropout)

        out = attn @ v                        # [B, H, T, D]
        out = out.transpose(1, 2).contiguous().view(B, T, E)
        return self.out_proj(out), None       # attn_weights=None (ONNX safe)


class ManualTransformerEncoderLayer(nn.Module):
    """Drop-in for nn.TransformerEncoderLayer with ONNX-traceable forward.

    The state-dict structure matches exactly:
        self_attn.(in_proj_weight|in_proj_bias|out_proj.weight|out_proj.bias)
        linear1.(weight|bias)
        linear2.(weight|bias)
        norm1.(weight|bias)
        norm2.(weight|bias)
        dropout1, dropout2
    """

    def __init__(self, d_model: int, nhead: int, dim_feedforward: int = 2048,
                 dropout: float = 0.1, activation: str = "gelu",
                 batch_first: bool = True, norm_first: bool = True):
        super().__init__()
        self.self_attn = ManualMultiheadAttention(d_model, nhead, dropout=dropout,
                                                   batch_first=batch_first)
        self.linear1 = nn.Linear(d_model, dim_feedforward)
        self.linear2 = nn.Linear(dim_feedforward, d_model)
        self.norm1 = nn.LayerNorm(d_model)
        self.norm2 = nn.LayerNorm(d_model)
        self.dropout1 = nn.Dropout(dropout)
        self.dropout2 = nn.Dropout(dropout)
        self.activation = activation
        self.norm_first = norm_first

    def forward(self, src, src_mask=None, src_key_padding_mask=None):
        if self.norm_first:
            src2 = self.norm1(src)
            src = src + self.dropout1(self.self_attn(src2, src2, src2)[0])
            src2 = self.norm2(src)
            src = src + self.dropout2(
                self.linear2(self.dropout2(
                    F.gelu(self.linear1(src2))
                    if self.activation == "gelu" else F.relu(self.linear1(src2))
                ))
            )
        else:
            src2, _ = self.self_attn(src, src, src)
            src = src + self.dropout1(src2)
            src = self.norm1(src)
            src2 = self.linear2(self.dropout2(
                F.gelu(self.linear1(src)) if self.activation == "gelu"
                else F.relu(self.linear1(src))
            ))
            src = src + self.dropout2(src2)
            src = self.norm2(src)
        return src


class ManualTransformerEncoder(nn.Module):
    """Drop-in for nn.TransformerEncoder with ONNX-traceable forward.

    State-dict structure: layers.{i}.{same as ManualTransformerEncoderLayer}
    """

    def __init__(self, encoder_layer: nn.TransformerEncoderLayer,
                 num_layers: int):
        super().__init__()
        d_model = encoder_layer.self_attn.embed_dim
        nhead = encoder_layer.self_attn.num_heads
        dim_ff = encoder_layer.linear1.out_features
        dropout = encoder_layer.dropout.p
        norm_first = encoder_layer.norm_first
        self.layers = nn.ModuleList([
            ManualTransformerEncoderLayer(
                d_model, nhead, dim_ff, dropout,
                batch_first=True, norm_first=norm_first,
            )
            for _ in range(num_layers)
        ])

    def forward(self, src, mask=None):
        for layer in self.layers:
            src = layer(src)
        return src


def _patch_transformer(network: nn.Module) -> None:
    """Replace any nn.TransformerEncoder with ManualTransformerEncoder.

    Copies the original weights by matching state-dict keys. The replacement
    has identical submodule structure, so load_state_dict works directly.
    """
    replaced = False
    for name, module in list(network.named_children()):
        if isinstance(module, nn.TransformerEncoder):
            num_layers = len(module.layers)
            new_enc = ManualTransformerEncoder(module.layers[0], num_layers)
            # Build the state dict for the replacement from the original.
            new_enc.load_state_dict(module.state_dict(), strict=True)
            setattr(network, name, new_enc)
            replaced = True
            print(f"[patch] replaced {name} (nn.TransformerEncoder, "
                  f"{num_layers} layers) with ManualTransformerEncoder")
        elif (isinstance(module, nn.Module) and
              not isinstance(module, ManualTransformerEncoder)):
            _patch_transformer(module)
    if not replaced:
        # Also search deeper (ModuleList etc.)
        for name, module in network.named_children():
            if (isinstance(module, (nn.ModuleList, nn.Sequential))
                    and not isinstance(module, ManualTransformerEncoder)):
                for child in module:
                    _patch_transformer(child)


# ── Pre-computed FFT frequency bins ──────────────────────────────────────────

def _patch_rfftfreq(model_module):
    """Replace torch.fft.rfft + rfftfreq with STFT-based computation.

    compute_input_band_log_energies uses rfft + rfftfreq to get per-band energy.
    Both ops block ONNX export. We replace the whole function with an STFT-based
    equivalent: compute magnitude spectrogram via torch.stft (ONNX-supported),
    sum power across time per frequency bin, then sum into bands.
    """
    # Pre-compute frequency bin → band mapping for the standard 262138-sample
    # input at 44100 Hz. The model was trained with spectral_inject_bands=24;
    # we always use _spectral_inject_crossovers to match the original.

    def _patched_compute_input_band_log_energies(
        waveform, num_bands=8, sample_rate=44100, crossovers=None
    ):
        if crossovers is None:
            crossovers = model_module._spectral_inject_crossovers(
                num_bands, sample_rate
            )
        if len(crossovers) != num_bands - 1:
            raise ValueError(
                f"spectral injection needs {num_bands-1} crossovers for "
                f"{num_bands} bands, got {len(crossovers)}"
            )

        B = waveform.shape[0]
        mono = waveform.mean(dim=1)  # [B, S]

        # Use a large n_fft for good frequency resolution across all 24 bands.
        # 4096-point FFT at 44100 Hz → ~10.8 Hz per bin.
        n_fft = 4096
        hop = n_fft // 4
        win = torch.hann_window(n_fft, device=waveform.device, dtype=torch.float32)
        spec = torch.stft(
            mono.reshape(-1, mono.shape[-1]),
            n_fft=n_fft,
            hop_length=hop,
            win_length=n_fft,
            window=win,
            return_complex=False,
        )  # [B, n_fft//2+1, T, 2]
        power = (spec[..., 0] ** 2 + spec[..., 1] ** 2).sum(dim=-2)  # [B, n_fft//2+1]

        # Map STFT frequency bins to bands. Freq per bin: k * sr / n_fft.
        freqs = torch.arange(power.shape[-1], device=power.device,
                             dtype=torch.float32) * (sample_rate / n_fft)
        edges = [0.0] + list(crossovers) + [float(sample_rate) / 2.0]

        bands = []
        for i in range(num_bands):
            lo, hi = edges[i], edges[i + 1]
            # Avoid aten::index with boolean mask (broken in ONNX).
            # Use float masking + sum instead.
            mask_f = ((freqs >= lo) & (freqs < hi)).float().unsqueeze(0)  # [1, F]
            band_power = (power * mask_f).sum(dim=-1) + 1e-12  # [B]
            bands.append(10.0 * torch.log10(band_power))

        stacked = torch.stack(bands, dim=-1)  # [B, num_bands]
        centered = stacked - stacked.mean(dim=-1, keepdim=True)
        return centered.to(waveform.dtype)

    # Replace the module-level function.
    model_module.compute_input_band_log_energies = (
        _patched_compute_input_band_log_energies
    )
    print("[patch] compute_input_band_log_energies → STFT-based (ONNX-safe)")


# ── Monkey-patch isfinite guards ─────────────────────────────────────────────

def _patch_isfinite_guards(model_module):
    """Replace validate_waveform + stft_magnitudes for ONNX compatibility."""
    model_module.validate_waveform = lambda waveform, channels=2: None
    print("[patch] validate_waveform → no-op")

    # torch.stft with return_complex=True is not supported in ONNX.
    # Patch stft_magnitudes to use return_complex=False and compute magnitude.
    _orig_stft_mag = model_module.stft_magnitudes

    def _patched_stft_magnitudes(waveform, config):
        flat = waveform.float().flatten(0, 1)
        magnitudes = []
        for resolution in config.stft_resolutions:
            window = torch.hann_window(
                resolution.win_length, device=waveform.device, dtype=torch.float32
            )
            spec = torch.stft(
                flat,
                resolution.fft_size,
                hop_length=resolution.hop_length,
                win_length=resolution.win_length,
                window=window,
                return_complex=False,
            )  # [B, n_fft//2+1, T, 2]
            mag = (spec[..., 0] ** 2 + spec[..., 1] ** 2).sqrt().clamp_min(
                config.log_magnitude_floor)
            magnitudes.append(mag)
        return magnitudes

    model_module.stft_magnitudes = _patched_stft_magnitudes
    print("[patch] stft_magnitudes → return_complex=False (ONNX-safe)")


# ── Export pipeline ──────────────────────────────────────────────────────────

def _resolve_paths(package_root: Path) -> tuple[Path, Path]:
    ckpt = (package_root / "models" / "v3" / "mastering-brain-v2-fullchain-best"
            / "checkpoints" / "best.ckpt")
    if not ckpt.exists():
        sys.exit(f"checkpoint not found: {ckpt}")
    return ckpt, package_root


def _load_module(package_root: Path):
    sys.path.insert(0, str(package_root / "training" / "neural-mastering"
                           / "bin" / "training"))
    from master_audio import load_module_from_checkpoint

    ckpt, _ = _resolve_paths(package_root)
    module, hparams = load_module_from_checkpoint(ckpt, torch.device("cpu"))
    module.eval()

    # Import model module for patching.
    import model as _model_module

    # Apply patches.
    _patch_rfftfreq(_model_module)
    _patch_isfinite_guards(_model_module)
    _patch_transformer(module.network)

    return module, hparams


class _DecisionOnly(nn.Module):
    """Wrap the network so forward(waveform) -> decision tensor.

    target_lufs is baked as a constant input (the analysis engine passes it
    separately to the inference source, and the ONNX runner only handles a
    single waveform input). A value of -14.0 LUFS is the default used by the
    SonicMasterAnalysisEngine.
    """

    def __init__(self, module, target_lufs: float = -14.0):
        super().__init__()
        self.net = module.network
        self.target_lufs = target_lufs

    def forward(self, waveform: torch.Tensor) -> torch.Tensor:
        target = torch.tensor([self.target_lufs], device=waveform.device,
                              dtype=torch.float32)
        out = self.net(waveform, target_lufs_db=target)
        if isinstance(out, tuple):
            out = out[0]
        decisions = getattr(self.net, "_last_mastering_decisions", None)
        return decisions if decisions is not None else out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--package", type=Path, required=True,
                    help="sonicmaster-v3-decision-engine package root")
    ap.add_argument("--output-dir", type=Path, default=Path("build/sonicmaster"))
    ap.add_argument("--target-lufs", type=float, default=-14.0)
    ap.add_argument("--opset", type=int, default=18)
    args = ap.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    module, _ = _load_module(args.package)
    wrapper = _DecisionOnly(module, target_lufs=args.target_lufs).eval()

    # Synthetic 6 s stereo for tracing + parity.
    rng = np.random.RandomState(0)
    t = np.arange(SEGMENT_FRAMES, dtype=np.float32) / 44100.0
    mono = (
        np.sin(2 * np.pi * 220.0 * t)
        + 0.3 * np.sin(2 * np.pi * 2500.0 * t)
        + 0.05 * rng.randn(SEGMENT_FRAMES)
    ).astype(np.float32) * 0.1
    stereo = np.broadcast_to(mono[None, :], (2, SEGMENT_FRAMES)).copy()
    waveform = torch.from_numpy(stereo).unsqueeze(0)   # [1, 2, N]
    target_lufs = torch.tensor([args.target_lufs], dtype=torch.float32)

    onnx_path = args.output_dir / "masteringbrain_v2_decision.onnx"

    with torch.no_grad():
        # PyTorch reference output.
        pt_out = wrapper(waveform).detach().cpu().numpy().reshape(-1)

        # Export via TorchScript path (dynamo=False). The manual transformer
        # avoids aten::_transformer_encoder_layer_fwd, and the patched rfftfreq
        # avoids aten.fft_rfftfreq.
        print("[export] tracing via TorchScript exporter (dynamo=False)...")
        torch.onnx.export(
            wrapper,
            (waveform,),
            str(onnx_path),
            input_names=["waveform"],
            output_names=["decision"],
            dynamic_axes={"waveform": {0: "batch"}, "decision": {0: "batch"}},
            opset_version=args.opset,
            do_constant_folding=True,
            dynamo=False,
        )
        print(f"[export] wrote {onnx_path}")

    # ── Parity check ─────────────────────────────────────────────────────────
    onnx_out_prefix = None
    max_abs = float("nan")
    try:
        import onnxruntime as ort
        sess = ort.InferenceSession(str(onnx_path),
                                     providers=["CPUExecutionProvider"])
        onnx_out = sess.run(None, {
            "waveform": waveform.numpy(),
        })[0].reshape(-1)
        max_abs = float(np.max(np.abs(pt_out - onnx_out)))
        onnx_out_prefix = onnx_out[:8].tolist()
    except Exception as exc:
        print(f"[warn] parity check skipped: {exc}")

    (args.output_dir / "parity_report.json").write_text(json.dumps({
        "pytorch_output_prefix": pt_out[:8].tolist(),
        "onnx_output_prefix": onnx_out_prefix,
        "max_abs_diff": max_abs,
        "passed": bool(np.isfinite(max_abs) and max_abs < 1e-4),
    }, indent=2))

    contract = {
        "schemaVersion": 1,
        "modelId": "masteringbrain-v2-fullchain-best",
        "inputName": "waveform",
        "outputName": "decision",
        "inputShape": [1, 2, SEGMENT_FRAMES],
        "outputShape": [1, DECISION_WIDTH],
        "sampleRate": 44100,
        "targetLufsDefault": args.target_lufs,
    }
    (args.output_dir / "masteringbrain_v2_contract.json").write_text(
        json.dumps(contract, indent=2))

    print(f"wrote {args.output_dir / 'masteringbrain_v2_contract.json'}")
    verdict = "PASS" if (np.isfinite(max_abs) and max_abs < 1e-4) else "FAIL"
    print(f"parity max_abs_diff={max_abs:.2e} -> {verdict}")
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
