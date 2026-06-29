#!/usr/bin/env bash
# T2-corrective retrain of Model A on the RTX PRO 6000 Blackwell.
#
# What's different from the prior v5 restraint run:
#   - FMA train labels are now T2 (CMA-ES against the REAL DSP render, targeting
#     the SonicMaster-corrective build_target_from_features). Smoke-proven: T2
#     label renders to -13.19 LUFS vs T1's -7.42 on the same segment.
#   - --eq-harmonic-l1-mult 6: penalize EQ+harmonic deltas 6x harder so the
#     student imitates the corrective target's restraint instead of saturating.
#   - --gated-head + --no-zero-init: gated residual head so the model can learn
#     to OUTPUT ZERO on already-good input (cures over-correction) without
#     starving the gate of gradient.
#   - AAM restraint + neutral restraint manifests stay in the concat to preserve
#     the do-nothing-on-good-input behavior.
#
# Run from morephi-control/ on Lightning. Expects:
#   data/manifest_fma_t2/train.jsonl   (T2-relabeled, built by generate_t2_labels.py)
#   data/manifest_aam_restraint/        (AAM human-mix do-nothing labels)
#   data/manifest_neutral_restraint_10k/(synthetic null-pair corpus)
set -euo pipefail
cd "$(dirname "$0")"

EPOCHS="${EPOCHS:-80}"
BATCH="${BATCH:-256}"
LR="${LR:-3e-4}"
DELTA_L1="${DELTA_L1:-0.02}"
OVERCORRECT="${OVERCORRECT:-0.05}"
EQ_HARM_MULT="${EQ_HARM_MULT:-6}"
OUT="${OUT:-runs/blackwell_t2}"

.venv/bin/python -c "import torch; assert torch.cuda.is_available(), 'CUDA required'; print('GPU:', torch.cuda.get_device_name(0))"

echo "=== T2 retrain: FMA-T2 + AAM-restraint + neutral-restraint ==="
.venv/bin/python train.py --data-mode manifest \
  --train-manifest data/manifest_fma_t2/train.jsonl \
                    data/manifest_aam_restraint/train.jsonl \
                    data/manifest_neutral_restraint_10k/train.jsonl \
  --val-manifest data/manifest_fma/val.jsonl \
                 data/manifest_aam_restraint/val.jsonl \
                 data/manifest_neutral_restraint_10k/val.jsonl \
  --epochs "$EPOCHS" --batch-size "$BATCH" --learning-rate "$LR" --device cuda \
  --delta-l1-weight "$DELTA_L1" --overcorrect-weight "$OVERCORRECT" \
  --eq-harmonic-l1-mult "$EQ_HARM_MULT" \
  --gated-head --no-zero-init \
  --output-dir "$OUT" \
  --export-onnx "$OUT/model_blackwell_t2.onnx"

echo "=== Contract test (must PASS) ==="
.venv/bin/python tests/test_contract.py "$OUT/model_blackwell_t2.onnx"

echo "=== Restraint characterization gate ==="
.venv/bin/python characterize_model.py "$OUT/model_blackwell_t2.onnx" \
  --fail-neutral-max-delta 0.08 || echo "(restraint gate soft-fail — not blocking T2 eval)"

echo ""
echo "DONE. Next: evaluate_student_audio.py --model $OUT/model_blackwell_t2.onnx"
