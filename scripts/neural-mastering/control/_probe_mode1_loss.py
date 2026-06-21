"""Ad-hoc probe: in normalizer mode 1, what loss do clean (non-EQ-abuse) plans get?
Run: python _probe_mode1_loss.py
"""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import json
import numpy as np
import soundfile as sf
from morephi_render import HeadlessRenderer
from objective import loss
from target import build_target

LIB = r'G:/More_Phi-vst3-plugin/build/tools/headless_mastering_render/Release/libmore_phi_headless_render.dll'
MAN = r'G:/More_Phi-vst3-plugin/scripts/neural-mastering/control/data/manifest_ssl_t1/train.jsonl'


def t(host, inter, seg, target, plan, label):
    o, m = host.render_candidate(inter, plan)
    L, det = loss(plan, seg, target, host)
    print(f"{label:28s}: lufs={m.lufs_integrated:+.2f} tp={m.true_peak_dbtp:+.2f} "
          f"loss={L:.3f} loud={det['loud']:.2f} spec={det['spec']:.2f} tp={det['tp']:.2f}")


def main():
    host = HeadlessRenderer(LIB, 48000, 512, 1)  # MODE 1
    target = build_target('neutral')
    row = json.loads([l for l in open(MAN, encoding='utf-8') if l.strip()][0])
    d, sr = sf.read(row['sourcePath'], always_2d=True)
    seg = d.T.astype('float32')[:, row['startSample']:row['startSample'] + 48000 * 6]
    inter = np.empty(seg.shape[1] * 2, dtype='float32')
    inter[0::2] = seg[0]; inter[1::2] = seg[1]

    t(host, inter, seg, target, np.zeros(72, dtype='float32'), 'ZERO')
    for lv in [0.0, 0.5, 0.99]:
        p = np.zeros(72, dtype='float32'); p[64] = lv
        t(host, inter, seg, target, p, f'loud={lv} only')
    p = np.zeros(72, dtype='float32'); p[64] = 0.0; p[0:8] = 0.1
    t(host, inter, seg, target, p, 'loud=0 + lowshelf+0.1')
    host.close()


if __name__ == "__main__":
    main()
