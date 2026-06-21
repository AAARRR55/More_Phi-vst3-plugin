"""Diagnostic: sweep loudness[0] slot to learn the delta -> output LUFS mapping.

Renders a real quiet FMA segment with ONLY the loudness slot varying, all else
zero. Prints input LUFS (zero-delta) and output LUFS for each loudness[0] value.
This tells us the ACTUAL sign/magnitude relationship before we fix anything.
"""
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from morephi_render import HeadlessRenderer  # noqa: E402


def load_seg(path, start, seg_samples, sr):
    import soundfile as sf
    data, _ = sf.read(path, always_2d=True)
    audio = data.T.astype(np.float32)
    seg = audio[:, start:start + seg_samples]
    if seg.shape[0] == 1:
        seg = np.stack([seg[0], seg[0]])
    return seg.astype(np.float32)


def to_inter(seg):
    a = np.asarray(seg, dtype=np.float32)
    L, R = a[0], a[1]
    out = np.empty(L.shape[0] * 2, dtype=np.float32)
    out[0::2] = L
    out[1::2] = R
    return out


def main():
    lib = sys.argv[1]
    manifest = sys.argv[2]
    sr = 48000
    seg_samples = int(10.0 * sr)
    host = HeadlessRenderer(lib, sample_rate=sr, block_size=512, normalizer_mode=0)

    row = json.loads(open(manifest).readline())
    sp = row["sourcePath"]
    if not Path(sp).exists():
        print("source missing:", sp); return
    seg = load_seg(sp, int(row.get("startSample", 0)), seg_samples, sr)
    pcm = to_inter(seg)

    # Zero-delta baseline (do nothing)
    _, m_zero = host.render_candidate(pcm, np.zeros(72, dtype=np.float32))
    print(f"INPUT (do-nothing): output LUFS = {m_zero.lufs_integrated:.2f}  TP = {m_zero.true_peak_dbtp:.2f}")
    print()
    print("loudness[0] slot sweep (all other slots = 0):")
    print(f"  {'delta[64]':>10}  {'-> target LUFS (labels.py)':>26}  {'actual output LUFS':>20}")
    for v in [-1.0, -0.5, 0.0, 0.3, 0.5, 0.74, 0.9, 1.0]:
        d = np.zeros(72, dtype=np.float32)
        d[64] = v  # loudness[0]
        # normalizer_mode=0 means gain=1.0; loudness delta is setpoint only. So the
        # setpoint is what the chain tries to hit. Let's see if it actually hits it.
        target_pred = -14.0 + v * 6.0
        try:
            _, m = host.render_candidate(pcm, d)
            print(f"  {v:>10.2f}  {target_pred:>26.2f}  {m.lufs_integrated:>20.2f}")
        except RuntimeError as e:
            print(f"  {v:>10.2f}  {target_pred:>26.2f}  RENDER FAIL {e}")
    print()
    print("NOTE: normalizer_mode=0 = gain=1.0 (loudness is setpoint only).")
    print("If output LUFS still changes with the slot, the normalizer IS responding")
    print("to setpoint even in mode 0. If it DOESN'T change, the loudness overshoot")
    print("comes from a DIFFERENT slot (dynamics/limiter), not loudness[0].")
    host.close()


if __name__ == "__main__":
    main()
