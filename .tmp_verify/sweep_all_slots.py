"""Diagnostic 2: which slot ACTUALLY drives output LUFS?

Sweeps EACH active slot group (eq, dynamics, stereo, harmonic, limiter, loudness)
one at a time on a real quiet segment, to find what makes output LUFS rise.
The loudness[0] sweep showed it's inert in normalizer_mode=0, so the -7.6 LUFS
overshoot must come from dynamics or limiter (compression makeup / limiting).
"""
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from morephi_render import HeadlessRenderer  # noqa: E402

SLICE = {  # from codec.py
    "eq[0..31]": (0, 32),
    "dynamics[0..3]": (32, 36),
    "stereo[0..3]": (40, 44),
    "harmonic[0]": (48, 49),
    "limiter[0]": (56, 57),
    "loudness[0]": (64, 65),
}


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
    out = np.empty(a[0].shape[0] * 2, dtype=np.float32)
    out[0::2] = a[0]; out[1::2] = a[1]
    return out


def main():
    lib = sys.argv[1]
    manifest = sys.argv[2]
    sr = 48000
    seg_samples = int(10.0 * sr)
    host = HeadlessRenderer(lib, sample_rate=sr, block_size=512, normalizer_mode=0)
    row = json.loads(open(manifest).readline())
    seg = load_seg(row["sourcePath"], int(row.get("startSample", 0)), seg_samples, sr)
    pcm = to_inter(seg)
    _, m0 = host.render_candidate(pcm, np.zeros(72, dtype=np.float32))
    print(f"BASELINE (all zero): LUFS={m0.lufs_integrated:.2f} TP={m0.true_peak_dbtp:.2f}\n")

    for name, (lo, hi) in SLICE.items():
        print(f"{name} sweep (set all slots in range to value, rest zero):")
        for v in [-1.0, 0.0, 0.5, 1.0]:
            d = np.zeros(72, dtype=np.float32)
            # harmonic[0] is one-sided [0,1]; clamp negative to 0 for that slot
            if name == "harmonic[0]" and v < 0:
                continue
            d[lo:hi] = v
            _, m = host.render_candidate(pcm, d)
            print(f"  v={v:>5.2f}  -> LUFS={m.lufs_integrated:>7.2f}  TP={m.true_peak_dbtp:>7.2f}  GR={m.limiter_gain_reduction_db:>6.2f}")
        print()
    host.close()


if __name__ == "__main__":
    main()
