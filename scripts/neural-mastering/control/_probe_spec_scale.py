"""Probe: compare input spectral_bands vs target spec_db vs rendered spectral_bands
to find why the ZERO plan has spec loss = 36 (should be ~0 if target = input+small)."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import json
import numpy as np
import soundfile as sf
from features import extract_feature_frame
from target import build_target_from_features
from morephi_render import HeadlessRenderer

LIB = r'G:/More_Phi-vst3-plugin/build/tools/headless_mastering_render/Release/libmore_phi_headless_render.dll'
MAN = r'G:/More_Phi-vst3-plugin/scripts/neural-mastering/control/data/manifest_ssl_t1/train.jsonl'

host = HeadlessRenderer(LIB, 48000, 512, 1)
row = json.loads([l for l in open(MAN, encoding='utf-8') if l.strip()][0])
d, sr = sf.read(row['sourcePath'], always_2d=True)
seg = d.T.astype('float32')[:, row['startSample']:row['startSample'] + 48000 * 6]
if seg.shape[0] == 1:
    seg = np.stack([seg[0], seg[0]])
frame = extract_feature_frame(seg, 48000, channel_count=2, block_size=512, frame_index=row['startSample'])
inp_spec = list(frame.spectral_bands)
target = build_target_from_features(inp_spec, genre='neutral', input_lufs=frame.integrated_lufs)
print(f"input  spec_bands[:6]: {[round(x,1) for x in inp_spec[:6]]}")
print(f"target spec_db[:6]  : {[round(x,1) for x in target.spec_db[:6]]}")
print(f"input  range: min={min(inp_spec):.1f} max={max(inp_spec):.1f}")
print(f"target range: min={min(target.spec_db):.1f} max={max(target.spec_db):.1f}")
# render ZERO and re-measure
inter = np.empty(seg.shape[1] * 2, dtype='float32')
inter[0::2] = seg[0]; inter[1::2] = seg[1]
o, m = host.render_candidate(inter, np.zeros(72, dtype='float32'))
rframe = extract_feature_frame(np.ascontiguousarray(o.T), 48000, channel_count=2, block_size=512, frame_index=0)
r_spec = list(rframe.spectral_bands)
print(f"rendered(ZERO) spec[:6]: {[round(x,1) for x in r_spec[:6]]}")
print(f"rendered range: min={min(r_spec):.1f} max={max(r_spec):.1f}")
# the spec loss term: mean((render - target)/12)^2
sb, ts = r_spec, target.spec_db
k = min(len(sb), len(ts))
spec_loss = float(np.mean([((sb[b] - ts[b]) / 12.0) ** 2 for b in range(k)]))
print(f"spec_loss(ZERO) = {spec_loss:.3f}")
# per-band diff
diffs = [sb[b] - ts[b] for b in range(k)]
print(f"per-band render-target diff[:6]: {[round(x,1) for x in diffs[:6]]}")
host.close()
