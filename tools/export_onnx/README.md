# SonicMaster ONNX Export

Produces the ONNX model + contract files consumed by `SonicMasterDecisionRunner`
in the plugin's realtime neural mastering path.

## Scripts

| Script | Purpose |
|--------|---------|
| `export_patched.py` | **Primary.** Patches the model for ONNX compatibility (manual MHA replacing `nn.TransformerEncoder`, STFT-based spectral injection replacing `torch.fft.rfft`), exports via TorchScript exporter (opset 17), runs parity check. |
| `masteringbrain_to_onnx.py` | Reference / historical. Attempts a direct export without patches — fails on `aten.fft_rfftfreq` and `aten::_transformer_encoder_layer_fwd`. Kept for comparison. |
| `probe_decisions.py` | Runs the PyTorch checkpoint on synthetic audio for manual inspection. |

## Prerequisites

Python 3.11+ with `torch>=2.10`, `numpy`, `onnxruntime`, `onnx`. The
`sonicmaster-v3-decision-engine` package must be unpacked locally.

```bash
pip install torch numpy onnx onnxruntime
```

## Run (patched export)

From the More-Phi repo root:

```bash
python tools/export_onnx/export_patched.py \
    --package "C:/Users/HP/Downloads/sonicmaster-v3-decision-engine-20260530T121536Z" \
    --output-dir build/sonicmaster \
    --opset 17
```

## Output

| File | Purpose |
|---|---|
| `masteringbrain_v2_decision.onnx` | The exported graph (opset 17, dynamic batch axis). Stage next to the test exe and ship as a plugin resource. |
| `masteringbrain_v2_contract.json` | I/O names + shapes. `SonicMasterDecisionRunner::loadModel` validates the model against this and refuses a mismatched checkpoint. |
| `parity_report.json` | `passed: true` iff max abs diff vs the PyTorch forward < 1e-4. |

## Verification

The export script runs a built-in parity check: it feeds the same 6 s synthetic
seed through both the PyTorch `MasteringDecisionNet.forward()` and the exported
ONNX graph, and asserts the max absolute difference is < 1e-4.

**If parity fails, do not stage the model.** A failure means the export drifted
from the trained checkpoint's behaviour; the runner would then refuse to load it
anyway (its shape check guards against gross mismatches, but the parity check is
the correctness gate). Re-run the export after fixing the cause (common cause:
opset version too old for an op in the graph).

## What the contract pins

```json
{
  "schemaVersion": 1,
  "modelId": "masteringbrain-v2-fullchain-best",
  "inputName": "waveform",
  "outputName": "decision",
  "inputShape": [1, 2, 262138],
  "outputShape": [1, 44],
  "sampleRate": 44100,
  "targetLufsDefault": -14.0
}
```

The C++ runner (`src/AI/SonicMasterDecisionRunner.cpp`) reads this contract at
load time and validates the ONNX session's actual input/output element counts
against `2 * 262138` and `44` respectively. Any mismatch → load fails, the
feature stays unavailable.
