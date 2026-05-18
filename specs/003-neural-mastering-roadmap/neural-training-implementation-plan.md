# Neural Mastering Technical Implementation Plan

**Feature**: `003-neural-mastering-roadmap`  
**Created**: 2026-05-18  
**Runtime posture**: offline training and validation only. No model inference, file I/O, allocation, or learned audio generation is introduced into `processBlock()`.

## 1. Full Model Architecture

### 1.1 Target Task

Train an offline stereo mastering model that maps an unmastered mix segment to a mastered reference segment while preserving musical intent, stereo phase integrity, transient detail, true-peak headroom, and loudness consistency.

**Canonical tensor shape**:

```text
input waveform:  [batch, 2, samples]
target waveform: [batch, 2, samples]
sample rate:     48 kHz default, 44.1/88.2/96 kHz allowed after resampling policy validation
segment length:  262,144 samples default (~5.46 s at 48 kHz)
```

Segments shorter than the canonical length are right-padded for training. Longer files are randomly cropped during training and deterministically center-cropped during validation. Stereo is mandatory for the core model; mono sources are duplicated to stereo and tagged in metadata.

### 1.2 Architecture: Hybrid Waveform CNN-Transformer

The baseline production architecture is `HybridMasteringNet`, implemented in [train_neural_mastering.py](../../scripts/neural-mastering/train_neural_mastering.py). It is a waveform-to-waveform U-Net with a long-context Transformer bottleneck and mid/side-aware input channels.

**Input stem**:

- Channels: left, right, mid, side.
- Initial convolution: `4 -> 64`, kernel size 7.
- Normalization: group normalization.
- Activation: SiLU.

**Encoder**:

| Stage | Channels | Stride | Effective Role |
|-------|----------|--------|----------------|
| E1 | 64 | 4 | transient and local spectral features |
| E2 | 128 | 4 | band-level dynamics and balance |
| E3 | 192 | 4 | phrase-scale envelope features |
| E4 | 256 | 4 | long-range tonal and loudness context |
| E5 | 384 | 4 | track-section mastering intent |

Each stage uses a residual convolution block plus a gated depthwise convolution block. The default total downsampling factor is 1024, so a 262,144-sample segment becomes a 256-step sequence for the bottleneck.

**Long-range temporal dependency module**:

- Transformer encoder depth: 6 layers by default.
- Embedding dimension: 384.
- Attention heads: 8.
- Feed-forward expansion: 4x.
- Positional encoding: sinusoidal in the reference script; the architecture reserves a direct upgrade path for RoPE or ALiBi if later benchmarks justify it.
- Attention scope: full attention at the downsampled sequence rate. For longer segments, replace full attention with block-local attention plus global summary tokens.

**Decoder**:

- Linear interpolation upsampling to skip length.
- Skip concatenation from each encoder stage.
- Residual convolution refinement after each upsample.
- Final projection: `64 -> 2`, kernel size 7.
- Output policy: residual mastering delta added to the dry stereo waveform with bounded `tanh` projection.

**Why residual prediction**: Mastering should usually make restrained, high-quality changes. Predicting a bounded delta around the input reduces the risk of hallucinated structure and improves identity transparency.

### 1.3 Alternative Architecture Gate: Diffusion Refinement

A diffusion-based residual refiner is allowed only as a later research phase. It must operate offline and generate a mastered residual conditioned on the unmastered waveform plus feature frames. It is not the default because iterative denoising increases training and validation cost, complicates phase safety, and has no current proof for DAW-runtime suitability.

If pursued, the diffusion refiner must use:

- Latent or STFT-domain residual prediction, not unconstrained raw stereo generation.
- Deterministic DDIM-style validation sampling for repeatability.
- A hard output projection through the same safety and objective metric gates as the CNN-Transformer model.

### 1.4 Loss Functions

The training script combines differentiable waveform, spectral, stereo, transient, and optional perceptual feature losses:

| Loss | Purpose |
|------|---------|
| Multi-resolution STFT spectral convergence | Match high-level spectral energy over multiple time-frequency resolutions |
| Multi-resolution log-magnitude STFT L1 | Preserve harmonic detail and suppress metallic artifacts |
| Waveform L1 | Preserve phase-aligned transients and avoid drift |
| Mid/side L1 | Preserve stereo center and side-field detail |
| Transient derivative L1 | Penalize softened attacks and spurious clicks |
| Loudness proxy loss | Keep integrated energy close before external LUFS validation |
| Stereo correlation/width loss | Avoid mono incompatibility and excessive side energy |
| VGG-style feature matching | Compare log-spectrogram activations from a frozen spectro-temporal feature network |

The VGG-style feature matcher is intentionally checkpoint-gated. If `--feature-loss-weight > 0`, the training run must provide `--feature-checkpoint`; random feature extractors are rejected.

### 1.5 Validation Metrics

Validation reports include metrics beyond MSE:

- L1 and MSE.
- SI-SDR.
- Multi-resolution STFT loss.
- Log-spectral distance.
- Loudness error.
- Stereo correlation error.
- Mid/side width ratio error.
- Optional PEAQ or VISQOL command integration via user-provided command templates.

PEAQ/VISQOL are treated as external validators because licensing and distribution vary by environment. The script writes temporary WAV pairs and parses numeric MOS/ODG-style values when command templates are configured.

## 2. Automated Dataset Generation Pipeline

The dataset generator is implemented in [generate_mastering_dataset.py](../../scripts/neural-mastering/generate_mastering_dataset.py).

### 2.1 Dataset Modes

**Professional paired mode**:

- Input manifest provides explicit unmastered/mastered file pairs.
- Files are resampled, stereo-normalized, cropped into aligned segments, loudness-normalized, and written into the canonical dataset layout.
- Metadata preserves source IDs, provenance, license status, session IDs, split hints, sample rate, LUFS method, and feature summaries.

**Synthetic pair mode**:

- Input is clean or near-unmastered audio.
- The generator creates an "unmastered" side using stochastic mix-level variations.
- The generator creates a "mastered" side using an algorithmic mastering chain.
- Both sides share the same source segment, so alignment is sample-accurate.

**Reference demastering mode**:

- Input is a mastered reference.
- Target is the reference after canonical normalization.
- Input is a demastered variant with reduced loudness, altered spectral balance, softened or exaggerated transients, and narrower or wider stereo image.
- This mode is useful for bootstrapping but must be labeled as synthetic-demaster data.

### 2.2 Pairwise Synthesis Chain

For each selected source segment:

1. Load audio and validate finite values.
2. Convert to stereo.
3. Resample to the dataset sample rate.
4. Crop or pad to the configured segment length.
5. Peak-limit to safe floating-point headroom.
6. Build unmastered input:
   - Random gain and LUFS target around mix-like loudness.
   - Spectral balance perturbation using smooth low/mid/high anchors.
   - Stochastic transient shaping.
   - Stereo width variation.
7. Build mastered target:
   - Corrective spectral tilt and gentle high-frequency contour.
   - Bus compression with configurable threshold/ratio.
   - Low-level saturation.
   - Stereo width normalization with mono-compatible bounds.
   - Limiting to true-peak-style ceiling proxy.
   - LUFS standardization.
8. Compute metadata features.
9. Assign train/validation/test splits by source identity to reduce leakage.
10. Write WAV pairs and manifest entries.

### 2.3 Feature Extraction and Standardization

Each manifest item records:

- Sample rate, channel count, duration, peak, RMS, crest factor.
- LUFS estimate and method (`pyloudnorm` when available, explicit RMS proxy otherwise).
- Stereo correlation and width ratio.
- Spectral centroid and low/mid/high band energy.
- Augmentation parameters used for transient response, spectral balance, stereo width, compression, limiting, and gain.

The generator does not silently claim broadcast-compliant LUFS when `pyloudnorm` is unavailable. The `lufsMethod` field distinguishes measured LUFS from RMS proxy loudness.

### 2.4 Data Augmentation

Training robustness is supported by controlled stochastic variation:

- Transient response: high-frequency residual boost/cut using moving-average residuals.
- Spectral balance: smooth low/mid/high EQ curves in the frequency domain.
- Stereo width: mid/side side-channel gain with correlation bounds.
- Dynamics: bus compression threshold, ratio, and envelope window.
- Harmonic content: bounded tanh saturation drive.
- Loudness: target LUFS jitter within configured bounds.

Augmentation parameters are written into the manifest to preserve reproducibility and enable leakage audits.

## 3. End-to-End Training Script

The training framework is implemented in [train_neural_mastering.py](../../scripts/neural-mastering/train_neural_mastering.py).

### 3.1 Training Features

- PyTorch training loop with `torch.amp.autocast`.
- FP32, FP16, and BF16 precision modes.
- Gradient scaling for FP16.
- Cosine learning-rate decay with warmup.
- Gradient accumulation and clipping.
- DataLoader tuning: pinned memory, persistent workers, configurable prefetching, and worker count.
- JSONL training logs.
- Best/last checkpoint writing.
- Resume support.
- Optional `torch.compile`.
- External PEAQ/VISQOL validation hooks.

### 3.2 Manifest Contract

The script expects a manifest with items containing either `inputPath`/`targetPath` or `unmasteredPath`/`masteredPath`. Relative paths are resolved against the manifest file.

```json
{
  "schemaVersion": 1,
  "sampleRate": 48000,
  "items": [
    {
      "id": "song_a_seg_0000",
      "split": "train",
      "inputPath": "input/song_a_seg_0000.wav",
      "targetPath": "target/song_a_seg_0000.wav",
      "sourceId": "song_a",
      "licenseStatus": "approved",
      "provenanceComplete": true
    }
  ]
}
```

### 3.3 Example Commands

Generate synthetic pairs:

```powershell
python scripts/neural-mastering/generate_mastering_dataset.py `
  --source-dir data/raw-mixes `
  --output-dir data/neural-mastering `
  --mode synthesize-mastered `
  --sample-rate 48000 `
  --segment-seconds 5.46 `
  --pairs-per-file 4 `
  --train-ratio 0.8 `
  --val-ratio 0.1 `
  --target-lufs -14 `
  --seed 1337
```

Train with BF16 on a supported GPU:

```powershell
python scripts/neural-mastering/train_neural_mastering.py `
  --manifest data/neural-mastering/manifest.json `
  --output-dir runs/neural-mastering/baseline `
  --precision bf16 `
  --batch-size 4 `
  --epochs 80 `
  --num-workers 8 `
  --prefetch-factor 4
```

Enable external VISQOL validation for a small validation subset:

```powershell
python scripts/neural-mastering/train_neural_mastering.py `
  --manifest data/neural-mastering/manifest.json `
  --output-dir runs/neural-mastering/visqol-check `
  --visqol-command "visqol --reference_file {ref} --degraded_file {deg} --output_csv {out}" `
  --external-metric-samples 8
```

### 3.4 Release Gates

Training artifacts must not be promoted to product behavior until:

- Dataset audit passes provenance, license, and split-leakage checks.
- Objective validation passes spectral, loudness, dynamics, stereo, true-peak, and artifact gates.
- PEAQ/VISQOL or equivalent perceptual reports are stored with the model card.
- Blind/expert listening artifacts are level-matched and reviewed.
- Model output remains offline, review-only, or Safety Layer constrained unless later runtime gates approve a broader posture.
