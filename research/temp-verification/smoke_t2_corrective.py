"""Smoke-test the T2 teacher with the v2 corrective target on ONE real segment.
Confirms: corrective target builds, CMA-ES runs, label is valid + LUFS-aware.
"""
import json
import random
import sys
from pathlib import Path

import numpy as np
import soundfile as sf

sys.path.insert(0, str(Path(__file__).resolve().parent))
from features import extract_feature_frame
from labels import synthesize_deltas
from labels_t2 import recover_deltas_render
from codec import control_deltas_to_vector
from target import build_target_from_features


def load_seg(path, start, seg_samples, sr):
    data, _ = sf.read(path, always_2d=True)
    audio = data.T.astype(np.float32)
    seg = audio[:, start:start + seg_samples]
    if seg.shape[0] == 1:
        seg = np.stack([seg[0], seg[0]])
    return seg.astype(np.float32)


def main():
    lib = sys.argv[1]
    manifest = sys.argv[2]
    from morephi_render import HeadlessRenderer
    host = HeadlessRenderer(lib, sample_rate=48000, block_size=512, normalizer_mode=0)

    row = json.loads(open(manifest).readline())
    sp = row["sourcePath"]
    if not Path(sp).exists():
        print("source missing:", sp); return 1
    seg = load_seg(sp, int(row.get("startSample", 0)), int(10 * 48000), 48000)
    frame = extract_feature_frame(seg, 48000, channel_count=2, block_size=512, frame_index=0)

    print(f"INPUT: lufs={frame.integrated_lufs:.2f} tilt={frame.spectral_tilt:.2f} "
          f"spec median={np.median(frame.spectral_bands):.2f}")
    # Show the corrective target adapts to THIS input (vs the fixed pink line)
    tc = build_target_from_features(list(frame.spectral_bands), "neutral", frame.integrated_lufs)
    print(f"v2 corrective target: lufs={tc.lufs} spec[0]={tc.spec_db[0]:.2f} spec[31]={tc.spec_db[31]:.2f} "
          f"(median-toward, not a fixed line)")

    rng = random.Random(7)
    t1 = synthesize_deltas(frame, rng)
    t1v = np.asarray(control_deltas_to_vector(t1), np.float32)
    print(f"T1 label: max|delta|={np.max(np.abs(t1v)):.3f} eq_mean={np.mean(t1v[:32]):.3f}")

    t2 = recover_deltas_render(frame, seg, rng, genre="neutral", render_host=host, max_fevals=150)
    t2v = np.asarray(control_deltas_to_vector(t2), np.float32)
    print(f"T2 label (corrective target): max|delta|={np.max(np.abs(t2v)):.3f} "
          f"eq_mean={np.mean(t2v[:32]):.3f} loudness[0]={t2v[64]:.3f}")

    # Render the T2 plan and check it lands closer to -14 than T1
    pcm = np.empty(seg.shape[1] * 2, dtype=np.float32)
    pcm[0::2] = seg[0]; pcm[1::2] = seg[1]
    _, m_t2 = host.render_candidate(pcm, t2v)
    _, m_t1 = host.render_candidate(pcm, t1v)
    print(f"RENDER: T1 plan -> LUFS={m_t1.lufs_integrated:.2f} | T2 plan -> LUFS={m_t2.lufs_integrated:.2f} (target -14.0)")
    host.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
