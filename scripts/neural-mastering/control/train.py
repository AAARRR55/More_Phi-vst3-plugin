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
from torch.utils.data import ConcatDataset, DataLoader, Dataset

# Allow running as `python train.py` from the package dir or via -m.
sys.path.insert(0, str(Path(__file__).resolve().parent))

from codec import (  # noqa: E402
    HARMONIC_COUNT,
    HARMONIC_SLICE,
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
    """Weighted MSE + EQ monotonicity + restraint priors.

    Components:
      - MSE: primary regression target.
      - eq_smooth_weight: penalises adjacent EQ-band jumps (a zig-zag mastering
        curve sounds bad even at low MSE). Cheap (one L1 over a diff), high value.
      - delta_l1_weight: Fix 1 (restraint). L1 on predicted deltas toward neutral.
        Correction-only data pulls every delta toward a saturated correction; this
        holds predictions toward smaller (more restrained) moves unless the audio
        strongly drives them. Default ON (0.02); pair with null-pair corpus (Fix 3).
      - overcorrect_weight: Fix 5 (restraint). Asymmetric penalty on
        over-correction — penalize |pred|>|target| more than |pred|<|target|, so
        the model is pushed to under-shoot when uncertain rather than saturate.
      - eq_harmonic_l1_mult: Fix 7 (audio-domain-verified). The per-slot sweep
        (evaluate_student_audio.py + sweep_all_slots.py) proved EQ[0..31] and
        harmonic[0] are the slots that ACTUALLY drive output LUFS — a flat +0.5 EQ
        lift lifts output ~11 dB. loudness[0]/limiter[0] are inert in
        normalizer_mode=0. So to stop the model compensating for quiet input by
        slamming a broad EQ/exciter boost, weight the L1 restraint on those slots
        higher than the rest. >1 = stronger restraint on EQ+harmonic; 1 = uniform.
    """

    def __init__(self, eq_count: int, eq_smooth_weight: float = 0.05,
                 delta_l1_weight: float = 0.02, overcorrect_weight: float = 0.0,
                 eq_harmonic_l1_mult: float = 1.0, harmonic_slice: tuple = HARMONIC_SLICE) -> None:
        super().__init__()
        self.eq_count = eq_count
        self.eq_smooth_weight = eq_smooth_weight
        self.delta_l1_weight = delta_l1_weight
        self.overcorrect_weight = overcorrect_weight
        self.eq_harmonic_l1_mult = float(eq_harmonic_l1_mult)
        # Per-slot weight mask for the L1 term: 1.0 everywhere, mult on EQ[0:32]
        # and harmonic slots (the audio-domain-verified LUFS drivers).
        w = torch.ones(OUTPUT_DELTA_COUNT)
        if self.eq_harmonic_l1_mult != 1.0:
            w[:eq_count] = self.eq_harmonic_l1_mult            # EQ[0..31]
            w[harmonic_slice[0]:harmonic_slice[1]] = self.eq_harmonic_l1_mult  # harmonic[0..7]
        self.register_buffer("l1_weights", w)

    def forward(self, pred: torch.Tensor, target: torch.Tensor) -> tuple[torch.Tensor, dict[str, float]]:
        mse = nn.functional.mse_loss(pred, target)
        eq_pred = pred[:, : self.eq_count]
        eq_target = target[:, : self.eq_count]
        # Adjacent-band jump magnitude — penalise on prediction (and lightly on
        # target so the term doesn't fight a legitimately smooth target).
        pred_rough = (eq_pred[:, 1:] - eq_pred[:, :-1]).abs().mean()
        target_rough = (eq_target[:, 1:] - eq_target[:, :-1]).abs().mean()
        smooth = (pred_rough - target_rough).clamp_min(0.0)
        l1 = pred.abs().mean()  # uniform restraint prior toward neutral deltas
        # Fix 7: per-group weighted L1 — extra restraint on EQ+harmonic (LUFS drivers).
        l1_weighted = (pred.abs() * self.l1_weights).mean()
        # Fix 5: asymmetric over-correction penalty. excess = how far |pred|
        # exceeds |target| (only the overshoot counts); symmetric MSE already
        # penalizes under-shoot, so this biases against saturation when uncertain.
        excess = (pred.abs() - target.abs()).clamp_min(0.0).mean()
        loss = (
            mse
            + self.eq_smooth_weight * smooth
            + self.delta_l1_weight * l1_weighted
            + self.overcorrect_weight * excess
        )
        return loss, {
            "mse": float(mse.detach()),
            "eq_smooth": float(smooth.detach()),
            "l1": float(l1.detach()),
            "l1_eqh": float(l1_weighted.detach()),
            "overcorrect": float(excess.detach()),
            "loss": float(loss.detach()),
        }


# ── Dynamics target (feature-driven, proxy-aligned) ──────────────────────────


def _clamp(v: float, lo: float = -0.95, hi: float = 0.95) -> float:
    return max(lo, min(hi, v))


def feature_driven_dynamics(frame: FeatureFrame) -> list[float]:
    """3-band x (threshold, ratio) mastering-bus compression target, emitting the
    EXACT layout the C++ runtime consumes (AutoMasteringEngine.cpp:593-599, the
    `else` branch every neural plan takes since hasCompParams is never set):

        d0/d1 = band0 (threshold, ratio)
        d2/d3 = band1 (threshold, ratio)
        d4/d5 = band2 (threshold, ratio)
        d6/d7 = unused

    C++ decode (per band b, base=2*b):
        thresholdDB = -20 + v*8      (clamped [-40, -6])
        ratio       = 3.5 + v*2.5    (clamped [1, 6])
    so the teacher's inverse maps are:  v_thr = (thr_db + 20)/8 ,  v_rat = (ratio - 3.5)/2.5.

    Driven ONLY by runtime-LIVE features with a VERIFIED scale. features.py stubs
    crestFactorDb / monoFoldDownDeltaDb / transientDensity / harmonicRisk to 0.0
    (dead). integrated_lufs (~[-35,-6] dB) and loudness_range (~[0,20]) are live
    and scale-verified on real SSBC audio. spectral_bands ARE live but live in
    LOG/dB scale (~[-120,-55]), so a naive energy sum is scale-ambiguous -- V1
    emits UNIFORM threshold/ratio across all 3 bands (coherent symmetric bus
    compression that scales with loudness); per-band differentiation via a
    log->linear spectral conversion is a V2 follow-up. Abstains (midpoint zeros)
    only on genuine near-silence (lufs < -45). Bounded [-0.95, 0.95].
    """
    lufs = getattr(frame, "integrated_lufs", -16.0)
    lra = getattr(frame, "loudness_range", 6.0)

    if lufs < -45.0:                       # genuine near-silence -> abstain to midpoint
        return [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]

    # Overall compression need: louder and/or wider material -> compress more.
    need = _clamp(math.tanh((lufs + 14.0) / 6.0) + 0.3 * math.tanh((lra - 6.0) / 8.0))
    # Map need[-1,1] -> ratio[2.0,3.5]:1 (always a little glue, never bypassed)
    #                    and thr[-14,-20] dB (louder -> lower threshold -> more GR).
    ratio = 2.0 + 1.5 * (need + 1.0) / 2.0
    thr_db = -14.0 - 6.0 * (need + 1.0) / 2.0
    v_thr = _clamp((thr_db + 20.0) / 8.0)
    v_rat = _clamp((ratio - 3.5) / 2.5)
    return [v_thr, v_rat, v_thr, v_rat, v_thr, v_rat, 0.0, 0.0]  # uniform across 3 bands


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
            spectral_bands=tuple(self.rng.uniform(-75.0, -15.0) for _ in range(SPECTRAL_BAND_COUNT)),  # dB scale, matches C++ magnitudeDB / features.py
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

        eq = []
        for i in range(SPECTRAL_BAND_COUNT):
            # Low bands cut when tilt is bright, highs boosted when dull.
            band_pos = i / (SPECTRAL_BAND_COUNT - 1)  # 0=low ... 1=high
            corrective = math.tanh(-tilt * (band_pos - 0.5) / 3.0) * 0.4
            eq.append(max(-1.0, min(1.0, corrective)))

        loudness_delta = math.tanh((-14.0 - lufs) / 6.0) * 0.6
        stereo_delta = math.tanh((frame.mono_fold_down_delta_db) / 3.0) * 0.2
        harmonic_delta = -math.tanh(frame.harmonic_risk * 2.0) * 0.2
        limiter_delta = max(0.0, math.tanh((-1.0 - frame.true_peak_dbtp) / 2.0)) * 0.3

        out = (
            eq
            + feature_driven_dynamics(frame)   # 8-dim dynamics (per-dim feature-driven, proxy-aligned)
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
            dynamo=False,  # legacy exporter: avoids cuda/cpu FakeTensor mismatch on a GPU-trained model
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
        # Fix 3 (restraint): accept multiple manifests so a null-pair/restraint
        # corpus (e.g. AAM human mixes labelled with --zero-labels) can be
        # concatenated with the correction corpus in one training set. A single
        # path still works (backward-compatible).
        train_paths = [Path(p) for p in args.train_manifest]
        val_paths = [Path(p) for p in args.val_manifest]
        if len(train_paths) == 1:
            train_ds = ManifestDataset(train_paths[0])
        else:
            train_ds = ConcatDataset([ManifestDataset(p) for p in train_paths])
            print(f"concatenated {len(train_paths)} train manifests "
                  f"({sum(len(ManifestDataset(p)) for p in train_paths)} segments)")
        val_ds = ManifestDataset(val_paths[0]) if len(val_paths) == 1 else \
            ConcatDataset([ManifestDataset(p) for p in val_paths])
    else:
        raise ValueError(f"unknown data mode {args.data_mode}")

    train_loader = DataLoader(train_ds, batch_size=args.batch_size, shuffle=True, drop_last=True)
    val_loader = DataLoader(val_ds, batch_size=args.batch_size, shuffle=False)

    model = build_model(gated_head=args.gated_head, zero_init=not args.no_zero_init).to(device)
    print(f"model params: {count_parameters(model):,} (budget ~150k) "
          f"gated_head={args.gated_head} zero_init={not args.no_zero_init}")
    criterion = MasteringControlLoss(
        eq_count=SPECTRAL_BAND_COUNT,
        eq_smooth_weight=args.eq_smooth_weight,
        delta_l1_weight=args.delta_l1_weight,
        overcorrect_weight=args.overcorrect_weight,
        eq_harmonic_l1_mult=args.eq_harmonic_l1_mult,
    ).to(device)
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.learning_rate, weight_decay=args.weight_decay)

    best_val = math.inf
    for epoch in range(args.epochs):
        model.train()
        train_metrics = {"loss": 0.0, "mse": 0.0, "eq_smooth": 0.0, "l1": 0.0, "l1_eqh": 0.0, "overcorrect": 0.0}
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
        val_metrics = {"loss": 0.0, "mse": 0.0, "l1": 0.0, "l1_eqh": 0.0, "overcorrect": 0.0}
        n_val = 0
        with torch.no_grad():
            for feature, delta in val_loader:
                feature = feature.to(device)
                delta = delta.to(device)
                pred = model(feature)
                _, metrics = criterion(pred, delta)
                val_metrics["loss"] += metrics["loss"]
                val_metrics["mse"] += metrics["mse"]
                val_metrics["l1"] += metrics["l1"]
                val_metrics["l1_eqh"] += metrics["l1_eqh"]
                val_metrics["overcorrect"] += metrics["overcorrect"]
                n_val += 1
        val_metrics = {k: v / max(1, n_val) for k, v in val_metrics.items()}

        improved = val_metrics["loss"] < best_val
        best_val = min(best_val, val_metrics["loss"])
        print(
            f"epoch {epoch:3d} | train loss {train_metrics['loss']:.5f} mse {train_metrics['mse']:.5f} "
            f"l1 {train_metrics['l1']:.5f} oc {train_metrics['overcorrect']:.5f} "
            f"| val loss {val_metrics['loss']:.5f} mse {val_metrics['mse']:.5f} "
            f"l1 {val_metrics['l1']:.5f} oc {val_metrics['overcorrect']:.5f}"
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
    p.add_argument("--train-manifest", nargs="+",
                   help="JSONL feature/delta manifest(s). Fix 3: pass multiple to concat a "
                        "null-pair/restraint corpus with the correction corpus.")
    p.add_argument("--val-manifest", nargs="+",
                   help="JSONL feature/delta manifest(s) (manifest mode)")
    p.add_argument("--synthetic-train", type=int, default=2048)
    p.add_argument("--synthetic-val", type=int, default=512)
    p.add_argument("--epochs", type=int, default=20)
    p.add_argument("--batch-size", type=int, default=64)
    p.add_argument("--learning-rate", type=float, default=3e-4)
    p.add_argument("--weight-decay", type=float, default=1e-4)
    p.add_argument("--eq-smooth-weight", type=float, default=0.05)
    p.add_argument("--delta-l1-weight", type=float, default=0.02,
                   help="Fix 1 (restraint): L1 penalty on predicted deltas toward neutral. "
                        "Default 0.02 (on). 0=off; sweep 0.005-0.05 against val MSE + restraint metric.")
    p.add_argument("--overcorrect-weight", type=float, default=0.0,
                   help="Fix 5 (restraint): asymmetric penalty on |pred|>|target|. 0=off; try 0.05-0.1.")
    p.add_argument("--eq-harmonic-l1-mult", type=float, default=1.0,
                   help="Fix 7 (audio-domain-verified): multiplier on the L1 restraint term for EQ[0..31] "
                        "+ harmonic slots — the slots a per-slot render sweep proved actually drive output "
                        "LUFS (loudness[0]/limiter[0] are inert). >1 = stronger restraint on those slots to "
                        "stop broad-EQ/exciter over-boost. 1=uniform. Try 3-8.")
    p.add_argument("--gated-head", action="store_true",
                   help="Fix 4 (restraint): gated residual head — a learned scalar gate can drive "
                        "the output to 0 on already-good audio. Contract stays 63->72/tanh.")
    p.add_argument("--no-zero-init", action="store_true",
                   help="Disable Fix 1 zero-init of the final head (identity start). Off by default.")
    p.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
    p.add_argument("--output-dir", default="runs/control-regressor")
    p.add_argument("--export-onnx", type=Path, default=Path("control_regressor.onnx"))
    p.add_argument("--seed", type=int, default=1337)
    return p.parse_args()


if __name__ == "__main__":
    train(parse_args())
