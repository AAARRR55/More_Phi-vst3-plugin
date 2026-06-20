#!/usr/bin/env python3
"""T2 teacher smoke test: on a synthetic 'unmastered' segment, does CMA-ES recover
deltas that beat T1 on the multi-objective loss (the Phase-1 gate: >=30%)?

Usage: python tests/test_labels_t2.py /path/to/libmore_phi_headless_render.so
"""
import random
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from morephi_render import HeadlessRenderer  # noqa: E402
from features import extract_feature_frame  # noqa: E402
from labels import synthesize_deltas  # noqa: E402
from codec import control_deltas_to_vector  # noqa: E402
from objective import loss  # noqa: E402
from target import build_target  # noqa: E402
from labels_t2 import recover_deltas_render  # noqa: E402


def main() -> int:
    lib = sys.argv[1]
    sr = 48000.0
    host = HeadlessRenderer(lib, sample_rate=sr, block_size=512, normalizer_mode=0)
    n = int(sr * 4)
    t = np.arange(n) / sr
    # deliberately quiet + dull "unmastered" mix (harmonic stack + noise)
    mid = 0.12 * (np.sin(2 * np.pi * 220 * t) + 0.5 * np.sin(2 * np.pi * 440 * t)
                  + 0.3 * np.random.randn(n)).astype(np.float32)
    side = 0.04 * np.random.randn(n).astype(np.float32)
    seg = np.stack([mid + side, mid - side]).astype(np.float32)  # [2, n]

    frame = extract_feature_frame(seg, sr, channel_count=2, block_size=512, frame_index=0)
    print(f"input: lufs={frame.integrated_lufs:.1f} tilt={frame.spectral_tilt:.2f} tp={frame.true_peak_dbtp:.2f}")

    rng = random.Random(7)
    target = build_target("neutral")
    t1 = synthesize_deltas(frame, rng)
    t1v = np.asarray(control_deltas_to_vector(t1), np.float32)
    L1, _ = loss(t1v, seg, target, host, x_t1=t1v)

    t2 = recover_deltas_render(frame, seg, rng, render_host=host, max_fevals=200)
    t2v = np.asarray(control_deltas_to_vector(t2), np.float32)
    L2, terms = loss(t2v, seg, target, host, x_t1=t1v)

    print(f"T1: loss={L1:.4f} |maxΔ|={np.max(np.abs(t1v)):.3f}")
    print(f"T2: loss={L2:.4f} |maxΔ|={np.max(np.abs(t2v)):.3f}  "
          f"improvement={100 * (1 - L2 / max(L1, 1e-9)):.1f}%  (gate: >=30%)")
    print(f"T2 terms: { {k: round(v, 3) for k, v in terms.items()} }")
    host.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
