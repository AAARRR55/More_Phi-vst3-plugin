# More-Phi Mastering Control Regressor (Model A)

Trains the **control regressor** that fits the C++ `OnnxNeuralMasteringRunner`
seam (`src/AI/OnnxNeuralMasteringRunner.{h,cpp}`). This is the *control* variant
of the neural mastering model — a small (≈145k param) feature-vector regressor
that predicts 72 per-control mastering deltas at 1–5 Hz on the message thread.
It is **not** the waveform-to-waveform `HybridMasteringNet` in the sibling
`train_neural_mastering.py`; see "Which model is this?" below.

## Contract (must not drift)

| Side | Shape | Notes |
|------|-------|-------|
| Input  | `[1, 63]` | 11 scalars + 32 spectral + 8 stereoCorr + 8 midSide + 4 meta |
| Output | `[1, 72]` | 32 eq + 8 dynamics + 8 stereo + 8 harmonic + 8 limiter + 8 loudness, **tanh → [-1,1]** |

The exact layout lives in `codec.py` and is asserted against the C++ constants
in `tests/test_contract.py`. If you change an offset, the runtime seam breaks
silently. Keep the parity test green.

## Files

- `codec.py` — single source of truth for the feature/label tensor layout (mirrors `src/Core/NeuralMasteringTypes.h` + `serializeFeatureFrame`/`buildPlanCandidate`).
- `model.py` — `MasteringControlRegressor` (conv front-end over spectral bands + MLP head, tanh-bounded). Input normalization is baked **inside** the model so the exported ONNX is self-contained and the C++ seam feeds raw values unchanged.
- `train.py` — training loop with two data modes.
- `tests/test_contract.py` — ONNX contract + train/serve parity verification.

## Requirements

```
torch>=2.0          # training + export
onnx>=1.14          # export (pulls onnxscript via torch.onnx in torch>=2.9)
onnxruntime>=1.16   # contract verification
numpy
```

For a GPU server add the CUDA build of torch matching the installed driver.

## Quick start — synthetic smoke (no data, ~seconds on CPU)

Validates the entire train→export→verify loop before spending real data:

```bash
pip install torch onnx onnxruntime numpy
python train.py --data-mode synthetic --epochs 20 --export-onnx model.onnx
python tests/test_contract.py model.onnx
```

Expected: `PASS`, 63→72, delta range within [-1,1], deterministic, codec parity OK.

The synthetic teacher is a hand-coded smooth map — it exists to exercise the
pipeline, **not** to be a real mastering target. Its loss has an irreducible
floor because the teacher is noisy by construction.

## Real training — manifest mode

Build a JSONL manifest of `{"feature": [63 floats], "delta": [72 floats]}` lines,
one per training example. `feature` is the output of `serialize_feature_frame()`
applied to a `FeatureFrame` extracted from real audio via the repo's
`extract_features.py`; `delta` is the (synthesized) control vector from the
mastering-DSP target pipeline (see `specs/003-neural-mastering-roadmap/`).

```bash
python train.py \
  --data-mode manifest \
  --train-manifest data/control/train.jsonl \
  --val-manifest   data/control/val.jsonl \
  --epochs 80 --batch-size 64 --learning-rate 3e-4 \
  --device cuda \
  --export-onnx runs/control/model.onnx

python tests/test_contract.py runs/control/model.onnx
```

### Train/serve parity — the critical invariant

The feature vector fed to the model at training time **must** be produced by the
same `serialize_feature_frame()` code the C++ runtime uses. Do not invent a
second preprocessing path. The normalization scales live inside the model, so
training and serving agree structurally — provided the *input* to both is the
raw `serializeFeatureFrame()` output.

## Deploying the artifact

Once `test_contract.py` passes, the `.onnx` is consumed by
`OnnxNeuralMasteringRunner::loadModel()` in the plugin (once ONNX Runtime is
linked into the C++ build — see the seam's `loadModel` body, which is the
single guarded extension point). The model's `audioCallbackInference` flag must
remain false: inference stays on the message thread and the
`NeuralMasteringSafetyPolicy` clamps every delta before DSP.

## Which model is this?

There are **two** neural mastering paths in this repo:

| | This package (`control/`) | Sibling (`train_neural_mastering.py`) |
|---|---|---|
| Type | Feature→control-delta regressor | Waveform→waveform U-Net+Transformer |
| Params | ≈145k | millions |
| Runs in VST | **Yes** (message thread, via the seam) | No (offline-only per spec) |
| Output | 72 control deltas | Mastered audio |

This package is the one that integrates into the shipped plugin.
