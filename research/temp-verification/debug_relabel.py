"""Debug: run the relabel worker logic on ONE segment with full traceback."""
import json
import sys
import traceback
from pathlib import Path

import numpy as np
import soundfile as sf

sys.path.insert(0, str(Path(__file__).resolve().parent))


def main():
    manifest = sys.argv[1]
    lib = sys.argv[2]
    from morephi_render import HeadlessRenderer
    from features import extract_feature_frame
    from labels_t2 import recover_deltas_render
    from codec import control_deltas_to_vector

    host = HeadlessRenderer(lib, sample_rate=48000, block_size=512, normalizer_mode=0)
    row = json.loads(open(manifest).readline())
    print("sourcePath:", row.get("sourcePath"), "| exists:", Path(row.get("sourcePath", "")).exists())
    print("startSample:", row.get("startSample"))

    sp = row["sourcePath"]
    start = int(row.get("startSample", 0))
    try:
        data, _ = sf.read(sp, always_2d=True)
        audio = data.T.astype(np.float32)
        seg = audio[:, start:start + 48000 * 10]
        if seg.shape[0] == 1:
            seg = np.stack([seg[0], seg[0]])
        print(f"seg loaded: shape={seg.shape}")
        frame = extract_feature_frame(seg, 48000, channel_count=2, block_size=512, frame_index=start)
        print(f"frame extracted: lufs={frame.integrated_lufs:.2f}")
        import random
        rng = random.Random(1337)
        deltas = recover_deltas_render(frame, seg, rng, genre="neutral", render_host=host, max_fevals=50)
        dv = control_deltas_to_vector(deltas)
        print(f"SUCCESS: delta max={max(abs(v) for v in dv):.3f}")
    except Exception as e:
        traceback.print_exc()


if __name__ == "__main__":
    main()
