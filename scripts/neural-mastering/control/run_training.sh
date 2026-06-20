#!/usr/bin/env bash
# Turnkey Model A training launcher for the RTX 6000 Blackwell.
# Run from the morephi-control/ dir (the flat control package on Lightning).
# Assumes datasets are already downloaded on this (persisted) studio:
#   data/raw/fma/fma_clean_medium/  (CC-BY-filtered FMA mp3s)
#   data/raw/aam/*.zip              (AAM multitrack archives)
# Does: ensure CUDA torch -> build AAM rough-mix manifest -> merge FMA+AAM
#       -> train on GPU -> export ONNX -> contract test.
set -euo pipefail
cd "$(dirname "$0")"

EPOCHS="${EPOCHS:-80}"
BATCH="${BATCH:-256}"
LR="${LR:-3e-4}"

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

echo "=== 3. AAM stems -> rough mixes -> manifest (build if missing; ~3000 songs) ==="
if [ -d data/raw/aam/stems ] && [ ! -f data/manifest_aam/train.jsonl ]; then
  .venv/bin/python build_aam_roughmixes.py --stems-dir data/raw/aam/stems \
    --out-dir data/raw/aam/roughmix --sample-rate 48000
  .venv/bin/python build_manifest.py --source-dir data/raw/aam/roughmix \
    --out-dir data/manifest_aam --sample-rate 48000 --segment-seconds 10 \
    --train-ratio 0.9 --val-ratio 0.05 --seed 1337 --corpus-name aam-roughmix --license-status approved
fi

echo "=== 4. Merge FMA + AAM (leakage-safe per-source) ==="
INPUTS=()
[ -f data/manifest_fma/train.jsonl ] && INPUTS+=(data/manifest_fma)
[ -f data/manifest_aam/train.jsonl ] && INPUTS+=(data/manifest_aam)
if [ ${#INPUTS[@]} -eq 0 ]; then echo "ERROR: no manifests found"; exit 1; fi
.venv/bin/python merge_manifests.py --inputs "${INPUTS[@]}" \
  --out-dir data/manifest_merged --train-ratio 0.9 --val-ratio 0.05 --seed 1337

echo "=== 5. Train Model A on GPU ==="
.venv/bin/python train.py --data-mode manifest \
  --train-manifest data/manifest_merged/train.jsonl \
  --val-manifest data/manifest_merged/val.jsonl \
  --epochs "$EPOCHS" --batch-size "$BATCH" --learning-rate "$LR" --device cuda \
  --output-dir runs/blackwell --export-onnx runs/blackwell/model_blackwell.onnx

echo "=== 6. Contract test (must PASS before loading into the VST seam) ==="
.venv/bin/python tests/test_contract.py runs/blackwell/model_blackwell.onnx

echo ""
echo "DONE. If contract PASSed, copy runs/blackwell/model_blackwell.onnx"
echo "to the plugin's OnnxNeuralMasteringRunner (once ONNX Runtime is linked)."
