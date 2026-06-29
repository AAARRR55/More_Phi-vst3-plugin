#!/usr/bin/env bash
# Waveform-line (HybridMasteringNet) training of SolidStateBusComp on Lightning.
#
# Distinct from control/run_training_t2.sh (the control-regressor line): this
# trains the waveform->waveform FORWARD model (train_neural_mastering.py) on the
# SolidStateBusComp paired ground truth (unmastered input -> 220 hardware
# compressor outputs), not the feature->delta control regressor.
#
# Staging model mirrors the established t2 pattern: audio is fetched from the
# gated HF dataset into data/ BEFORE training; ${HF_TOKEN} is a Lightning secret
# (never committed); the manifest's repo-relative paths then resolve under data/
# for torchaudio.load. The full corpus is ~2.6 TB, so --max-songs caps a
# curriculum subset.
#
# Run from scripts/neural-mastering/ on Lightning.
set -euo pipefail
cd "$(dirname "$0")"

EPOCHS="${EPOCHS:-80}"
BATCH="${BATCH:-4}"
LR="${LR:-2e-4}"
SEGMENT_S="${SEGMENT_S:-5.46}"
MAX_SONGS="${MAX_SONGS:-20}"   # curriculum cap; full corpus = 175 songs (~2.6 TB)
OUT="${OUT:-runs/ssbc_waveform}"
DATA="${DATA:-data/ssbc}"
PY="${PY:-python}"             # on Lightning: point at a venv with torch+torchaudio+huggingface_hub
EQ_AUGMENT="${EQ_AUGMENT:-1}"        # EQ axis on (P4: POST-EQ co-conditioning; P3: was input-only aug). 0 = e pinned to identity
EQ_AUGMENT_DB="${EQ_AUGMENT_DB:-6}"  # EQ magnitude (dB) at 180/1k/6k anchors — also the e-axis range
EQ_PROXY="${EQ_PROXY:-0}"            # eval-only F2-graybox proxy D(A(x)) ceiling indicator (default off)

$PY -c "import torch; assert torch.cuda.is_available(), 'CUDA required'; print('GPU:', torch.cuda.get_device_name(0))"

echo "=== Build manifest + stage audio (curriculum: ${MAX_SONGS} songs) ==="
$PY build_solidstatebuscomp_manifest.py \
  --mode hf --repo amphion/SolidStateBusComp \
  --out "${DATA}/manifest.json" \
  --max-songs "$MAX_SONGS" --holdout-combo-fraction 0.1 \
  --audit-report "${DATA}/g10_audit.json" --materialize

echo "=== Train FiLM-conditioned HybridMasteringNet (controllable waveform -> waveform) ==="
$PY train_conditioned_mastering.py \
  --manifest "${DATA}/manifest.json" \
  --output-dir "$OUT" \
  --epochs "$EPOCHS" --batch-size "$BATCH" --learning-rate "$LR" \
  --segment-seconds "$SEGMENT_S" --precision bf16 --device cuda \
  --train-split train --val-split val \
  --eq-augment-db "$EQ_AUGMENT_DB" \
  $( [ "$EQ_AUGMENT" = "0" ] && echo --no-eq-augment ) \
  $( [ "$EQ_PROXY" = "1" ] && echo --eq-proxy )

echo ""
echo "DONE. Manifest + G10 audit: ${DATA}/ ; best checkpoint: ${OUT}/best.pt"
echo "CC-BY-NC: research/evaluation only — weights are NOT commercial-ship-eligible."
