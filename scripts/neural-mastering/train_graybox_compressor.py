#!/usr/bin/env python3
"""Train the F2 gray-box bus compressor on SolidStateBusComp (Lightning).

Reuses the MultiResolutionSTFT/waveform/mid-side/transient/loudness/stereo
loss suite from train_neural_mastering.MasteringLoss (unmodified) and adds one
compression-specific term: a gain-reduction-curve L1 loss matching the model's
implicit GR envelope to the device's (target-vs-input level in dB). The model
has 5 learnable parameters, so it converges fast in fp32 with a large LR.

On finish it writes learned_params.json — the interpretable payoff (knee width,
attack/release calibration, detector blend, auto-makeup), i.e. the device
characterization the black-box model hides inside 2M weights.

Consumes the manifest from build_solidstatebuscomp_manifest.py (per-item
threshold/attack/release/ratio are the conditioning).
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

import torch
import torch.nn.functional as F

import train_neural_mastering as tm
from graybox_compressor import GrayBoxBusCompressor, _block_stats, _hot_signal


def device_gr_curve(input: torch.Tensor, target: torch.Tensor, hop: int) -> torch.Tensor:
    """Reference gain-reduction (dB, >=0) from the device: target-vs-input RMS
    per control block. Negative level change = positive reduction."""
    eps = 1e-8
    _, rms_i, _ = _block_stats(input, hop)
    _, rms_t, _ = _block_stats(target, hop)
    dev_db = 20.0 * torch.log10(rms_t + eps) - 20.0 * torch.log10(rms_i + eps)
    return (-dev_db).clamp_min(0.0)


class CondDataset(torch.utils.data.Dataset):
    def __init__(self, manifest: Path, split: str, sample_rate: int, segment_samples: int, seed: int, training: bool):
        payload = json.loads(manifest.read_text(encoding="utf-8"))
        raw = payload.get("items", payload if isinstance(payload, list) else [])
        self.base = manifest.parent
        self.items = [it for it in raw if it.get("split", split) == split]
        if not self.items:
            raise ValueError(f"manifest has no items for split '{split}'")
        self.sample_rate = sample_rate
        self.segment_samples = segment_samples
        self.training = training
        self.rng = __import__("random").Random(seed + (0 if training else 10_000))

    def __len__(self) -> int:
        return len(self.items)

    def __getitem__(self, index: int):
        it = self.items[index]
        inp = tm.load_audio(tm.resolve_audio_path(self.base, str(it["inputPath"])), self.sample_rate)
        tgt = tm.load_audio(tm.resolve_audio_path(self.base, str(it["targetPath"])), self.sample_rate)
        inp, tgt = tm.crop_pair(inp, tgt, self.segment_samples, self.training, self.rng)
        cond = {
            "threshold": float(it["threshold"]),
            "ratio": float(it["ratio"]),
            "attack": float(it["attack"]),
            "release": float(it["release"]),
        }
        return inp.contiguous(), tgt.contiguous(), cond


def learned_params(model: GrayBoxBusCompressor) -> dict:
    """Interpretable device characterization from the 5 raw parameters."""
    return {
        "paramCount": sum(p.numel() for p in model.parameters()),
        "knee_db": float(F.softplus(model.log_knee).detach()),
        "attack_scale_s_per_label": float(F.softplus(model.attack_log_scale).detach()),
        "release_scale_s_per_label": float(F.softplus(model.release_log_scale).detach()),
        "detector_rms_blend": float(torch.sigmoid(model.detector_raw).detach()),
        "auto_makeup_fraction": float((torch.sigmoid(model.makeup_raw) * 0.5).detach()),
    }


def train_one_epoch(model, loader, loss_fn, opt, sched, fs, gr_weight, hop, precision, device, scaler, grad_clip):
    model.train()
    avg = tm.MetricAverages.create()
    amp = precision != "fp32"
    dtype = tm.autocast_dtype(precision)
    for inp, tgt, cond in loader:
        inp = inp.to(device); tgt = tgt.to(device)
        cond = {k: v.to(device) for k, v in cond.items()}
        with torch.amp.autocast(device_type=device.type, dtype=dtype, enabled=amp):
            pred, gr = model(inp, cond, fs)
            main_loss, metrics = loss_fn(pred, tgt)
            if gr_weight > 0:
                gr_loss = F.l1_loss(gr.float(), device_gr_curve(inp, tgt, hop))
                metrics["gr_l1"] = float(gr_loss.detach())
                loss = main_loss + gr_weight * gr_loss
            else:
                loss = main_loss
        opt.zero_grad(set_to_none=True)
        if precision == "fp16":
            scaler.scale(loss).backward(); scaler.unscale_(opt)
        else:
            loss.backward()
        if grad_clip > 0:
            torch.nn.utils.clip_grad_norm_(model.parameters(), grad_clip)
        if precision == "fp16":
            scaler.step(opt); scaler.update()
        else:
            opt.step()
        sched.step()
        avg.update(metrics, n=inp.shape[0])
    return avg.mean()


@torch.no_grad()
def validate(model, loader, loss_fn, fs, gr_weight, hop, precision, device, max_batches):
    model.eval()
    avg = tm.MetricAverages.create()
    amp = precision != "fp32"
    dtype = tm.autocast_dtype(precision)
    for i, (inp, tgt, cond) in enumerate(loader):
        if max_batches is not None and i >= max_batches:
            break
        inp = inp.to(device); tgt = tgt.to(device)
        cond = {k: v.to(device) for k, v in cond.items()}
        with torch.amp.autocast(device_type=device.type, dtype=dtype, enabled=amp):
            pred, gr = model(inp, cond, fs)
            _, metrics = loss_fn(pred, tgt)
        metrics["si_sdr"] = float(tm.si_sdr(pred, tgt).mean().detach())
        if gr_weight > 0:
            metrics["gr_l1"] = float(F.l1_loss(gr.float(), device_gr_curve(inp, tgt, hop)).detach())
        avg.update(metrics, n=inp.shape[0])
    return avg.mean()


def _build_loss() -> tm.MasteringLoss:
    return tm.MasteringLoss(1.0, 0.25, 0.25, 0.15, 0.05, 0.1, 0.0, None)


def smoke(fs: float, device: torch.device) -> int:
    """No-data check: a few steps on synthetic audio verify the full loop,
    checkpointing, and learned_params.json writing."""
    model = GrayBoxBusCompressor(hop=64).to(device)
    loss_fn = _build_loss().to(device)
    opt = torch.optim.AdamW(model.parameters(), lr=5e-3)
    samples = 8192
    for step in range(3):
        x = _hot_signal(2, samples, fs).to(device)
        tgt = x * 0.8
        cond = {k: torch.full((2,), v, device=device) for k, v in
                {"threshold": -12.0, "ratio": 4.0, "attack": 10.0, "release": 1.0}.items()}
        with torch.amp.autocast(device_type=device.type, enabled=False):
            pred, gr = model(x, cond, fs)
            main_loss, metrics = loss_fn(pred, tgt)
            gr_loss = F.l1_loss(gr, device_gr_curve(x, tgt, 64))
            loss = main_loss + 0.5 * gr_loss
        opt.zero_grad(set_to_none=True); loss.backward(); opt.step()
        if not math.isfinite(float(loss.detach())):
            print(f"SMOKE FAIL: non-finite loss at step {step}", file=sys.stderr); return 1
    torch.save({"model": model.state_dict(), "params": learned_params(model)}, "graybox_smoke.pt")
    print(json.dumps({"smoke": "PASS", "final_loss": float(loss.detach()), "params": learned_params(model)}, indent=2))
    Path("graybox_smoke.pt").unlink(missing_ok=True)
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--manifest", type=Path)
    p.add_argument("--output-dir", type=Path)
    p.add_argument("--smoke", action="store_true", help="no-data loop check (ignores --manifest)")
    p.add_argument("--epochs", type=int, default=40)
    p.add_argument("--batch-size", type=int, default=8)
    p.add_argument("--learning-rate", type=float, default=5e-3)
    p.add_argument("--segment-seconds", type=float, default=5.46)
    p.add_argument("--sample-rate", type=int, default=48000)
    p.add_argument("--hop", type=int, default=256)
    p.add_argument("--gr-loss-weight", type=float, default=0.5)
    p.add_argument("--grad-clip", type=float, default=1.0)
    p.add_argument("--precision", choices=["fp32", "fp16", "bf16"], default="fp32")
    p.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
    p.add_argument("--num-workers", type=int, default=4)
    p.add_argument("--seed", type=int, default=1337)
    p.add_argument("--train-split", default="train")
    p.add_argument("--val-split", default="val")
    p.add_argument("--validate-batches", type=int)
    args = p.parse_args()

    fs = float(args.sample_rate)
    device = torch.device(args.device)
    tm.set_seed(args.seed)

    if args.smoke:
        return smoke(fs, device)

    seg = int(round(args.segment_seconds * args.sample_rate))
    train_ds = CondDataset(args.manifest, args.train_split, args.sample_rate, seg, args.seed, True)
    val_ds = CondDataset(args.manifest, args.val_split, args.sample_rate, seg, args.seed, False)
    kw = {"batch_size": args.batch_size, "num_workers": args.num_workers, "pin_memory": True,
          "persistent_workers": args.num_workers > 0}
    train_loader = torch.utils.data.DataLoader(train_ds, shuffle=True, drop_last=True, **kw)
    val_loader = torch.utils.data.DataLoader(val_ds, shuffle=False, **kw)

    model = GrayBoxBusCompressor(hop=args.hop).to(device)
    loss_fn = _build_loss().to(device)
    opt = torch.optim.AdamW(model.parameters(), lr=args.learning_rate)
    total = math.ceil(len(train_loader)) * args.epochs
    sched = tm.WarmupCosineScheduler(opt, 200, max(201, total), 0.05)
    scaler = tm.make_grad_scaler(device.type, enabled=args.precision == "fp16")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    log = args.output_dir / "train_log.jsonl"
    best = math.inf
    for epoch in range(args.epochs):
        tr = train_one_epoch(model, train_loader, loss_fn, opt, sched, fs, args.gr_loss_weight,
                             args.hop, args.precision, device, scaler, args.grad_clip)
        vl = validate(model, val_loader, loss_fn, fs, args.gr_loss_weight, args.hop,
                      args.precision, device, args.validate_batches)
        rec = {"epoch": epoch, "train": tr, "validation": vl}
        tm.write_jsonl(log, rec); print(json.dumps(rec, indent=2, sort_keys=True))
        torch.save({"model": model.state_dict(), "params": learned_params(model)}, args.output_dir / "last.pt")
        score = vl.get("loss", math.inf)
        if score < best:
            best = score
            torch.save({"model": model.state_dict(), "params": learned_params(model)}, args.output_dir / "best.pt")
    (args.output_dir / "learned_params.json").write_text(
        json.dumps(learned_params(model), indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print("DONE. learned_params.json written.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
