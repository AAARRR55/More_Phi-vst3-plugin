#!/usr/bin/env python3
"""Train the FiLM-conditioned black-box forward model (F1) on SolidStateBusComp.

Consumes the manifest from build_solidstatebuscomp_manifest.py. Conditioning is
cond = [c, e] (P4: 8-dim): c = compressor (threshold, attack, release, ratio),
normalized with stats computed from the TRAIN items only; e = EQ (low/mid/high/q),
normalized analytically from its sampling distribution (q mean = 1.0). The P4
synthetic target is rendered POST-DEVICE: peak_normalize(min_phase_eq(device_target;
e), 0.98) — the SAME e labels the cond, so the §1.1 "denoise-the-EQ" incoherence is
dissolved (residual ceiling = EQ/comp non-commutativity, provenance 'synthetic-eq').

Losses:
  - MasteringLoss (8-term suite, reused from train_neural_mastering) on the
    forward prediction;
  - a gain-reduction-curve L1 loss (model's implicit GR vs the device's);
  - an inverse-head MSE on c only (audio -> predicted compressor params; e masked).

Validation: SI-SDR partitioned by holdoutAxis (P2 interp/extrap gate); validate_under_eq
input-robustness probe (P3); validate_eq_sweep e-responsiveness gate (P4) — the model
must reproduce the synthetic EQ'd target across an e sweep. None touch vl['loss'].
"""

from __future__ import annotations

import argparse
import json
import math
import random
import sys
from pathlib import Path

import torch
import torch.nn.functional as F

import train_neural_mastering as tm
from conditioned_mastering_net import ConditionedHybridMasteringNet
from train_graybox_compressor import device_gr_curve
from graybox_compressor import _hot_signal
import eq_augment

PARAM_KEYS = ["threshold", "attack", "release", "ratio"]


def compute_stats(items: list[dict], eq_augment_db: float = 6.0) -> dict:
    """cond = [c(4), e(4)] = 8-dim. c-stats are EMPIRICAL over manifest rows; e-stats
    are ANALYTICAL from the sampling distribution (e is drawn per-epoch, never stored,
    so a manifest scan would have nothing to read).

    ponytail: q mean is 1.0 (the identity), NOT 0 — sample_eq_gains draws q~U(0.5,1.5),
    so the identity q=1.0 is the distribution centre. Using mean_q=0 would break the
    e=identity <-> cond_norm=0 mapping that anchors FiLM identity-init and the gate's
    'flat' continuity check."""
    mean, std = [], []
    for k in PARAM_KEYS:
        v = [float(it[k]) for it in items]
        m = sum(v) / len(v)
        s = math.sqrt(sum((x - m) ** 2 for x in v) / len(v))
        mean.append(m)
        std.append(max(s, 1e-6))
    m = max(eq_augment_db, 1e-6)
    mean += [0.0, 0.0, 0.0, 1.0]                            # e: low/mid/high mean 0; q mean 1.0
    std += [m / math.sqrt(3)] * 3 + [1.0 / math.sqrt(12)]   # U(-m,m)->m/sqrt3 ; U(0.5,1.5)->1/sqrt12
    return {"mean": torch.tensor(mean), "std": torch.tensor(std)}


class CondDataset(torch.utils.data.Dataset):
    def __init__(self, manifest: Path, split: str, sample_rate: int, segment_samples: int,
                 seed: int, training: bool, stats: dict,
                 eq_augment: bool = True, eq_augment_db: float = 6.0):
        payload = json.loads(manifest.read_text(encoding="utf-8"))
        raw = payload.get("items", payload if isinstance(payload, list) else [])
        self.items = [it for it in raw if it.get("split", split) == split]
        if not self.items:
            raise ValueError(f"manifest has no items for split '{split}'")
        self.base = manifest.parent
        self.sample_rate = sample_rate
        self.segment_samples = segment_samples
        self.training = training
        self.rng = random.Random(seed + (0 if training else 10_000))
        self.mean, self.std = stats["mean"], stats["std"]
        self.eq_augment = eq_augment
        self.eq_augment_db = eq_augment_db

    def __len__(self) -> int:
        return len(self.items)

    def __getitem__(self, index: int):
        it = self.items[index]
        inp = tm.load_audio(tm.resolve_audio_path(self.base, str(it["inputPath"])), self.sample_rate)
        tgt = tm.load_audio(tm.resolve_audio_path(self.base, str(it["targetPath"])), self.sample_rate)
        inp, tgt = tm.crop_pair(inp, tgt, self.segment_samples, self.training, self.rng)
        # P4 (plan §1.2): EQ is now a CO-CONDITIONING AXIS and the synthetic target is
        # rendered POST-DEVICE through the SAME e that labels the cond. The hardware
        # device is unreachable, so the causally-correct PRE-comp target device(EQ(x;e))
        # is uncomputable; POST-EQ renders EQ(device_target;e), which keeps the REAL
        # device compression in the target and is continuous at e=identity (min_phase_eq
        # is passthrough at 0 dB -> target == device_target). This DISSOLVES the P3 §1.1
        # "denoise-the-EQ" label/target incoherence: the model is asked to PRODUCE
        # EQ(device_target;e), conditioned on e — the objective flips from invariance to
        # responsiveness. Residual ceiling (provenance 'synthetic-eq'): EQ/comp
        # non-commutativity (post-comp EQ != pre-comp EQ). Draw e ONCE per item per
        # epoch, threaded into BOTH the target render and the cond vector.
        if self.training and self.eq_augment and self.eq_augment_db > 0:
            low, mid, high, q = eq_augment.sample_eq_gains(self.rng, self.eq_augment_db)
        else:
            low = mid = high = 0.0; q = 1.0                 # identity: val split + --no-eq-augment
        tgt = tm.peak_normalize(eq_augment.min_phase_eq(tgt, self.sample_rate, low, mid, high, q), 0.98)
        raw = torch.tensor([float(it[k]) for k in PARAM_KEYS] + [low, mid, high, q])  # [c(4), e(4)] c-first
        cond_norm = (raw - self.mean) / self.std
        return inp.contiguous(), tgt.contiguous(), cond_norm, float(it.get("holdoutAxis", False))


def _build_loss() -> tm.MasteringLoss:
    return tm.MasteringLoss(1.0, 0.25, 0.25, 0.15, 0.05, 0.1, 0.0, None)


def train_one_epoch(model, loader, loss_fn, opt, sched, fs, gr_w, inv_w, hop, precision, device, scaler, grad_clip):
    model.train()
    avg = tm.MetricAverages.create()
    amp = precision != "fp32"
    dtype = tm.autocast_dtype(precision)
    for inp, tgt, cond, _holdout in loader:
        inp = inp.to(device); tgt = tgt.to(device); cond = cond.to(device)
        with torch.amp.autocast(device_type=device.type, dtype=dtype, enabled=amp):
            pred = model(inp, cond)
            inv = model.inverse(tgt)
            main_loss, metrics = loss_fn(pred, tgt)
            gr_loss = F.l1_loss(device_gr_curve(inp, pred, hop), device_gr_curve(inp, tgt, hop))
            # P4: mask e from inv_loss — recovering the EQ tilt (a deconvolution) from the
            # synthetic target is harder/less stable than recovering c. Supervise c only;
            # measure inv_mse split by [c,e] halves on the first run, promote to full 8-dim
            # if e-recovery proves stable.
            inv_loss = F.mse_loss(inv[:, :4], cond[:, :4])
            metrics["gr_l1"] = float(gr_loss.detach()); metrics["inv_mse"] = float(inv_loss.detach())
            loss = main_loss + gr_w * gr_loss + inv_w * inv_loss
        opt.zero_grad(set_to_none=True)
        (scaler.scale(loss) if precision == "fp16" else loss).backward()
        if precision == "fp16":
            scaler.unscale_(opt)
        if grad_clip > 0:
            torch.nn.utils.clip_grad_norm_(model.parameters(), grad_clip)
        (scaler.step(opt), scaler.update()) if precision == "fp16" else opt.step()
        sched.step()
        avg.update(metrics, n=inp.shape[0])
    return avg.mean()


@torch.no_grad()
def validate(model, loader, loss_fn, fs, gr_w, inv_w, hop, precision, device, max_batches):
    model.eval()
    avg = tm.MetricAverages.create()
    si_interp, si_extrap = [], []
    amp = precision != "fp32"
    dtype = tm.autocast_dtype(precision)
    for i, (inp, tgt, cond, holdout) in enumerate(loader):
        if max_batches is not None and i >= max_batches:
            break
        inp = inp.to(device); tgt = tgt.to(device); cond = cond.to(device)
        with torch.amp.autocast(device_type=device.type, dtype=dtype, enabled=amp):
            pred = model(inp, cond)
            _, metrics = loss_fn(pred, tgt)
            metrics["gr_l1"] = float(F.l1_loss(device_gr_curve(inp, pred, hop), device_gr_curve(inp, tgt, hop)).detach())
            metrics["inv_mse"] = float(F.mse_loss(model.inverse(tgt)[:, :4], cond[:, :4]).detach())  # c-only (P4 e-mask)
        per_item_si = tm.si_sdr(pred, tgt)            # [B]
        metrics["si_sdr"] = float(per_item_si.mean().detach())
        avg.update(metrics, n=inp.shape[0])
        for b, h in enumerate(holdout.tolist()):
            (si_extrap if h else si_interp).append(float(per_item_si[b].detach()))
    out = avg.mean()
    out["si_sdr_interp"] = sum(si_interp) / max(1, len(si_interp))
    out["si_sdr_extrap"] = sum(si_extrap) / max(1, len(si_extrap))
    return out


@torch.no_grad()
def validate_under_eq(model, loader, fs, precision, device, max_batches, probe_seed,
                      proxy=None, mean=None, std=None):
    """P3 robustness probe (plan §6): perturb each held-out val INPUT by a fixed
    +-6 dB EQ grid and measure SI-SDR vs the ORIGINAL device target. Reports ONLY
    separate `eq_probe` keys — never touches vl['loss'] (best-checkpoint stays on
    the clean metric). Inherits validate()'s holdoutAxis interp/extrap partition.

    ponytail: GATE = EQ-ROBUSTNESS, not device-robustness. Good si_sdr_robust_drop
    can coexist with the model having learned to invert EQ; the gate is meaningful
    only if an AUGMENTED run lowers robust_drop vs the un-augmented baseline floor.
    Optional proxy (GrayBoxBusCompressor, --eq-proxy) is a strictly eval-only
    ceiling indicator D_proxy(A(x)): how device-relevant (GR-coupled) the EQ delta
    is vs 'denoise-the-EQ' waste — never a training target."""
    model.eval()
    grid = eq_augment.eq_probe_grid()
    amp = precision != "fp32"
    dtype = tm.autocast_dtype(precision)
    sr = int(round(fs))
    clean_all, clean_interp, clean_extrap, flat_all = [], [], [], []
    pert_all, pert_interp, pert_extrap, proxy_all = [], [], [], []
    dev_mean = mean.to(device) if mean is not None else None
    dev_std = std.to(device) if std is not None else None
    for i, (inp, tgt, cond, holdout) in enumerate(loader):
        if max_batches is not None and i >= max_batches:
            break
        inp = inp.to(device); tgt = tgt.to(device); cond = cond.to(device)
        with torch.amp.autocast(device_type=device.type, dtype=dtype, enabled=amp):
            pred_clean = model(inp, cond)
        for b, h in enumerate(holdout.tolist()):
            v = float(tm.si_sdr(pred_clean, tgt)[b])
            clean_all.append(v); (clean_extrap if h else clean_interp).append(v)
        cd = None
        if proxy is not None:                       # de-normalize -> raw device labels (cond is 8-dim [c,e])
            cond_raw = cond * dev_std + dev_mean
            c_raw = cond_raw[:, :4]                 # ponytail: pin c-first; reordering [e,c] would feed EQ gains to the proxy
            cd = {"threshold": c_raw[:, 0], "attack": c_raw[:, 1], "release": c_raw[:, 2], "ratio": c_raw[:, 3]}
        for label, (lo, mi, hi, q) in grid:
            inp_p = tm.peak_normalize(eq_augment.min_phase_eq(inp, sr, lo, mi, hi, q), 0.98)
            with torch.amp.autocast(device_type=device.type, dtype=dtype, enabled=amp):
                pred_p = model(inp_p, cond)
            si_p = tm.si_sdr(pred_p, tgt).tolist()
            if label == "flat":
                flat_all.extend(si_p)
            else:
                for b, h in enumerate(holdout.tolist()):
                    pert_all.append(si_p[b]); (pert_extrap if h else pert_interp).append(si_p[b])
            if proxy is not None:
                with torch.amp.autocast(device_type=device.type, dtype=dtype, enabled=amp):
                    y_proxy, _ = proxy(inp_p, cd, fs)
                proxy_all.extend(tm.si_sdr(pred_p, y_proxy).tolist())

    def avg(xs):
        return sum(xs) / len(xs) if xs else float("nan")
    out = {
        "si_sdr_clean": avg(clean_all),
        "si_sdr_flat": avg(flat_all),
        "flat_check_drop": avg(clean_all) - avg(flat_all),     # wiring-bug catcher: ~0
        "si_sdr_perturbed_mean": avg(pert_all),
        "si_sdr_robust_drop": avg(clean_all) - avg(pert_all),
        "si_sdr_robust_drop_interp": avg(clean_interp) - avg(pert_interp),
        "si_sdr_robust_drop_extrap": avg(clean_extrap) - avg(pert_extrap),
    }
    if proxy is not None:
        out["proxy_si_mean"] = avg(proxy_all)
    return out


def _band_energy(x: torch.Tensor, sr: int) -> tuple[float, float]:
    """Low-band (<250 Hz) and high-band (>4 kHz) energy of [B,2,T] via rFFT of the
    mono sum. Used only for the e-sweep monotonicity sign check in validate_eq_sweep."""
    m = (x[:, 0] + x[:, 1]) * 0.5
    X = torch.fft.rfft(m)
    p = (X.real ** 2 + X.imag ** 2).sum(dim=0)            # [F] — aggregate energy over batch
    freqs = torch.fft.rfftfreq(m.shape[-1], 1.0 / sr)
    return p[freqs < 250.0].sum().item(), p[freqs > 4000.0].sum().item()


@torch.no_grad()
def validate_eq_sweep(model, loader, fs, precision, device, max_batches, mean, std):
    """P4 gate (plan §6): predictable output response to an e-conditioning sweep.
    POST-EQ: EQ lives in the TARGET, so the input is UNPERTURBED and e enters only via
    cond. For each val item (fixed real c), sweep e over eq_probe_grid(); the model must
    reproduce peak_normalize(min_phase_eq(device_target; e), 0.98). The 'flat' entry
    (e=identity) reproduces the clean pass -> flat_check_drop ~ 0 is the wiring/
    normalization catcher (a non-zero value flags a cond ordering or q-mean bug).
    mono_*_up_frac check that low+6 raises the low band and high+6 raises the high band
    (correct sign of the EQ response). Reports ONLY separate `eq_sweep` keys; never
    touches vl['loss']."""
    model.eval()
    grid = eq_augment.eq_probe_grid()
    amp = precision != "fp32"
    dtype = tm.autocast_dtype(precision)
    sr = int(round(fs))
    e_mean = mean[4:].to(device)            # e-half of stats (q mean = 1.0)
    e_std = std[4:].to(device)
    per_sweep_si = {label: [] for label, _ in grid}
    clean_all = []
    low_up = high_up = 0
    n_mono = 0
    for i, (inp, tgt, cond, _holdout) in enumerate(loader):
        if max_batches is not None and i >= max_batches:
            break
        inp = inp.to(device); tgt = tgt.to(device); cond = cond.to(device)
        c_norm = cond[:, :4]                # fixed real compressor conditioning
        with torch.amp.autocast(device_type=device.type, dtype=dtype, enabled=amp):
            pred_clean = model(inp, cond)   # val cond == [c_norm, 0,0,0,0] (e=identity)
        clean_all.extend(tm.si_sdr(pred_clean, tgt).tolist())
        outs = {}
        for label, (lo, mi, hi, q) in grid:
            e_raw = torch.tensor([lo, mi, hi, q], device=device)
            e_norm = (e_raw - e_mean) / e_std
            cond8 = torch.cat([c_norm, e_norm.unsqueeze(0).expand(c_norm.shape[0], -1)], dim=1)
            with torch.amp.autocast(device_type=device.type, dtype=dtype, enabled=amp):
                pred = model(inp, cond8)
            tgt_e = tm.peak_normalize(eq_augment.min_phase_eq(tgt, sr, lo, mi, hi, q), 0.98)
            per_sweep_si[label].extend(tm.si_sdr(pred, tgt_e).tolist())
            outs[label] = pred
        lo_flat, hi_flat = _band_energy(outs["flat"], sr)
        lo_p6, _ = _band_energy(outs["low+6"], sr)
        _, hi_p6 = _band_energy(outs["high+6"], sr)
        low_up += int(lo_p6 > lo_flat)
        high_up += int(hi_p6 > hi_flat)
        n_mono += 1

    def avg(xs):
        return sum(xs) / len(xs) if xs else float("nan")
    out = {f"si_sdr_e_{label}": avg(v) for label, v in per_sweep_si.items()}
    out["flat_check_drop"] = avg(clean_all) - out["si_sdr_e_flat"]    # ~0 = wiring OK
    out["mono_low_up_frac"] = low_up / max(1, n_mono)                 # low+6 -> low band up
    out["mono_high_up_frac"] = high_up / max(1, n_mono)               # high+6 -> high band up
    return out


def smoke(fs: float, device: torch.device) -> int:
    torch.manual_seed(0)
    model = ConditionedHybridMasteringNet().to(device)
    loss_fn = _build_loss().to(device)
    opt = torch.optim.AdamW(model.parameters(), lr=3e-4)
    for step in range(2):
        x = _hot_signal(2, 8192, fs).to(device)
        tgt = x * 0.8
        cond = torch.randn(2, 8, device=device)
        pred = model(x, cond)
        inv = model.inverse(tgt)
        main_loss, _ = loss_fn(pred, tgt)
        gr_loss = F.l1_loss(device_gr_curve(x, pred, 256), device_gr_curve(x, tgt, 256))
        inv_loss = F.mse_loss(inv, cond)
        loss = main_loss + 0.5 * gr_loss + 0.1 * inv_loss
        opt.zero_grad(set_to_none=True); loss.backward(); opt.step()
        if not math.isfinite(float(loss.detach())):
            print(f"SMOKE FAIL: non-finite loss at step {step}", file=sys.stderr); return 1
    print(json.dumps({"smoke": "PASS", "final_loss": float(loss.detach()),
                      "paramCount_M": round(sum(p.numel() for p in model.parameters()) / 1e6, 3)}, indent=2))
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--manifest", type=Path)
    p.add_argument("--output-dir", type=Path)
    p.add_argument("--smoke", action="store_true")
    p.add_argument("--epochs", type=int, default=80)
    p.add_argument("--batch-size", type=int, default=4)
    p.add_argument("--learning-rate", type=float, default=2e-4)
    p.add_argument("--segment-seconds", type=float, default=5.46)
    p.add_argument("--sample-rate", type=int, default=48000)
    p.add_argument("--hop", type=int, default=256)
    p.add_argument("--gr-loss-weight", type=float, default=0.5)
    p.add_argument("--inverse-loss-weight", type=float, default=0.1)
    p.add_argument("--grad-clip", type=float, default=1.0)
    p.add_argument("--precision", choices=["fp32", "fp16", "bf16"], default="bf16")
    p.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
    p.add_argument("--num-workers", type=int, default=4)
    p.add_argument("--seed", type=int, default=1337)
    p.add_argument("--train-split", default="train")
    p.add_argument("--val-split", default="val")
    p.add_argument("--validate-batches", type=int)
    p.add_argument("--residual-scale", type=float, default=0.25)
    p.add_argument("--eq-augment-db", type=float, default=6.0,
                   help="P3 input-only min-phase EQ augmentation magnitude (dB); 0 disables")
    p.add_argument("--no-eq-augment", action="store_true", help="disable P3 EQ augmentation")
    p.add_argument("--eq-proxy", action="store_true",
                   help="eval-only F2-graybox proxy D(A(x)) ceiling indicator (plan §1.1)")
    p.add_argument("--eq-probe-seed", type=int, default=4242, help="fixed seed for the EQ robustness probe grid")
    args = p.parse_args()

    fs = float(args.sample_rate)
    device = torch.device(args.device)
    tm.set_seed(args.seed)

    if args.smoke:
        return smoke(fs, device)

    seg = int(round(args.segment_seconds * args.sample_rate))
    train_ds = CondDataset(args.manifest, args.train_split, args.sample_rate, seg, args.seed, True, stats=None,  # type: ignore[arg-type]
                           eq_augment=not args.no_eq_augment, eq_augment_db=args.eq_augment_db)
    stats = compute_stats(train_ds.items, args.eq_augment_db)
    train_ds.mean, train_ds.std = stats["mean"], stats["std"]
    val_ds = CondDataset(args.manifest, args.val_split, args.sample_rate, seg, args.seed, False, stats=stats,
                         eq_augment=False, eq_augment_db=0.0)
    kw = {"batch_size": args.batch_size, "num_workers": args.num_workers, "pin_memory": True,
          "persistent_workers": args.num_workers > 0}
    train_loader = torch.utils.data.DataLoader(train_ds, shuffle=True, drop_last=True, **kw)
    val_loader = torch.utils.data.DataLoader(val_ds, shuffle=False, **kw)

    model = ConditionedHybridMasteringNet(residual_scale=args.residual_scale).to(device)
    loss_fn = _build_loss().to(device)
    opt = torch.optim.AdamW(model.parameters(), lr=args.learning_rate, betas=(0.9, 0.95))
    total = math.ceil(len(train_loader)) * args.epochs
    sched = tm.WarmupCosineScheduler(opt, 2000, max(2001, total), 0.05)
    scaler = tm.make_grad_scaler(device.type, enabled=args.precision == "fp16")
    proxy = None
    if args.eq_proxy:                # eval-only F2-graybox ceiling indicator (never a training target)
        from graybox_compressor import GrayBoxBusCompressor
        proxy = GrayBoxBusCompressor(hop=args.hop).to(device).eval()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "cond_stats.json").write_text(
        json.dumps({k: stats[k].tolist() for k in stats}, indent=2) + "\n", encoding="utf-8")
    log = args.output_dir / "train_log.jsonl"
    best = math.inf
    for epoch in range(args.epochs):
        tr = train_one_epoch(model, train_loader, loss_fn, opt, sched, fs, args.gr_loss_weight,
                             args.inverse_loss_weight, args.hop, args.precision, device, scaler, args.grad_clip)
        vl = validate(model, val_loader, loss_fn, fs, args.gr_loss_weight, args.inverse_loss_weight,
                      args.hop, args.precision, device, args.validate_batches)
        probe = validate_under_eq(model, val_loader, fs, args.precision, device, args.validate_batches,
                                  args.eq_probe_seed, proxy=proxy, mean=stats["mean"], std=stats["std"])
        sweep = validate_eq_sweep(model, val_loader, fs, args.precision, device, args.validate_batches,
                                  mean=stats["mean"], std=stats["std"])
        rec = {"epoch": epoch, "train": tr, "validation": vl, "eq_probe": probe, "eq_sweep": sweep}
        tm.write_jsonl(log, rec); print(json.dumps(rec, indent=2, sort_keys=True))
        torch.save({"model": model.state_dict(), "cond_stats": stats}, args.output_dir / "last.pt")
        score = vl.get("loss", math.inf)
        if score < best:
            best = score
            torch.save({"model": model.state_dict(), "cond_stats": stats}, args.output_dir / "best.pt")
    print("DONE. cond_stats.json + best.pt written.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
