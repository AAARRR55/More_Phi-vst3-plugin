# Dataset Framework Completion Design

**Date:** 2026-03-07
**Status:** Approved
**Approach:** Parallel Specialist Agents

## Overview

Complete the missing pieces of the Synthetic Audio-Parameter Dataset Framework by implementing TimeStretch/PitchShift phase vocoder, adding unit tests for uncovered modules, and validating end-to-end with stress testing.

## Current State

The framework is ~85% complete with the following already implemented:
- DatasetGeneratorV2/V3 (sequential and async pipelines)
- ParameterSampler (LHS, stratified sampling)
- FeatureExtractor (31-dim MFCC, spectral, temporal, perceptual)
- AudioAugmentation (noise, gain, dynamics, freq/time masking - **TimeStretch/PitchShift are placeholders**)
- ParameterNormalization (MinMax, ZScore, LogMinMax, RobustScaling)
- ValidationEngine (KS test, MMD, coverage)
- MetadataWriter + DatasetOrganizer
- CheckpointManager, WatchdogTimer, ResourceMonitor, WorkerPool

## Missing Pieces

1. **Phase Vocoder Implementation** - TimeStretch/PitchShift are no-op placeholders
2. **Unit Test Coverage** - AudioAugmentation and ParameterNormalization lack tests
3. **Integration Validation** - Need stress testing with 10,000+ samples

## Agent Team Structure

### Agent 1: DSP Agent (Phase Vocoder)

**File:** `src/AI/Dataset/PhaseVocoder.h` (new, ~300 lines)

```cpp
class PhaseVocoder {
public:
    void prepare(double sampleRate, int fftSize = 2048);
    void processTimeStretch(juce::AudioBuffer<float>& buffer,
                            float stretchRatio,   // 0.5 = 2x slower, 2.0 = 2x faster
                            juce::Random& rng);
    void processPitchShift(juce::AudioBuffer<float>& buffer,
                           float semitones,        // -12 to +12
                           juce::Random& rng);
private:
    juce::dsp::FFT fft_;
    std::vector<float> phaseAccumulator_;
    std::vector<float> previousPhase_;
    double sampleRate_ = 48000.0;
    int fftSize_ = 2048;
    int hopSize_ = 512;
    int analysisHopSize_ = 512;
};
```

**Algorithm:**
1. STFT with Hann window, 75% overlap
2. Phase propagation using phase vocoder identity
3. For time-stretch: modify hop size ratio
4. For pitch-shift: resample in frequency domain (phase locking)
5. Overlap-add resynthesis

**Integration:**
- Add `#include "PhaseVocoder.h"` to `AudioAugmentation.h`
- Add `std::unique_ptr<PhaseVocoder> phaseVocoder_` member to `AudioAugmenter`
- Update `applyAugmentation()` switch for TimeStretch/PitchShift cases

### Agent 2: Test Agent (Unit Tests)

**File:** `tests/Unit/TestDatasetModules.cpp` (extend, ~320 new lines)

**AudioAugmentation Tests:**
- `AudioAugmenter: noise injection changes RMS`
- `AudioAugmenter: gain change applies correct dB`
- `AudioAugmenter: dynamic processing reduces peaks`
- `AudioAugmenter: frequency mask zeros target band`
- `AudioAugmenter: time mask silences segment`
- `AudioAugmenter: probability controls application rate`
- `ParameterAugmenter: noise clamps to [0,1]`
- `ParameterAugmenter: interpolation blends correctly`
- `ParameterAugmenter: extrapolation respects bounds`
- `AugmentationChainPreset: default chain has 5 augmentations`

**ParameterNormalization Tests:**
- `ParameterNormalizer: MinMax maps to [0,1]`
- `ParameterNormalizer: ZScore produces zero mean`
- `ParameterNormalizer: LogMinMax handles frequency range`
- `ParameterNormalizer: discrete round-trips correctly`
- `ParameterNormalizer: recommendMethod selects correctly`
- `DatasetNormalizer: fit computes correct stats`
- `DatasetNormalizer: transform/inverseTransform round-trips`
- `FeatureNormalizer: MFCC uses ZScore`
- `FeatureNormalizer: chroma L2 norm normalizes`
- `FeatureNormalizer: export/import preserves state`

**ValidationEngine Edge Cases:**
- `ValidationEngine: empty samples handled gracefully`
- `ValidationEngine: single sample coverage`
- `ValidationEngine: MMD with different sizes`

### Agent 3: Integration Agent (Wiring & Validation)

**Tasks:**

1. **Wire PhaseVocoder** (~30 lines in AudioAugmentation.h)
   - Initialize `phaseVocoder_` on first use
   - Map intensity to stretch ratio (0.8x to 1.2x) and semitones (-6 to +6)

2. **DatasetGeneratorV2 Config** (~20 lines)
   - Add `bool enableTimeStretch = true` and `bool enablePitchShift = true` to config
   - Pass to AudioAugmenter chain setup

3. **Stress Test Script** (~100 lines, new file `tests/stress_test_dataset.py`)
   ```python
   def test_10000_samples():
       config = DatasetGeneratorConfig()
       config.totalSamples = 10000
       config.enableAugmentation = True
       # Monitor memory, check for crashes, validate outputs
   ```

4. **Documentation Updates**
   - Update `SYNTHETIC_AUDIO_PARAMETER_DATASET_FRAMEWORK.md`
   - Mark TimeStretch/PitchShift as implemented
   - Update test coverage table

5. **Bug Fixes** (as discovered during integration)

## Success Criteria

| Criterion | Verification |
|-----------|--------------|
| All 6 deliverables implemented | Code review |
| Passing tests | `ctest --output-on-failure` |
| Documentation complete | Doc review |
| Sample dataset validates | QA scripts pass |
| 10,000+ samples without crash | Stress test |
| Memory under 4GB | ResourceMonitor log |

## File Changes Summary

| File | Action | Lines |
|------|--------|-------|
| `src/AI/Dataset/PhaseVocoder.h` | Create | ~300 |
| `src/AI/Dataset/AudioAugmentation.h` | Modify | ~50 |
| `src/AI/Dataset/DatasetGeneratorV2.h` | Modify | ~20 |
| `tests/Unit/TestDatasetModules.cpp` | Extend | ~320 |
| `tests/stress_test_dataset.py` | Create | ~100 |
| `SYNTHETIC_AUDIO_PARAMETER_DATASET_FRAMEWORK.md` | Update | ~30 |

**Total:** ~820 lines of new/modified code

## Dependencies

- JUCE DSP module (already linked)
- Catch2 v3 (already linked for tests)
- Python 3.8+ (for stress test script)

## Risks & Mitigations

| Risk | Mitigation |
|------|------------|
| Phase vocoder artifacts | Use established algorithm, test with multiple audio types |
| Threading issues | One PhaseVocoder per AudioAugmenter instance |
| Memory growth in V3 pipeline | ResourceMonitor throttling already implemented |
| Test flakiness | Use fixed seeds, deterministic assertions |
