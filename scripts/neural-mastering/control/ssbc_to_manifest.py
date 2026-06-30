#!/usr/bin/env python3
"""Build (feature, delta) JSONL manifests from the SolidStateBusComp corpus for
blended control-regressor training. (License: user-dismissed CC-BY-NC.)

Each SSBC pair's 72-delta target is a COHERENT full-mastering label:
  - the DYNAMICS slice (8) comes from the real SSBC compressor combo (threshold/
    ratio), grounded in the proxy DSP (diff_dsp._apply_compressor):
      dyn[0]=amount(=1, full), dyn[1]=threshold, dyn[2]=ratio, dyn[3..7]=0
  - the OTHER 64 deltas (eq/stereo/harmonic/limiter/loudness) come from the
    synthetic mastering teacher (train.SyntheticTeacher) evaluated on that
    input's real features — corrective EQ + loudness makeup + width, exactly as
    a balanced master applies AFTER compression. This avoids the compression-
    only trap (SSBC targets have no EQ/makeup, so naively zeroing the other
    slices taught the model to skip EQ/loudness on real audio).

Features (63) are extracted from the INPUT audio once per unique song (each is
shared across ~220 combos) via features.py (strict C++ runtime parity).

Output JSONL rows: {"feature":[63], "delta":[72], split, sourceId, combo, ...}.
Consumed by train.py --data-mode manifest (ConcatDataset blends with synthetic).
"""
from __future__ import annotations

import argparse
import json
import math
import sys
import threading
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from codec import OUTPUT_DELTA_COUNT, DYNAMICS_SLICE, serialize_feature_frame  # noqa: E402
from features import extract_feature_frame  # noqa: E402

DY0 = DYNAMICS_SLICE[0]  # 32
DY_LEN = DYNAMICS_SLICE[1] - DYNAMICS_SLICE[0]  # 8


def combo_dynamics(threshold_db: float, ratio: float) -> list[float]:
    """The 8-element dynamics slice for an SSBC combo (proxy-aligned).

    dyn[0]=amount -> sigmoid(2*d0); SSBC = full compression -> d0 = 1
    dyn[1]=threshold -> -24 + 12*tanh(d1); d1 = clamp(arctanh((th+24)/12),-1,1)
    dyn[2]=ratio -> 1 + 5*sigmoid(2*d2); d2 = clamp(0.5*logit((ratio-1)/5),-1,1)
    dyn[3..7] unused (no proxy slot; attack/release not modeled by the proxy).
    The proxy's threshold/ratio ranges are narrower than SSBC's -> extreme combos clamp.
    """
    d = [0.0] * DY_LEN
    d[0] = 1.0  # amount: full compression (proxy sigmoid(2) ~= 0.88)
    t = max(-0.999, min(0.999, (threshold_db + 24.0) / 12.0))
    d[1] = max(-1.0, min(1.0, math.atanh(t)))
    r = max(1e-3, min(1.0 - 1e-3, (ratio - 1.0) / 5.0))
    d[2] = max(-1.0, min(1.0, 0.5 * math.log(r / (1.0 - r))))
    return d


def read_audio(path: Path) -> tuple[np.ndarray, int]:
    import soundfile as sf
    data, sr = sf.read(str(path), always_2d=True, dtype="float32")
    return data.T.astype(np.float32), int(sr)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--manifest", type=Path, required=True)
    ap.add_argument("--audio-root", type=Path, required=True)
    ap.add_argument("--out-train", type=Path, required=True)
    ap.add_argument("--out-val", type=Path, required=True)
    ap.add_argument("--max-workers", type=int, default=16)
    ap.add_argument("--limit", type=int, help="cap items (debug)")
    ap.add_argument("--selfcheck", action="store_true")
    args = ap.parse_args()

    from train import SyntheticTeacher  # lazy: pulls torch; only needed for the vocab blend
    teacher = SyntheticTeacher(seed=1337)  # teacher_deltas is rng-free; seed irrelevant

    payload = json.loads(args.manifest.read_text(encoding="utf-8"))
    items = payload.get("items", payload if isinstance(payload, list) else [])
    if args.limit:
        items = items[: args.limit]
    if not items:
        raise SystemExit("no items in manifest")

    uniq: list[str] = []
    seen: set[str] = set()
    for it in items:
        rel = it.get("inputPath")
        if rel and rel not in seen:
            seen.add(rel)
            uniq.append(rel)
    print(f"ssbc: {len(items)} items, {len(uniq)} unique inputs -> {args.audio_root}")

    frames: dict[str, object] = {}
    errs: list[str] = []
    lock = threading.Lock()
    done = [0]

    def extract(rel: str):
        path = args.audio_root / rel
        if not path.exists():
            return rel, None, f"missing: {rel}"
        try:
            audio, sr = read_audio(path)
        except Exception as exc:  # noqa: BLE001
            return rel, None, f"read fail {rel}: {exc!r}"
        frame = extract_feature_frame(audio, float(sr),
                                      channel_count=int(audio.shape[0]), block_size=512, frame_index=0)
        return rel, frame, None

    with ThreadPoolExecutor(max_workers=args.max_workers) as ex:
        futs = {ex.submit(extract, rel): rel for rel in uniq}
        for fut in as_completed(futs):
            rel, frame, err = fut.result()
            with lock:
                done[0] += 1
                if err:
                    errs.append(err)
                else:
                    frames[rel] = frame
                if done[0] % 10 == 0 or done[0] == len(uniq):
                    print(f"  features {done[0]}/{len(uniq)} (ok={len(frames)})")
    if len(frames) < max(1, len(uniq) // 2):
        raise SystemExit(f"too many input extractions failed: {len(errs)}/{len(uniq)}; first: {errs[:3]}")

    by_split: dict[str, list[dict]] = {"train": [], "val": [], "test": []}
    miss = 0
    for it in items:
        frame = frames.get(it.get("inputPath"))
        if frame is None:
            miss += 1
            continue
        feature = serialize_feature_frame(frame)
        base = list(teacher.teacher_deltas(frame))            # full synthetic vocab (64+8)
        base[DY0:DY0 + DY_LEN] = combo_dynamics(float(it["threshold"]), float(it["ratio"]))  # real compression
        delta = base
        row = {
            "id": it.get("id"),
            "split": it.get("split", "train"),
            "sourceId": it.get("sourceId"),
            "feature": feature,
            "delta": delta,
            "inputPath": it.get("inputPath"),
            "targetPath": it.get("targetPath"),
            "combo": it.get("combo"),
            "threshold": float(it["threshold"]),
            "attack": float(it.get("attack", 0.0)),
            "release": float(it.get("release", 0.0)),
            "ratio": float(it["ratio"]),
            "dataset": "ssbc",
        }
        by_split.setdefault(row["split"], []).append(row)

    args.out_train.parent.mkdir(parents=True, exist_ok=True)
    with args.out_train.open("w", encoding="utf-8") as fh:
        for r in by_split["train"]:
            fh.write(json.dumps(r) + "\n")
    val_rows = by_split["val"] + by_split["test"]
    with args.out_val.open("w", encoding="utf-8") as fh:
        for r in val_rows:
            fh.write(json.dumps(r) + "\n")
    print(f"wrote {args.out_train}: {len(by_split['train'])} train | "
          f"{args.out_val}: {len(val_rows)} val(+test) | missing-feature items: {miss}")
    if errs:
        print(f"WARN: {len(errs)} input extractions failed; first: {errs[:3]}")

    # ONE self-check on the dynamics derivation (the novel logic; the 64 non-dynamics
    # deltas are just SyntheticTeacher.teacher_deltas, already tested in train.py).
    if args.selfcheck:
        import torch
        from diff_dsp import DifferentiableMasteringChain

        failures: list[str] = []
        print("=== dynamics-derivation grid: [amount, threshold, ratio] ===")
        for th in (-24.0, -12.0, -6.0, 0.0):
            for ra in (2.0, 4.0, 10.0):
                cd = combo_dynamics(th, ra)
                in_range = all(-1.0 <= v <= 1.0 for v in cd)
                print(f"  th={th:<5} ra={ra:<4} -> amount={cd[0]:.3f} thr_d={cd[1]:+.3f} ratio_d={cd[2]:+.3f} "
                      f"unused[3:]=0:{all(v == 0.0 for v in cd[3:])} in_range:{in_range}")
                if not (in_range and all(v == 0.0 for v in cd[3:])):
                    failures.append(f"grid th={th} ra={ra}")
        d2_by_ratio = [combo_dynamics(-12.0, ra)[2] for ra in (2.0, 4.0, 10.0)]
        d1_by_th = [combo_dynamics(th, 4.0)[1] for th in (-24.0, -12.0, -6.0, 0.0)]
        if d2_by_ratio != sorted(d2_by_ratio):
            failures.append(f"ratio_d not monotonic: {d2_by_ratio}")
        if d1_by_th != sorted(d1_by_th):
            failures.append(f"threshold_d not monotonic: {d1_by_th}")

        chain = DifferentiableMasteringChain(sample_rate=48000.0).eval()
        rng = np.random.default_rng(0)
        audio = torch.tensor(0.3 * rng.standard_normal((1, 2, 48000 * 2), dtype=np.float32))
        rin = 20.0 * math.log10(max(float(np.sqrt((audio[0].numpy() ** 2).mean())), 1e-9))

        def rms_db(t: torch.Tensor) -> float:
            return 20.0 * math.log10(max(float(np.sqrt((t[0].numpy() ** 2).mean())), 1e-9))

        print("=== isolated _apply_compressor reduction by ratio (threshold=-12) ===")
        mags = []
        for ra in (2.0, 4.0, 10.0):
            dyn = torch.zeros((1, OUTPUT_DELTA_COUNT), dtype=torch.float32)
            dyn[0, DY0:DY0 + DY_LEN] = torch.tensor(combo_dynamics(-12.0, ra))
            with torch.no_grad():
                out = chain._apply_compressor(audio, dyn)
            red = rms_db(out) - rin
            mags.append(-red)
            print(f"  ratio={ra:<4}: reduction={red:+.2f}dB")
        if not (all(m > 0.1 for m in mags) and mags == sorted(mags)):
            failures.append(f"isolated compressor not reducing/monotonic: {mags}")

        if failures:
            print("SELF-CHECK FAIL: " + "; ".join(failures))
            return 2
        print("SELF-CHECK PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
