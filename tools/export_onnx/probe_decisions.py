"""Run masteringbrainv2 directly via PyTorch and decode its decisions.

This bypasses ONNX (the deployment artifact) to directly exercise the
checkpoint the way it was validated, then decodes the 44-float decision through
the same slice map the C++ decoder uses (SonicMasterDecisionDecoder). The goal
is to SEE what the model actually predicts on representative audio, not to test
the ONNX plumbing.

Usage:
    python tools/export_onnx/probe_decisions.py \\
        --package "C:/Users/HP/Downloads/sonicmaster-v3-decision-engine-20260530T121536Z"
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
import torch

SEGMENT = 262138
# Slice offsets — must match src/AI/SonicMasterDecisionDecoder.h exactly.
EQ_FREQS = [60, 120, 250, 500, 1000, 2500, 5000, 10000]
TARGET_LUFS_IDX = 8
TRUE_PEAK_IDX = 9
COMP_GATE_IDX = 10
COMP_OFFSET = 11
COMP_BANDS = 3
COMP_WIDTH = 6
EXCITER_GATE_IDX = 29
SAT_OFFSET = 30
STEREO_GATE_IDX = 34
STEREO_OFFSET = 35
AGGR_IDX = 39
GAIN_RED_IDX = 40
CHAR_OFFSET = 41
CHAR_NAMES = ["transparent", "balanced", "aggressive"]


def load_module(package_root: Path):
    sys.path.insert(0, str(package_root / "training" / "neural-mastering" / "bin" / "training"))
    from master_audio import load_module_from_checkpoint
    ckpt = package_root / "models" / "v3" / "mastering-brain-v2-fullchain-best" / "checkpoints" / "best.ckpt"
    module, hp = load_module_from_checkpoint(ckpt, torch.device("cpu"))
    module.eval()
    return module, hp


def _decode_char(logits):
    arr = np.asarray(logits, dtype=np.float32)
    if arr.size == 0:
        return "n/a", float("nan")
    idx = int(np.argmax(arr))
    return CHAR_NAMES[idx] if idx < len(CHAR_NAMES) else f"#{idx}", float(arr[idx])


def synth(name: str, sr: int = 44100):
    """Synthesize a few recognizable source materials at `sr` for `SEGMENT` samples."""
    n = SEGMENT
    t = np.arange(n, dtype=np.float32) / sr
    rng = np.random.RandomState(0)
    if name == "bright_pop":
        # Treble-heavy: 2 kHz + 8 kHz sines, moderate level.
        x = 0.18 * (np.sin(2 * np.pi * 220 * t)
                    + 0.6 * np.sin(2 * np.pi * 2000 * t)
                    + 0.4 * np.sin(2 * np.pi * 8000 * t))
    elif name == "bass_heavy_edm":
        # Sub + kick-like thump at 50 Hz, bright hat noise.
        kick = np.sign(np.sin(2 * np.pi * 50 * t)) * np.exp(-((t * 4) % 1) * 12)
        x = 0.30 * (np.sin(2 * np.pi * 55 * t) + 0.8 * kick) + 0.05 * rng.randn(n)
    elif name == "quiet_acoustic":
        # Low-level 330 Hz guitar-like + 660 Hz harmonic.
        x = 0.02 * (np.sin(2 * np.pi * 330 * t) + 0.3 * np.sin(2 * np.pi * 660 * t))
    elif name == "loud_clipping":
        # Pushed hot so peaks are near +1, aggressive transients.
        x = 0.9 * np.sin(2 * np.pi * 440 * t) + 0.3 * rng.randn(n)
        x = np.tanh(x * 1.5)
    elif name == "mid_sine":
        x = 0.1 * np.sin(2 * np.pi * 1000 * t)
    else:
        x = 0.1 * rng.randn(n)
    stereo = np.stack([x, x + 0.01 * rng.randn(n)], axis=0).astype(np.float32)
    return torch.from_numpy(stereo).unsqueeze(0)


def measure_simple(stereo):
    """Cheap objective measures of the input for context (LUFS-ish, peak)."""
    x = stereo[0].numpy()
    mean_sq = float(np.mean(np.clip(x, -1, 1) ** 2))
    rms_db = 10 * np.log10(mean_sq + 1e-12)
    peak = float(np.max(np.abs(x)))
    peak_dbtp = 20 * np.log10(peak + 1e-12)
    return rms_db, peak_dbtp


def decode(decision, target_lufs):
    d = np.asarray(decision, dtype=np.float32).reshape(-1)
    eq = d[0:8]
    out = {
        "target_lufs": float(d[TARGET_LUFS_IDX]),
        "true_peak_ceiling_dbtp": float(d[TRUE_PEAK_IDX]),
        "compressor_gate": float(d[COMP_GATE_IDX]),
        "compressor": [
            {
                "threshold_db": float(d[COMP_OFFSET + b * COMP_WIDTH + 0]),
                "ratio": float(d[COMP_OFFSET + b * COMP_WIDTH + 1]),
                "attack_ms": float(d[COMP_OFFSET + b * COMP_WIDTH + 2]),
                "release_ms": float(d[COMP_OFFSET + b * COMP_WIDTH + 3]),
                "makeup_db": float(d[COMP_OFFSET + b * COMP_WIDTH + 4]),
                "knee_db": float(d[COMP_OFFSET + b * COMP_WIDTH + 5]),
            }
            for b in range(COMP_BANDS)
        ],
        "exciter_gate": float(d[EXCITER_GATE_IDX]),
        "saturation": [{"drive": float(d[SAT_OFFSET + 2 * i]),
                        "mix": float(d[SAT_OFFSET + 2 * i + 1])} for i in range(2)],
        "stereo_gate": float(d[STEREO_GATE_IDX]),
        "stereo": [{"width": float(d[STEREO_OFFSET + 2 * i]),
                    "side_gain": float(d[STEREO_OFFSET + 2 * i + 1])} for i in range(2)],
        "limiter_aggr": float(d[AGGR_IDX]),
        "expected_gr_db": float(d[GAIN_RED_IDX]),
        "character": _decode_char(d[CHAR_OFFSET:CHAR_OFFSET + 3])[0],
    }
    return out, eq


def fmt_eq(eq):
    parts = []
    for f, g in zip(EQ_FREQS, eq):
        sign = "+" if g >= 0 else ""
        parts.append(f"{f}Hz:{sign}{g:.2f}dB")
    return "  ".join(parts)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--package", type=Path, required=True)
    ap.add_argument("--target-lufs", type=float, default=-14.0)
    args = ap.parse_args()

    module, hp = load_module(args.package)
    print(f"loaded model. architecture={hp.get('modelConfig', {}).get('architectureName', '?')}\n")

    sources = ["bright_pop", "bass_heavy_edm", "quiet_acoustic", "loud_clipping", "mid_sine"]
    expected_width = 44
    for name in sources:
        wav = synth(name)
        rms_db, peak_dbtp = measure_simple(wav)
        with torch.no_grad():
            # The Lightning module's forward returns the waveform, not the
            # decision; the decision is cached on network._last_mastering_decisions.
            # This mirrors run_decision_inference._collect_decision_predictions.
            ret = module(wav, target_lufs_db=torch.tensor([args.target_lufs]))
            if ret is not None and ret.ndim == 2 and int(ret.shape[-1]) == expected_width:
                dec = ret.detach().cpu().numpy().reshape(-1)
            else:
                last = getattr(module.network, "_last_mastering_decisions", None)
                dec = last.detach().cpu().numpy().reshape(-1) if last is not None else None
        if dec is None or dec.size < 44:
            print(f"[{name}] model produced no decision (got shape {None if dec is None else dec.shape})")
            continue

        plan, eq = decode(dec, args.target_lufs)
        char, char_logit = _decode_char(dec[CHAR_OFFSET:CHAR_OFFSET + 3])
        print(f"=== {name}  (input RMS≈{rms_db:+.1f} dBFS, peak≈{peak_dbtp:+.1f} dBTP) ===")
        print(f"  target LUFS         : {plan['target_lufs']:+.2f}  (request was {args.target_lufs:+.1f})")
        print(f"  true-peak ceiling   : {plan['true_peak_ceiling_dbtp']:+.2f} dBTP")
        print(f"  character           : {char}  (logit {char_logit:+.2f})")
        print(f"  limiter aggressiv.  : {plan['limiter_aggr']:.3f}   expected GR {plan['expected_gr_db']:+.2f} dB")
        print(f"  EQ (8 bands, dB)    : {fmt_eq(eq)}")
        print(f"  compressor gate     : {plan['compressor_gate']:.3f}")
        for b, c in enumerate(plan["compressor"]):
            print(f"    band{b}: thr {c['threshold_db']:+.1f}dB  ratio {c['ratio']:.2f}:1  "
                  f"atk {c['attack_ms']:.1f}ms  rel {c['release_ms']:.1f}ms  "
                  f"makeup {c['makeup_db']:+.2f}dB  knee {c['knee_db']:.1f}dB")
        print(f"  exciter gate {plan['exciter_gate']:.3f}  sat={plan['saturation']}")
        print(f"  stereo gate {plan['stereo_gate']:.3f}  stereo={plan['stereo']}")
        print()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
