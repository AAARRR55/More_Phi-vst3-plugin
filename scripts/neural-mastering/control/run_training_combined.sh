#!/usr/bin/env bash
# Combined retrain of Model A on the RTX PRO 6000 Blackwell.
#
# Data philosophy: multiple complementary teachers in one concat dataset.
#   - synth-B (10k)      : bulk spectral/loudness coverage (cheap closed-form teacher)
#   - AAM-restraint (14k): do-nothing on already-good human mixes (cures over-correction)
#   - neutral-restraint  : synthetic null-pair corpus (zero-delta on flat input)
#   - T2 (optional, ~400): CMA-ES anchors vs the REAL DSP render (high accuracy)
# The T2 manifest is included only if it exists; the run is valid without it.
#
# Flags:
#   --gated-head --no-zero-init : residual gate so model can output ZERO on good input
#   --eq-harmonic-l1-mult 6     : penalize EQ+harmonic deltas 6x (restraint)
#   --overcorrect-weight 0.05   : asymmetric penalty for over-correction
set -euo pipefail
cd "$(dirname "$0")"

EPOCHS="${EPOCHS:-60}"
BATCH="${BATCH:-256}"
LR="${LR:-3e-4}"
DELTA_L1="${DELTA_L1:-0.02}"
OVERCORRECT="${OVERCORRECT:-0.05}"
EQ_HARM_MULT="${EQ_HARM_MULT:-6}"
OUT="${OUT:-runs/blackwell_combined}"

TRAIN_MANIFESTS=(data/manifest_synthetic_b/train.jsonl
                 data/manifest_aam_restraint/train.jsonl
                 data/manifest_neutral_restraint_10k/train.jsonl)
VAL_MANIFESTS=(data/manifest_synthetic_b/val.jsonl
               data/manifest_aam_restraint/val.jsonl
               data/manifest_neutral_restraint_10k/val.jsonl)

# Add T2 anchors if present AND non-empty (CMA-ES labels — high accuracy)
if [ -s data/manifest_fma_t2/train.jsonl ]; then
  TRAIN_MANIFESTS+=(data/manifest_fma_t2/train.jsonl)
  echo "[+] T2 train anchors found and included"
else
  echo "[-] T2 train manifest missing/empty — skipping (retrain is valid without it)"
fi
if [ -s data/manifest_fma_t2/val.jsonl ]; then
  VAL_MANIFESTS+=(data/manifest_fma_t2/val.jsonl)
fi

.venv/bin/python -c "import torch; assert torch.cuda.is_available(), 'CUDA required'; print('GPU:', torch.cuda.get_device_name(0))"

echo "=== Combined retrain: ${TRAIN_MANIFESTS[*]} ==="
TRAIN_ARGS=""
for m in "${TRAIN_MANIFESTS[@]}"; do TRAIN_ARGS="$TRAIN_ARGS $m"; done
VAL_ARGS=""
for m in "${VAL_MANIFESTS[@]}"; do VAL_ARGS="$VAL_ARGS $m"; done

.venv/bin/python train.py --data-mode manifest \
  --train-manifest $TRAIN_ARGS \
  --val-manifest $VAL_ARGS \
  --epochs "$EPOCHS" --batch-size "$BATCH" --learning-rate "$LR" --device cuda \
  --delta-l1-weight "$DELTA_L1" --overcorrect-weight "$OVERCORRECT" \
  --eq-harmonic-l1-mult "$EQ_HARM_MULT" \
  --gated-head --no-zero-init \
  --output-dir "$OUT" \
  --export-onnx "$OUT/model_blackwell_combined.onnx"

echo "=== Contract test (must PASS) ==="
.venv/bin/python tests/test_contract.py "$OUT/model_blackwell_combined.onnx"

echo "=== Restraint characterization gate ==="
.venv/bin/python characterize_model.py "$OUT/model_blackwell_combined.onnx" \
  --fail-neutral-max-delta 0.08 || echo "(restraint gate soft-fail — not blocking eval)"

echo ""
echo "DONE. Next: evaluate_student_audio.py --model $OUT/model_blackwell_combined.onnx"
