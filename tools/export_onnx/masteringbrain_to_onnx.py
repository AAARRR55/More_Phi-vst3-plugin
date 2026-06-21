"""Export masteringbrainv2 checkpoint to ONNX for the VST3 realtime path.

Run from the More-Phi repo root, pointing at the unpacked
sonicmaster-v3-decision-engine package:

    python tools/export_onnx/masteringbrain_to_onnx.py \\
        --package "C:/Users/HP/Downloads/sonicmaster-v3-decision-engine-20260530T121536Z" \\
        --output-dir build/sonicmaster

Produces:
  masteringbrain_v2_decision.onnx   - the traced graph (opset 17)
  masteringbrain_v2_contract.json   - I/O names/shapes the runner validates at load
  parity_report.json                - max abs diff vs the PyTorch forward

The model's forward (MasteringDecisionNet.forward in model.py:2651) returns the
decision tensor directly for a given waveform + target_lufs_db, so the export
wrapper simply delegates to module.network(waveform, target_lufs_db=...).
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
import torch

SEGMENT_FRAMES = 262138
DECISION_WIDTH = 44


def _resolve_paths(package_root: Path) -> tuple[Path, Path]:
    ckpt = package_root / "models" / "v3" / "mastering-brain-v2-fullchain-best" / "checkpoints" / "best.ckpt"
    if not ckpt.exists():
        sys.exit(f"checkpoint not found: {ckpt}")
    return ckpt, package_root


def _load_module(package_root: Path):
    """Load the Lightning module + hparams from the checkpoint, eval mode, CPU."""
    sys.path.insert(0, str(package_root / "training" / "neural-mastering" / "bin" / "training"))
    from master_audio import load_module_from_checkpoint  # type: ignore
    ckpt, _ = _resolve_paths(package_root)
    module, hparams = load_module_from_checkpoint(ckpt, torch.device("cpu"))
    module.eval()
    return module, hparams


class _DecisionOnly(torch.nn.Module):
    """Wrap the network so forward(waveform, target_lufs) -> decision tensor.

    MasteringDecisionNet.forward returns the decision tensor directly, so this
    wrapper just delegates. (If a future checkpoint uses HybridMasteringNet,
    the fallback to `_last_mastering_decisions` below keeps this working.)
    """

    def __init__(self, module):
        super().__init__()
        self.net = module.network

    def forward(self, waveform: torch.Tensor, target_lufs: torch.Tensor) -> torch.Tensor:
        out = self.net(waveform, target_lufs_db=target_lufs)
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
    ap.add_argument("--opset", type=int, default=17)
    args = ap.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    module, _ = _load_module(args.package)
    wrapper = _DecisionOnly(module).eval()

    # Synthetic 6 s stereo seed for tracing + parity (sine + harmonics + noise).
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
        torch.onnx.export(
            wrapper,
            (waveform, target_lufs),
            str(onnx_path),
            input_names=["waveform", "target_lufs"],
            output_names=["decision"],
            dynamic_axes={"waveform": {0: "batch"}, "decision": {0: "batch"}},
            opset_version=args.opset,
            do_constant_folding=True,
        )
        pt_out = wrapper(waveform, target_lufs).detach().cpu().numpy().reshape(-1)

    # ── Parity: PyTorch vs ONNX on the same seed ─────────────────────────────
    onnx_out_prefix = None
    try:
        import onnxruntime as ort
        sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
        onnx_out = sess.run(None, {
            "waveform": waveform.numpy(),
            "target_lufs": target_lufs.numpy(),
        })[0].reshape(-1)
        max_abs = float(np.max(np.abs(pt_out - onnx_out)))
        onnx_out_prefix = onnx_out[:8].tolist()
    except Exception as exc:  # onnxruntime optional for the export step itself
        max_abs = float("nan")
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
    (args.output_dir / "masteringbrain_v2_contract.json").write_text(json.dumps(contract, indent=2))

    print(f"wrote {onnx_path}")
    print(f"wrote {args.output_dir / 'masteringbrain_v2_contract.json'}")
    verdict = "PASS" if (np.isfinite(max_abs) and max_abs < 1e-4) else "FAIL"
    print(f"parity max_abs_diff={max_abs:.2e} -> {verdict}")
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
