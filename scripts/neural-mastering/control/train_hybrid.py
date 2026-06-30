#!/usr/bin/env python3
"""Train the hybrid audio-to-parameter mastering control regressor.

Runtime contract is unchanged: feature tensor [1,63] -> control deltas [1,72].
The differentiable DSP chain in this script is an offline training proxy only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
import wave
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
import torch
from torch.utils.data import DataLoader, Dataset

sys.path.insert(0, str(Path(__file__).resolve().parent))

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

from codec import INPUT_FEATURE_COUNT, OUTPUT_DELTA_COUNT, serialize_feature_frame  # noqa: E402
from diff_dsp import DifferentiableMasteringChain  # noqa: E402
from hybrid_loss import HybridLossWeights, HybridMasteringLoss  # noqa: E402
from model import build_model, count_parameters  # noqa: E402
from train import SyntheticTeacher, export_onnx  # noqa: E402


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def _resolve_audio_path(row: dict, audio_root: Path | None, keys: tuple[str, ...]) -> Path | None:
    for key in keys:
        value = row.get(key)
        if isinstance(value, str) and value:
            p = Path(value)
            if not p.is_absolute() and audio_root is not None:
                p = audio_root / p
            return p
    return None


def _read_wav_stdlib(path: Path) -> tuple[np.ndarray, int]:
    with wave.open(str(path), "rb") as wf:
        channels = wf.getnchannels()
        sr = wf.getframerate()
        width = wf.getsampwidth()
        frames = wf.readframes(wf.getnframes())
    if width != 2:
        raise ValueError(f"stdlib WAV fallback only supports 16-bit PCM, got sample width {width}")
    data = np.frombuffer(frames, dtype="<i2").astype(np.float32) / 32768.0
    data = data.reshape(-1, channels).T
    return data, sr


def read_audio(path: Path) -> tuple[np.ndarray, int]:
    if not path.exists():
        raise FileNotFoundError(f"audio file not found: {path}")
    try:
        import soundfile as sf

        data, sr = sf.read(str(path), always_2d=True, dtype="float32")
        return data.T.astype(np.float32), int(sr)
    except Exception:
        if path.suffix.lower() == ".wav":
            return _read_wav_stdlib(path)
        raise


def resample_linear(audio: np.ndarray, src_sr: int, dst_sr: int) -> np.ndarray:
    if src_sr == dst_sr:
        return audio.astype(np.float32, copy=False)
    n_in = audio.shape[-1]
    n_out = max(1, int(round(n_in * float(dst_sr) / float(src_sr))))
    x_old = np.linspace(0.0, 1.0, n_in, endpoint=False)
    x_new = np.linspace(0.0, 1.0, n_out, endpoint=False)
    return np.stack([np.interp(x_new, x_old, ch).astype(np.float32) for ch in audio])


def load_segment(path: Path, start_sample: int, segment_samples: int, sample_rate: int) -> torch.Tensor:
    audio, sr = read_audio(path)
    audio = resample_linear(audio, sr, sample_rate)
    if audio.shape[0] == 1:
        audio = np.repeat(audio, 2, axis=0)
    elif audio.shape[0] > 2:
        audio = audio[:2]

    start = max(0, int(start_sample))
    seg = audio[:, start : start + segment_samples]
    if seg.shape[1] < segment_samples:
        seg = np.pad(seg, ((0, 0), (0, segment_samples - seg.shape[1])))
    return torch.tensor(seg, dtype=torch.float32)


class HybridManifestDataset(Dataset):
    def __init__(self, manifest: Path, audio_root: Path | None, segment_seconds: float, sample_rate: int) -> None:
        self.manifest = manifest
        self.audio_root = audio_root
        self.segment_samples = int(round(segment_seconds * sample_rate))
        self.sample_rate = int(sample_rate)
        self.items: list[dict] = []

        missing_audio = 0
        with manifest.open("r", encoding="utf-8") as handle:
            for line_no, line in enumerate(handle, start=1):
                line = line.strip()
                if not line:
                    continue
                row = json.loads(line)
                feature = row.get("feature")
                delta = row.get("delta")
                if not isinstance(feature, list) or len(feature) != INPUT_FEATURE_COUNT:
                    raise ValueError(f"{manifest}:{line_no} feature length != {INPUT_FEATURE_COUNT}")
                if not isinstance(delta, list) or len(delta) != OUTPUT_DELTA_COUNT:
                    raise ValueError(f"{manifest}:{line_no} delta length != {OUTPUT_DELTA_COUNT}")
                audio_path = _resolve_audio_path(
                    row,
                    audio_root,
                    ("sourcePath", "audioPath", "inputPath", "path", "input_audio", "input"),
                )
                if audio_path is None:
                    missing_audio += 1
                row["_audioPath"] = str(audio_path) if audio_path is not None else ""
                target_path = _resolve_audio_path(
                    row,
                    audio_root,
                    ("targetPath", "targetAudioPath", "gtPath", "referencePath", "target_audio", "gt"),
                )
                row["_targetPath"] = str(target_path) if target_path is not None else ""
                self.items.append(row)

        if not self.items:
            raise ValueError(f"manifest {manifest} contained no rows")
        if missing_audio:
            raise ValueError(
                f"manifest {manifest} has {missing_audio} rows without an audio reference; "
                "hybrid training requires sourcePath/audioPath/inputPath/path or --data-mode synthetic"
            )

    def __len__(self) -> int:
        return len(self.items)

    def __getitem__(self, idx: int):
        row = self.items[idx]
        feature = torch.tensor(row["feature"], dtype=torch.float32)
        delta = torch.tensor(row["delta"], dtype=torch.float32)
        start = int(row.get("startSample", 0))
        audio = load_segment(Path(row["_audioPath"]), start, self.segment_samples, self.sample_rate)

        has_target = bool(row.get("_targetPath"))
        if has_target:
            target_audio = load_segment(Path(row["_targetPath"]), start, self.segment_samples, self.sample_rate)
        else:
            target_audio = torch.zeros_like(audio)

        target_lufs = float(row.get("targetLufs", row.get("target_lufs", -14.0)))
        return feature, delta, audio, target_audio, torch.tensor(has_target, dtype=torch.bool), torch.tensor(target_lufs)


class SyntheticHybridDataset(Dataset):
    def __init__(self, size: int, sample_rate: int, segment_seconds: float, seed: int) -> None:
        self.size = int(size)
        self.sample_rate = int(sample_rate)
        self.segment_samples = int(round(segment_seconds * sample_rate))
        self.teacher = SyntheticTeacher(seed=seed)
        self.seed = int(seed)

    def __len__(self) -> int:
        return self.size

    def __getitem__(self, idx: int):
        frame = self.teacher.sample_frame()
        feature = torch.tensor(serialize_feature_frame(frame), dtype=torch.float32)
        delta = torch.tensor(self.teacher.teacher_deltas(frame), dtype=torch.float32)

        t = torch.arange(self.segment_samples, dtype=torch.float32) / float(self.sample_rate)
        base = 110.0 + float((idx * 37) % 660)
        left = 0.12 * torch.sin(2.0 * math.pi * base * t)
        right = 0.10 * torch.sin(2.0 * math.pi * (base * 1.005) * t + 0.2)
        gen = torch.Generator().manual_seed(self.seed + idx)
        noise = 0.01 * torch.randn((2, self.segment_samples), generator=gen)
        audio = torch.stack((left, right), dim=0) + noise
        return feature, delta, audio, torch.zeros_like(audio), torch.tensor(False), torch.tensor(-14.0)


def make_datasets(args: argparse.Namespace) -> tuple[Dataset, Dataset]:
    if args.data_mode == "synthetic":
        return (
            SyntheticHybridDataset(args.synthetic_train, args.sample_rate, args.segment_seconds, args.seed),
            SyntheticHybridDataset(args.synthetic_val, args.sample_rate, args.segment_seconds, args.seed + 101),
        )
    audio_root = Path(args.audio_root).resolve() if args.audio_root else None
    return (
        HybridManifestDataset(Path(args.train_manifest), audio_root, args.segment_seconds, args.sample_rate),
        HybridManifestDataset(Path(args.val_manifest), audio_root, args.segment_seconds, args.sample_rate),
    )


def run_epoch(model, loader, criterion, optimizer, device: torch.device, train: bool) -> dict[str, float]:
    model.train(train)
    totals: dict[str, float] = {}
    batches = 0
    for feature, delta, audio, target_audio, has_target_audio, _target_lufs in loader:
        feature = feature.to(device)
        delta = delta.to(device)
        audio = audio.to(device)
        target_audio = target_audio.to(device)
        has_target_audio = has_target_audio.to(device)

        with torch.set_grad_enabled(train):
            pred = model(feature)
            loss, metrics = criterion(audio, pred, delta, target_audio, has_target_audio)
            if train:
                optimizer.zero_grad(set_to_none=True)
                loss.backward()
                torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
                optimizer.step()

        for key, value in metrics.items():
            totals[key] = totals.get(key, 0.0) + float(value)
        batches += 1
    return {key: value / max(1, batches) for key, value in totals.items()}


def evaluate_headless(model, dataset: Dataset, args: argparse.Namespace, device: torch.device) -> dict[str, float]:
    if not args.headless_lib:
        return {}

    from morephi_render import HeadlessRenderer

    host = HeadlessRenderer(args.headless_lib, sample_rate=float(args.sample_rate), block_size=512, normalizer_mode=0)
    n = min(int(args.headless_limit), len(dataset))
    eq_mag = []
    overcorrect = []
    lufs_error = []
    true_peak_violations = 0
    model.eval()
    try:
        for idx in range(n):
            feature, delta, audio, _target_audio, _has_target, target_lufs = dataset[idx]
            with torch.no_grad():
                pred = model(feature.view(1, -1).to(device)).cpu().numpy().reshape(OUTPUT_DELTA_COUNT).astype(np.float32)
            source = audio.numpy().astype(np.float32)
            interleaved = np.empty(source.shape[1] * 2, dtype=np.float32)
            interleaved[0::2] = source[0]
            interleaved[1::2] = source[1]
            _rendered, meters = host.render_candidate(interleaved, pred)
            lufs_error.append(abs(float(meters.lufs_integrated) - float(target_lufs)))
            true_peak_violations += int(float(meters.true_peak_dbtp) > -1.0)
            eq_mag.append(float(np.mean(np.abs(pred[:32] * 12.0))))
            label = delta.numpy()
            overcorrect.append(float(np.mean(np.abs(pred) > (np.abs(label) + 0.05))))
    finally:
        host.close()

    return {
        "headless_count": float(n),
        "headless_lufs_error": float(np.mean(lufs_error)) if lufs_error else 0.0,
        "headless_true_peak_violations": float(true_peak_violations),
        "headless_eq_gain_mae_db": float(np.mean(eq_mag)) if eq_mag else 0.0,
        "headless_overcorrection_rate": float(np.mean(overcorrect)) if overcorrect else 0.0,
    }


def write_model_card(path: Path, args: argparse.Namespace, weights: HybridLossWeights, metrics: dict[str, float]) -> None:
    manifests = []
    for label, maybe_path in (("train", args.train_manifest), ("val", args.val_manifest)):
        if maybe_path:
            p = Path(maybe_path)
            manifests.append({"split": label, "path": str(p), "sha256": sha256_file(p) if p.exists() else None})

    card = {
        "schema": 1,
        "createdUtc": datetime.now(timezone.utc).isoformat(),
        "trainingMode": "hybrid_param_policy",
        "evidenceLevel": "PrototypeMeasured",
        "runtimeIntegration": "offline_export_existing_onnx_runner",
        "inputFeatureCount": INPUT_FEATURE_COUNT,
        "outputDeltaCount": OUTPUT_DELTA_COUNT,
        "featureSchemaVersion": 1,
        "outputSchemaVersion": 1,
        "audioCallbackInference": False,
        "lossWeights": weights.as_dict(),
        "sampleRate": args.sample_rate,
        "segmentSeconds": args.segment_seconds,
        "manifests": manifests,
        "exportOnnx": str(args.export_onnx) if args.export_onnx else None,
        "exportOnnxSha256": sha256_file(Path(args.export_onnx)) if args.export_onnx and Path(args.export_onnx).exists() else None,
        "metrics": metrics,
        "notes": [
            "Differentiable DSP is an offline training proxy only.",
            "Deployment still requires headless-render validation against the real More-Phi chain.",
        ],
    }
    path.write_text(json.dumps(card, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def train(args: argparse.Namespace) -> dict[str, float]:
    torch.manual_seed(args.seed)
    device = torch.device(args.device)
    train_ds, val_ds = make_datasets(args)
    nw = max(0, int(args.num_workers))
    dl_common = dict(
        num_workers=nw,
        pin_memory=str(args.device).startswith("cuda"),
        persistent_workers=nw > 0,
    )
    train_loader = DataLoader(train_ds, batch_size=args.batch_size, shuffle=True,
                              drop_last=len(train_ds) >= args.batch_size, **dl_common)
    val_loader = DataLoader(val_ds, batch_size=args.batch_size, shuffle=False, **dl_common)

    model = build_model(gated_head=args.gated_head, zero_init=not args.no_zero_init).to(device)
    weights = HybridLossWeights(
        param=args.param_weight,
        spectral=args.spectral_weight,
        lufs=args.lufs_weight,
        true_peak=args.true_peak_weight,
        eq_smooth=args.eq_smooth_weight,
        restraint=args.restraint_weight,
    )
    criterion = HybridMasteringLoss(sample_rate=args.sample_rate, weights=weights, ceiling_dbtp=args.ceiling_dbtp).to(device)
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.learning_rate, weight_decay=args.weight_decay)

    print(f"model params: {count_parameters(model):,} data_mode={args.data_mode} hybrid_proxy=on")
    best_val = float("inf")
    best_state = None
    final_metrics: dict[str, float] = {}
    for epoch in range(args.epochs):
        train_metrics = run_epoch(model, train_loader, criterion, optimizer, device, train=True)
        val_metrics = run_epoch(model, val_loader, criterion, optimizer, device, train=False)
        improved = val_metrics.get("loss", float("inf")) < best_val
        if improved:
            best_val = val_metrics["loss"]
            best_state = {k: v.detach().cpu().clone() for k, v in model.state_dict().items()}
        final_metrics = {f"train_{k}": v for k, v in train_metrics.items()}
        final_metrics.update({f"val_{k}": v for k, v in val_metrics.items()})
        print(
            f"epoch {epoch:3d} | train loss {train_metrics.get('loss', 0.0):.5f} "
            f"param {train_metrics.get('param', 0.0):.5f} spectral {train_metrics.get('spectral', 0.0):.5f} "
            f"| val loss {val_metrics.get('loss', 0.0):.5f} param {val_metrics.get('param', 0.0):.5f} "
            f"spectral {val_metrics.get('spectral', 0.0):.5f}{' *' if improved else ''}"
        )

    if best_state is not None:
        model.load_state_dict(best_state)

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    torch.save({"model": model.state_dict(), "args": vars(args), "metrics": final_metrics}, output_dir / "control_regressor_hybrid.pt")

    if args.export_onnx:
        export_path = Path(args.export_onnx)
        export_path.parent.mkdir(parents=True, exist_ok=True)
        export_onnx(model, export_path)
        print(f"exported ONNX -> {export_path}")

    # ponytail: the ONNX + checkpoint are already written above, so a headless-render
    # failure (ctypes/.so/NaN) must not abort the run or lose the model card. Log + skip.
    try:
        headless_metrics = evaluate_headless(model, val_ds, args, device)
    except Exception as exc:  # noqa: BLE001 — degrade to empty metrics, keep the card
        print(f"headless validation SKIPPED: {exc!r}")
        headless_metrics = {}
    if headless_metrics:
        print("headless validation: " + json.dumps(headless_metrics, sort_keys=True))
        final_metrics.update(headless_metrics)

    card_path = Path(args.model_card) if args.model_card else Path(args.export_onnx).with_suffix(".model-card.json")
    card_path.parent.mkdir(parents=True, exist_ok=True)
    write_model_card(card_path, args, weights, final_metrics)
    print(f"wrote model card -> {card_path}")
    return final_metrics


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--data-mode", choices=["manifest", "synthetic"], default="manifest")
    p.add_argument("--train-manifest")
    p.add_argument("--val-manifest")
    p.add_argument("--audio-root", help="base directory for relative audio paths in manifest rows")
    p.add_argument("--sample-rate", type=int, default=48000)
    p.add_argument("--segment-seconds", type=float, default=4.0)
    p.add_argument("--synthetic-train", type=int, default=64)
    p.add_argument("--synthetic-val", type=int, default=16)
    p.add_argument("--epochs", type=int, default=20)
    p.add_argument("--batch-size", type=int, default=8)
    p.add_argument("--learning-rate", type=float, default=3e-4)
    p.add_argument("--weight-decay", type=float, default=1e-4)
    p.add_argument("--param-weight", type=float, default=1.0)
    p.add_argument("--spectral-weight", type=float, default=1.0)
    p.add_argument("--lufs-weight", type=float, default=0.5)
    p.add_argument("--true-peak-weight", type=float, default=5.0)
    p.add_argument("--eq-smooth-weight", type=float, default=0.05)
    p.add_argument("--restraint-weight", type=float, default=0.02)
    p.add_argument("--ceiling-dbtp", type=float, default=-1.0)
    p.add_argument("--gated-head", action="store_true")
    p.add_argument("--no-zero-init", action="store_true")
    p.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
    p.add_argument("--output-dir", default="runs/control-regressor-hybrid")
    p.add_argument("--export-onnx", type=Path, default=Path("control_regressor_hybrid.onnx"))
    p.add_argument("--model-card", type=Path)
    p.add_argument("--headless-lib", help="optional libmore_phi_headless_render path for final acceptance metrics")
    p.add_argument("--headless-limit", type=int, default=8)
    p.add_argument("--num-workers", type=int, default=0,
                   help="DataLoader worker processes (0=main thread). Real-audio manifest mode "
                        "benefits most; synthetic is GPU-bound. pin_memory auto-on for cuda.")
    p.add_argument("--seed", type=int, default=1337)
    args = p.parse_args(argv)
    if args.data_mode == "manifest" and (not args.train_manifest or not args.val_manifest):
        p.error("--train-manifest and --val-manifest are required in manifest mode")
    return args


if __name__ == "__main__":
    train(parse_args())
