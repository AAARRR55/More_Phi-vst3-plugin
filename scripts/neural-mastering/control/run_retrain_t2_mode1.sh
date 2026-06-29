#!/usr/bin/env bash
# Retrain on T2 mode-1 labels (reweighted loss: loudness live, eq_mag penalty).
# CPU-only (no CUDA in this env); restraint recipe flags kept.
# Expects: data/manifest_ssl_t2/train.jsonl (relabel output) to exist & be non-empty.
set -euo pipefail
cd "$(dirname "$0")"

PY="${PY:-C:/Users/HP/AppData/Local/Python/pythoncore-3.14-64/python.exe}"
TRAIN=data/manifest_ssl_t2/train.jsonl
VAL=data/manifest_ssl_t1/val.jsonl   # T1 val for loss-tracking only (mode-agnostic features)
OUT=runs/blackwell_t2_mode1

if [ ! -s "$TRAIN" ]; then
  echo "[-] $TRAIN missing/empty — relabel must finish first. ABORT."
  exit 1
fi
echo "[+] training on $(wc -l < "$TRAIN") T2 mode-1 rows"

"$PY" train.py --data-mode manifest \
  --train-manifest "$TRAIN" \
  --val-manifest "$VAL" \
  --epochs 60 --batch-size 32 --learning-rate 3e-4 --device cpu \
  --delta-l1-weight 0.02 --overcorrect-weight 0.02 \
  --eq-harmonic-l1-mult 2 \
  --gated-head --no-zero-init \
  --output-dir "$OUT" \
  --export-onnx "$OUT/model_blackwell_t2_mode1.onnx"

echo "[+] DONE -> evaluate:"
echo "  $PY evaluate_student_audio.py --model $OUT/model_blackwell_t2_mode1.onnx --lib <dll> --manifest data/manifest_ssl_eval/val.jsonl --normalizer-mode 1"
