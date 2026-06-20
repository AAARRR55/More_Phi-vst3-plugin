#!/usr/bin/env python3
"""Train the More-Phi mastering control regressor (Model A).

Two data modes:

  --data-mode synthetic
      Zero external dependencies. Generates random (feature, delta) pairs from
      a smooth synthetic teacher so the whole train+export pipeline can be
      smoke-tested on a fresh remote in seconds before spending real data. The
      teacher is intentionally simple (a hand-coded linear+sign map) — it
      exists to exercise the pipeline, NOT to be a real mastering target.

  --data-mode manifest
      Reads a JSONL manifest produced by the repo's feature-extraction +
      synthetic-target pipeline. Each line:
          {"feature": [63 floats], "delta": [72 floats in [-1,1]]}
      This is the mode for real training. Build the manifest with the in-repo
      dataset generator (see specs/003-neural-mastering-roadmap/) plus
      scripts/neural-mastering/extract_features.py.

The script exports an ONNX model at the end (--export-onnx) whose I/O contract
matches the C++ OnnxNeuralMasteringRunner seam exactly (63 -> 72, tanh). The
emitted model passes the contract-assertion test in tests/test_contract.py.

Example (synthetic smoke, CPU):
    python train.py --data-mode synthetic --epochs 20 --export-onnx model.onnx
"""

from __future__ import annotations

import argparse
import json
import math
import random
import sys
from pathlib import Path

import torch
from torch import nn
from torch.utils.data import DataLoader, Dataset

# Allow running as `python train.py` from the package dir or via -m.
sys.path.insert(0, str(Path(__file__).resolve().parent))

from codec import (  # noqa: E402
    INPUT_FEATURE_COUNT,
    OUTPUT_DELTA_COUNT,
    SCALAR_FEATURE_COUNT,
    SPECTRAL_BAND_COUNT,
    FeatureFrame,
    serialize_feature_frame,
)
from model import build_model, count_parameters  # noqa: E402


# ── Loss ─────────────────────────────────────────────────────────────────────


class MasteringControlLoss(nn.Module):
    """Weighted MSE + an EQ monotonicity regularizer.

    The EQ-delta smoothness term penalises large jumps between adjacent EQ
    band deltas: a mastering curve that zig-zags band-to-band sounds bad even
    at low MSE. Cheap (one L1 over a diff), high value.
    """

    def __init__(self, eq_count: int, eq_smooth_weight: float = 0.05) -> None:
        super().__init__()
        self.eq_count = eq_count
        self.eq_smooth_weight = eq_smooth_weight

    def forward(self, pred: torch.Tensor, target: torch.Tensor) -> tuple[torch.Tensor, dict[str, float]]:
        mse = nn.functional.mse_loss(pred, target)
        eq_pred = pred[:, : self.eq_count]
        eq_target = target[:, : self.eq_count]
        # Adjacent-band jump magnitude — penalise on prediction (and lightly on
        # target so the term doesn't fight a legitimately smooth target).
        pred_rough = (eq_pred[:, 1:] - eq_pred[:, :-1]).abs().mean()
        target_rough = (eq_target[:, 1:] - eq_target[:, :-1]).abs().mean()
        smooth = (pred_rough - target_rough).clamp_min(0.0)
        loss = mse + self.eq_smooth_weight * smooth
        return loss, {
            "mse": float(mse.detach()),
            "eq_smooth": float(smooth.detach()),
            "loss": float(loss.detach()),
        }


# ── Synthetic teacher ────────────────────────────────────────────────────────


class SyntheticTeacher:
    """Hand-coded, deterministic-ish map (frame -> deltas).

    NOT a real mastering model. It exists so the train/export/verify loop can
    run end-to-end without datasets. The map is smooth and bounded, which is
    enough to confirm the model can learn *something* and that the contract is
    intact. Real training uses --data-mode manifest with real pairs.
    """

    def __init__(self, seed: int = 1337) -> None:
        self.rng = random.Random(seed)

    def sample_frame(self) -> FeatureFrame:
        return FeatureFrame(
            integrated_lufs=self.rng.uniform(-23.0, -8.0),
            short_term_lufs=self.rng.uniform(-24.0, -6.0),
            momentary_lufs=self.rng.uniform(-30.0, -4.0),
            loudness_range=self.rng.uniform(2.0, 14.0),
            true_peak_dbtp=self.rng.uniform(-3.0, 0.0),
            crest_factor_db=self.rng.uniform(6.0, 18.0),
            spectral_tilt=self.rng.uniform(-6.0, 6.0),
            mono_fold_down_delta_db=self.rng.uniform(-3.0, 3.0),
            transient_density=self.rng.uniform(0.0, 1.0),
            harmonic_risk=self.rng.uniform(0.0, 0.5),
            source_quality_score=self.rng.uniform(0.5, 1.0),
            spectral_bands=tuple(self.rng.uniform(0.0, 1.0) for _ in range(SPECTRAL_BAND_COUNT)),
            stereo_correlation=tuple(self.rng.uniform(-0.3, 1.0) for _ in range(8)),
            mid_side_ratio=tuple(self.rng.uniform(0.1, 1.0) for _ in range(8)),
            sample_rate=self.rng.choice([44100.0, 48000.0]),
            channel_count=2,
            block_size=self.rng.choice([256, 512, 1024]),
            frame_index=self.rng.randint(0, 1_000_000),
        )

    def teacher_deltas(self, frame: FeatureFrame) -> list[float]:
        """Smooth bounded map. Loud material -> reduce loudness delta, tilted
        spectrum -> corrective EQ tilt, etc. Shape only; not validated as audio.
        """
        lufs = frame.integrated_lufs
        tilt = frame.spectral_tilt
        quality = max(0.1, frame.source_quality_score)

        eq = []
        for i in range(SPECTRAL_BAND_COUNT):
            # Low bands cut when tilt is bright, highs boosted when dull.
            band_pos = i / (SPECTRAL_BAND_COUNT - 1)  # 0=low ... 1=high
            corrective = math.tanh(-tilt * (band_pos - 0.5) / 3.0) * 0.4
            eq.append(max(-1.0, min(1.0, corrective)))

        loudness_delta = math.tanh((-14.0 - lufs) / 6.0) * 0.6
        dynamics_delta = math.tanh((lufs + 14.0) / 8.0) * 0.3 * quality
        stereo_delta = math.tanh((frame.mono_fold_down_delta_db) / 3.0) * 0.2
        harmonic_delta = -math.tanh(frame.harmonic_risk * 2.0) * 0.2
        limiter_delta = max(0.0, math.tanh((-1.0 - frame.true_peak_dbtp) / 2.0)) * 0.3

        out = (
            eq
            + [dynamics_delta] * 8
            + [stereo_delta] * 8
            + [harmonic_delta] * 8
            + [limiter_delta] * 8
            + [loudness_delta] * 8
        )
        assert len(out) == OUTPUT_DELTA_COUNT
        return out


class SyntheticDataset(Dataset):
    def __init__(self, teacher: SyntheticTeacher, size: int) -> None:
        self.teacher = teacher
        self.size = size

    def __len__(self) -> int:
        return self.size

    def __getitem__(self, idx: int):
        frame = self.teacher.sample_frame()
        feature = torch.tensor(serialize_feature_frame(frame), dtype=torch.float32)
        delta = torch.tensor(self.teacher.teacher_deltas(frame), dtype=torch.float32)
        return feature, delta


# ── Manifest (real-data) dataset ─────────────────────────────────────────────


class ManifestDataset(Dataset):
    """Reads a JSONL manifest of {"feature": [63], "delta": [72]} lines."""

    def __init__(self, path: Path) -> None:
        self.items: list[tuple[list[float], list[float]]] = []
        with path.open("r", encoding="utf-8") as handle:
            for line_no, line in enumerate(handle, start=1):
                line = line.strip()
                if not line:
                    continue
                row = json.loads(line)
                feature = row["feature"]
                delta = row["delta"]
                if len(feature) != INPUT_FEATURE_COUNT:
                    raise ValueError(f"{path}:{line_no} feature length {len(feature)} != {INPUT_FEATURE_COUNT}")
                if len(delta) != OUTPUT_DELTA_COUNT:
                    raise ValueError(f"{path}:{line_no} delta length {len(delta)} != {OUTPUT_DELTA_COUNT}")
                self.items.append((feature, delta))
        if not self.items:
            raise ValueError(f"manifest {path} contained no rows")

    def __len__(self) -> int:
        return len(self.items)

    def __getitem__(self, idx: int):
        feature, delta = self.items[idx]
        return torch.tensor(feature, dtype=torch.float32), torch.tensor(delta, dtype=torch.float32)


# ── ONNX export ──────────────────────────────────────────────────────────────


def export_onnx(model: nn.Module, path: Path) -> None:
    """Export the model to ONNX with static shapes (matches the C++ seam).

    Static shapes let ONNX Runtime fully optimize the graph. Input/output
    names are kept boring ("input"/"output") so the C++ runner can hardcode
    them when ONNX Runtime is linked.
    """
    model.eval()
    model.cpu()  # export on CPU: ONNX graph is device-agnostic, and this avoids the cuda/cpu FakeTensor mismatch when the model was trained on GPU
    dummy = torch.zeros(1, INPUT_FEATURE_COUNT, dtype=torch.float32)
    with torch.no_grad():
        torch.onnx.export(
            model,
            dummy,
            str(path),
            input_names=["input"],
            output_names=["output"],
            dynamic_axes=None,  # static batch=1; the plugin proposes one plan at a time
            opset_version=18,  # torch 2.12's exporter floor; ORT >= 1.16 supports it
            do_constant_folding=True,
        )


# ── Training loop ────────────────────────────────────────────────────────────


def train(args: argparse.Namespace) -> None:
    torch.manual_seed(args.seed)
    device = torch.device(args.device)

    if args.data_mode == "synthetic":
        teacher = SyntheticTeacher(seed=args.seed)
        train_ds = SyntheticDataset(teacher, args.synthetic_train)
        val_ds = SyntheticDataset(teacher, args.synthetic_val)
    elif args.data_mode == "manifest":
        train_ds = ManifestDataset(Path(args.train_manifest))
        val_ds = ManifestDataset(Path(args.val_manifest))
    else:
        raise ValueError(f"unknown data mode {args.data_mode}")

    train_loader = DataLoader(train_ds, batch_size=args.batch_size, shuffle=True, drop_last=True)
    val_loader = DataLoader(val_ds, batch_size=args.batch_size, shuffle=False)

    model = build_model().to(device)
    print(f"model params: {count_parameters(model):,} (budget ~150k)")
    criterion = MasteringControlLoss(eq_count=SPECTRAL_BAND_COUNT, eq_smooth_weight=args.eq_smooth_weight).to(device)
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.learning_rate, weight_decay=args.weight_decay)

    best_val = math.inf
    for epoch in range(args.epochs):
        model.train()
        train_metrics = {"loss": 0.0, "mse": 0.0, "eq_smooth": 0.0}
        n_batches = 0
        for feature, delta in train_loader:
            feature = feature.to(device)
            delta = delta.to(device)
            pred = model(feature)
            loss, metrics = criterion(pred, delta)
            optimizer.zero_grad(set_to_none=True)
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            optimizer.step()
            for k in train_metrics:
                train_metrics[k] += metrics[k]
            n_batches += 1
        train_metrics = {k: v / max(1, n_batches) for k, v in train_metrics.items()}

        model.eval()
        val_metrics = {"loss": 0.0, "mse": 0.0}
        n_val = 0
        with torch.no_grad():
            for feature, delta in val_loader:
                feature = feature.to(device)
                delta = delta.to(device)
                pred = model(feature)
                _, metrics = criterion(pred, delta)
                val_metrics["loss"] += metrics["loss"]
                val_metrics["mse"] += metrics["mse"]
                n_val += 1
        val_metrics = {k: v / max(1, n_val) for k, v in val_metrics.items()}

        improved = val_metrics["loss"] < best_val
        best_val = min(best_val, val_metrics["loss"])
        print(
            f"epoch {epoch:3d} | train loss {train_metrics['loss']:.5f} mse {train_metrics['mse']:.5f} "
            f"| val loss {val_metrics['loss']:.5f} mse {val_metrics['mse']:.5f}"
            f"{' *' if improved else ''}"
        )

    # Save state dict (PyTorch) for resuming.
    Path(args.output_dir).mkdir(parents=True, exist_ok=True)
    torch.save(
        {"model": model.state_dict(), "args": vars(args)},
        Path(args.output_dir) / "control_regressor.pt",
    )

    if args.export_onnx:
        onnx_path = Path(args.export_onnx)
        export_onnx(model, onnx_path)
        print(f"exported ONNX -> {onnx_path}")


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--data-mode", choices=["synthetic", "manifest"], default="synthetic")
    p.add_argument("--train-manifest", help="JSONL feature/delta manifest (manifest mode)")
    p.add_argument("--val-manifest", help="JSONL feature/delta manifest (manifest mode)")
    p.add_argument("--synthetic-train", type=int, default=2048)
    p.add_argument("--synthetic-val", type=int, default=512)
    p.add_argument("--epochs", type=int, default=20)
    p.add_argument("--batch-size", type=int, default=64)
    p.add_argument("--learning-rate", type=float, default=3e-4)
    p.add_argument("--weight-decay", type=float, default=1e-4)
    p.add_argument("--eq-smooth-weight", type=float, default=0.05)
    p.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
    p.add_argument("--output-dir", default="runs/control-regressor")
    p.add_argument("--export-onnx", type=Path, default=Path("control_regressor.onnx"))
    p.add_argument("--seed", type=int, default=1337)
    return p.parse_args()


if __name__ == "__main__":
    train(parse_args())
