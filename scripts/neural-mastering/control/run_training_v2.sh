#!/usr/bin/env bash
# Retrain v2 — evidence-based rebalance after the combined retrain COLLAPSED.
#
# DIAGNOSIS (audio eval, honest):
#   Combined v1 (34k rows) -> model LUFS -25.09 (WORSE than do-nothing -18.86).
#   Root causes:
#     (1) 24k/34k rows were null/restraint -> model learned near-zero (hiding)
#     (2) synth-B deltas computed in Python FFT-DSP don't transfer to the real
#         C++ AutoMasteringEngine. Only T2 (CMA-ES vs real render) transfers.
#
# FIX v2:
#   - DROP neutral-restraint (10k pure-zero rows poisoned the loss)
#   - KEEP AAM-restraint (real good-input small-moves, the genuine restraint)
#   - DOWNWEIGHT synth-B (cap ~3k for spectral diversity only; it doesn't
#     transfer so it can't drive loudness — keep small)
#   - FOLD IN all 352 T2 anchors (the ONLY teacher that lands near -14 on the
#     real engine; oversample via ConcatDataset duplication)
#   - LOWER --eq-harmonic-l1-mult 6 -> 2 (6 collapsed the deltas)
#   - LOWER overcorrect 0.05 -> 0.02 (let the model act on T2 anchors)
set -euo pipefail
cd "$(dirname "$0")"

EPOCHS="${EPOCHS:-70}"
BATCH="${BATCH:-256}"
LR="${LR:-3e-4}"
DELTA_L1="${DELTA_L1:-0.01}"
OVERCORRECT="${OVERCORRECT:-0.02}"
EQ_HARM_MULT="${EQ_HARM_MULT:-2}"
OUT="${OUT:-runs/blackwell_v2}"

# Subsample synth-B to ~3k for diversity (it doesn't transfer; keep small).
SYNTH_B_TRAIN=data/manifest_synthetic_b/train_3k.jsonl
head -n 3000 data/manifest_synthetic_b/train.jsonl > "$SYNTH_B_TRAIN"

TRAIN_MANIFESTS=("$SYNTH_B_TRAIN"
                 data/manifest_aam_restraint/train.jsonl)
VAL_MANIFESTS=(data/manifest_synthetic_b/val.jsonl
               data/manifest_aam_restraint/val.jsonl)

# Fold in T2 anchors (the only teacher that lands near -14 on the real engine).
# Duplicate them 8x so their signal isn't drowned by 14k restraint rows.
if [ -s data/manifest_fma_t2/train.jsonl ]; then
  T2_OVER=data/manifest_fma_t2/train_8x.jsonl
  for i in $(seq 1 8); do cat data/manifest_fma_t2/train.jsonl; done > "$T2_OVER"
  TRAIN_MANIFESTS+=("$T2_OVER")
  echo "[+] T2 anchors included (352 x8 = $((352*8)) rows oversampled)"
else
  echo "[-] T2 manifest missing/empty — ABORT (T2 is the only transfer teacher)"
  exit 1
fi

.venv/bin/python -c "import torch; assert torch.cuda.is_available(), 'CUDA required'; print('GPU:', torch.cuda.get_device_name(0))"

TRAIN_ARGS=""; for m in "${TRAIN_MANIFESTS[@]}"; do TRAIN_ARGS="$TRAIN_ARGS $m"; done
VAL_ARGS=""; for m in "${VAL_MANIFESTS[@]}"; do VAL_ARGS="$VAL_ARGS $m"; done

echo "=== v2 retrain (rebalanced): ${TRAIN_MANIFESTS[*]} ==="
.venv/bin/python train.py --data-mode manifest \
  --train-manifest $TRAIN_ARGS \
  --val-manifest $VAL_ARGS \
  --epochs "$EPOCHS" --batch-size "$BATCH" --learning-rate "$LR" --device cuda \
  --delta-l1-weight "$DELTA_L1" --overcorrect-weight "$OVERCORRECT" \
  --eq-harmonic-l1-mult "$EQ_HARM_MULT" \
  --gated-head --no-zero-init \
  --output-dir "$OUT" \
  --export-onnx "$OUT/model_blackwell_v2.onnx"

echo "=== Contract test (must PASS) ==="
.venv/bin/python tests/test_contract.py "$OUT/model_blackwell_v2.onnx"

echo "=== DONE -> evaluate_student_audio.py --model $OUT/model_blackwell_v2.onnx"
