#!/usr/bin/env bash
# Turnkey Model A training launcher for the RTX 6000 Blackwell.
# Run from the morephi-control/ dir (the flat control package on Lightning).
# Assumes datasets are already downloaded on this (persisted) studio:
#   data/raw/fma/fma_clean_medium/  (CC-BY-filtered FMA mp3s)
#   data/raw/aam/*.zip              (AAM multitrack archives)
# Does: ensure CUDA torch -> build correction + restraint manifests -> train
#       the v5 restraint recipe on GPU -> export ONNX -> contract/restraint gates.
set -euo pipefail
cd "$(dirname "$0")"

EPOCHS="${EPOCHS:-80}"
BATCH="${BATCH:-256}"
LR="${LR:-3e-4}"
DELTA_L1="${DELTA_L1:-0.01}"
OVERCORRECT="${OVERCORRECT:-0.02}"
RESTRAINT_GATE_MAX_DELTA="${RESTRAINT_GATE_MAX_DELTA:-0.08}"
NEUTRAL_TRAIN_COUNT="${NEUTRAL_TRAIN_COUNT:-10000}"
NEUTRAL_VAL_COUNT="${NEUTRAL_VAL_COUNT:-1000}"
RUN_DIR="${RUN_DIR:-runs/blackwell_restraint_v5}"
MODEL_OUT="${MODEL_OUT:-$RUN_DIR/model_blackwell_restraint_v5.onnx}"

echo "=== 1. ensure CUDA torch (current venv is +cpu) ==="
if ! .venv/bin/python -c "import torch; assert torch.cuda.is_available()" 2>/dev/null; then
  echo "CUDA not available in venv (torch is +cpu) — installing CUDA build..."
  .venv/bin/pip install --upgrade torch --index-url https://download.pytorch.org/whl/cu124
fi
.venv/bin/python -c "import torch; print('torch', torch.__version__, '| GPU:', torch.cuda.get_device_name(0) if torch.cuda.is_available() else 'NONE - ABORT'); assert torch.cuda.is_available()"

echo "=== 2. FMA manifest (build if missing) ==="
if [ ! -f data/manifest_fma/train.jsonl ]; then
  .venv/bin/python build_manifest.py --source-dir data/raw/fma/fma_clean_medium \
    --out-dir data/manifest_fma --sample-rate 48000 --segment-seconds 10 \
    --train-ratio 0.9 --val-ratio 0.05 --seed 1337 --corpus-name fma-ccby --license-status approved
fi

echo "=== 3. AAM stems -> rough mixes -> correction/restraint manifests ==="
if [ -d data/raw/aam/stems ] && [ ! -d data/raw/aam/roughmix ]; then
  .venv/bin/python build_aam_roughmixes.py --stems-dir data/raw/aam/stems \
    --out-dir data/raw/aam/roughmix --sample-rate 48000
fi

AAM_MIX_SOURCE=""
if [ -d data/raw/aam/roughmix ]; then
  AAM_MIX_SOURCE="data/raw/aam/roughmix"
elif [ -d data/raw/aam/mixes ]; then
  AAM_MIX_SOURCE="data/raw/aam/mixes"
fi

if [ -n "$AAM_MIX_SOURCE" ] && [ ! -f data/manifest_aam/train.jsonl ]; then
  .venv/bin/python build_manifest.py --source-dir "$AAM_MIX_SOURCE" \
    --out-dir data/manifest_aam --sample-rate 48000 --segment-seconds 10 \
    --train-ratio 0.9 --val-ratio 0.05 --seed 1337 --corpus-name aam-roughmix --license-status approved
fi
if [ -n "$AAM_MIX_SOURCE" ] && [ ! -f data/manifest_aam_restraint/train.jsonl ]; then
  .venv/bin/python build_manifest.py --source-dir "$AAM_MIX_SOURCE" \
    --out-dir data/manifest_aam_restraint --sample-rate 48000 --segment-seconds 10 \
    --train-ratio 0.9 --val-ratio 0.05 --seed 2337 --corpus-name aam-roughmix-restraint \
    --license-status approved --zero-labels
fi

echo "=== 4. Neutral restraint manifest (pin already-good region) ==="
if [ ! -f data/manifest_neutral_restraint_10k/train.jsonl ]; then
  .venv/bin/python generate_neutral_restraint_manifest.py \
    --out-dir data/manifest_neutral_restraint_10k \
    --train-count "$NEUTRAL_TRAIN_COUNT" --val-count "$NEUTRAL_VAL_COUNT" --seed 7333
fi

echo "=== 5. Train Model A on GPU (v5 restraint recipe) ==="
.venv/bin/python train.py --data-mode manifest \
  --train-manifest data/manifest_fma/train.jsonl data/manifest_aam_restraint/train.jsonl data/manifest_neutral_restraint_10k/train.jsonl \
  --val-manifest data/manifest_fma/val.jsonl data/manifest_aam_restraint/val.jsonl data/manifest_neutral_restraint_10k/val.jsonl \
  --epochs "$EPOCHS" --batch-size "$BATCH" --learning-rate "$LR" --device cuda \
  --delta-l1-weight "$DELTA_L1" --overcorrect-weight "$OVERCORRECT" \
  --output-dir "$RUN_DIR" --export-onnx "$MODEL_OUT"

echo "=== 6. Contract test (must PASS before loading into the VST seam) ==="
.venv/bin/python tests/test_contract.py "$MODEL_OUT"

echo "=== 7. Restraint characterization gate (already-good input should stay subtle) ==="
.venv/bin/python characterize_model.py "$MODEL_OUT" \
  --fail-neutral-max-delta "$RESTRAINT_GATE_MAX_DELTA"

echo ""
echo "DONE. If contract + restraint gates PASSed, copy $MODEL_OUT"
echo "to scripts/neural-mastering/control/model_blackwell_restraint_v5.onnx for plugin/test staging."
