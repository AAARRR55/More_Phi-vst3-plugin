#!/usr/bin/env python3
"""Disambiguate OOD-probing vs real training bias: run the model on REAL FMA
held-out features (val.jsonl, which stores the actual T1 labels) and measure
fidelity. High corr here = the synthetic-probe divergence was out-of-distribution;
low corr = a genuine model defect.

Usage: python probe_on_val.py model.onnx data/manifest_fma/val.jsonl
"""
import json
import sys

import numpy as np
import onnxruntime as ort

model, val = sys.argv[1], sys.argv[2]
sess = ort.InferenceSession(model)

feats, t1 = [], []
for line in open(val, encoding="utf-8"):
    line = line.strip()
    if not line:
        continue
    r = json.loads(line)
    feats.append(r["feature"])
    t1.append(r["delta"])
feats = np.asarray(feats, dtype=np.float32)
t1 = np.asarray(t1, dtype=np.float32)

# model is static batch=1 (the VST proposes one plan at a time) -> run row by row
outs = np.stack([sess.run(None, {"input": feats[i:i + 1]})[0][0] for i in range(len(feats))])
mse = float(np.mean((outs - t1) ** 2))
corrs = [
    float(np.corrcoef(outs[i], t1[i])[0, 1])
    for i in range(len(t1))
    if outs[i].std() > 1e-6 and t1[i].std() > 1e-6
]
model_max = np.max(np.abs(outs), axis=1)
t1_max = np.max(np.abs(t1), axis=1)

print(f"=== model vs T1 on {len(t1)} REAL FMA val segments ===")
print(f"MSE(model,T1)  = {mse:.5f}   (RMSE/delta = {np.sqrt(mse):.4f})")
print(f"corr(model,T1) = mean {np.mean(corrs):.3f}  median {np.median(corrs):.3f}  min {np.min(corrs):.3f}")
print(f"|maxΔ| model mean={np.mean(model_max):.3f}  |  T1 mean={np.mean(t1_max):.3f}")
print(f"|maxΔ| match within 0.1: {np.mean(np.abs(model_max - t1_max) < 0.1) * 100:.0f}% of segments")
print(f"restraint |maxΔ|<0.05: model {np.mean(model_max < 0.05) * 100:.1f}%  |  T1 {np.mean(t1_max < 0.05) * 100:.1f}%")
print(f"T1 label |maxΔ| spread: p10={np.percentile(t1_max, 10):.3f} p50={np.percentile(t1_max, 50):.3f} p90={np.percentile(t1_max, 90):.3f}")
# worst-fitting segments
worst = np.argsort((outs - t1).var(axis=1))[-3:][::-1]
print("worst-fit segments (row: model|T1 |maxΔ|):")
for i in worst:
    print(f"  row {i}: model |maxΔ|={model_max[i]:.3f}  T1 |maxΔ|={t1_max[i]:.3f}")
