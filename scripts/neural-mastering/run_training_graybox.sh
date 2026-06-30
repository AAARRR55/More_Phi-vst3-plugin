#!/usr/bin/env bash
# F2 gray-box differentiable-compressor baseline on SolidStateBusComp (Lightning).
#
# 5 learnable params -> the interpretable lower bound the black-box
# HybridMasteringNet (F1) must beat. fp32 (the recurrence + handful of params
# make AMP unnecessary and fp16 would erode the smoothing precision). Reuses the
# manifest staged by run_training_waveform.sh if present, else builds+stages it.
#
# Run from scripts/neural-mastering/ on Lightning.
set -euo pipefail
cd "$(dirname "$0")"

EPOCHS="${EPOCHS:-40}"          # few params -> converges fast
BATCH="${BATCH:-8}"
LR="${LR:-5e-3}"               # large LR ok for a 5-param model
HOP="${HOP:-256}"              # control-block size (~5.3 ms @ 48k)
GR_W="${GR_W:-0.5}"            # gain-reduction-curve loss weight
MAX_SONGS="${MAX_SONGS:-20}"
OUT="${OUT:-runs/ssbc_graybox}"
DATA="${DATA:-data/ssbc}"
PY="${PY:-python}"             # on Lightning: point at a venv with torch+torchaudio+huggingface_hub

$PY -c "import torch; assert torch.cuda.is_available(), 'CUDA required'; print('GPU:', torch.cuda.get_device_name(0))"

MANIFEST="${DATA}/manifest.json"
if [ ! -f "$MANIFEST" ]; then
  echo "=== Staging manifest + audio (curriculum: ${MAX_SONGS} songs) ==="
  $PY build_solidstatebuscomp_manifest.py --mode hf --repo amphion/SolidStateBusComp \
    --out "$MANIFEST" --max-songs "$MAX_SONGS" --holdout-combo-fraction 0.1 \
    --audit-report "${DATA}/g10_audit.json" --materialize
fi

echo "=== Train gray-box compressor (fp32, ${HOP}-sample control blocks) ==="
$PY train_graybox_compressor.py \
  --manifest "$MANIFEST" --output-dir "$OUT" \
  --epochs "$EPOCHS" --batch-size "$BATCH" --learning-rate "$LR" \
  --hop "$HOP" --gr-loss-weight "$GR_W" --precision fp32 --device cuda

echo ""
echo "DONE. Learned device characterization: ${OUT}/learned_params.json ; checkpoint: ${OUT}/best.pt"
echo "CC-BY-NC: research/evaluation only — weights are NOT commercial-ship-eligible."
