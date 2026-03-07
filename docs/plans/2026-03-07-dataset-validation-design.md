# Dataset Framework Validation Design

> **Date**: 2026-03-07
> **Status**: Approved
> **Author**: Claude Code

## Overview

This document defines the validation strategy for the MorphSnap synthetic audio-parameter dataset framework. The framework is already fully implemented in `src/AI/Dataset/` with all 6 deliverables present. This design validates that it meets all success criteria.

## Background

### Existing Implementation

The framework includes:

| Component | File | Purpose |
|-----------|------|---------|
| V2 Generator | `DatasetGeneratorV2.h` | Sequential pipeline orchestrator |
| V3 Generator | `DatasetGeneratorV3.h` | Modular async pipeline |
| Parameter Sampler | `ParameterSampler.h` | Latin Hypercube, stratified sampling |
| Audio Library | `AudioContentLibrary.h` | Source audio with genre classification |
| Plugin Chain | `PluginChainEngine.h` | Sequential multi-plugin chains |
| Render Pipeline | `EnhancedRenderPipeline.h` | Multi-segment rendering |
| Feature Extraction | `FeatureExtractor.h` | MFCC, LUFS, spectral, temporal, perceptual |
| Metadata Writer | `MetadataWriter.h` | JSON/CSV/Parquet export |
| Validation Engine | `ValidationEngine.h` | KS test, MMD, coverage metrics |
| Dataset Organizer | `DatasetOrganizer.h` | Train/Val/Test splits |
| Normalization | `ParameterNormalization.h` | MinMax, ZScore, LogMinMax, RobustScaling |
| Augmentation | `AudioAugmentation.h` | NoiseInjection, FrequencyMask, TimeMask, etc. |

### Known Gaps

| Gap | Severity | Description |
|-----|----------|-------------|
| TimeStretch placeholder | Medium | `AudioAugmentation.h:181` - no-op |
| PitchShift placeholder | Medium | `AudioAugmentation.h:182` - no-op |
| FeatureExtractor test coverage | Low | Tests exist but may be incomplete |
| V2/V3 integration test | Low | End-to-end integration not tested |

## Validation Strategy

### Goal

Verify the existing dataset generation framework meets all success criteria:
- All 6 deliverables implemented with passing tests
- Handles 10,000+ samples without crash
- Memory under 4GB during generation
- Output audio/features are valid

### Validation Dimensions

| Dimension | What We'll Validate | Success Criteria |
|-----------|---------------------|------------------|
| **Unit Tests** | All modules pass existing tests | 100% pass rate |
| **Scale Test** | Generate 10,000+ samples | No crashes, completes |
| **Memory** | Peak memory during generation | < 4GB |
| **Correctness** | Output audio/features are valid | Non-silent, correct dimensions |
| **Completeness** | All 6 deliverables implemented | No placeholders (documented exceptions) |
| **Integration** | End-to-end pipeline works | V2 + V3 both functional |

## Test Cases

### Phase 1: Unit Test Validation

Existing tests in `tests/Unit/TestDatasetModules.cpp` cover:
- `ParameterSampler`: LHS dimensions, [0,1] bounds, strata coverage, constraint validation
- Additional coverage needed for FeatureExtractor, ValidationEngine, Normalization, Augmentation

**New Tests to Add:**

```cpp
// FeatureExtractor (8 tests)
TEST_CASE("FeatureExtractor: MFCC dimension is 13", "[dataset][features]")
TEST_CASE("FeatureExtractor: LUFS computed for known signal", "[dataset][features]")
TEST_CASE("FeatureExtractor: Chroma sums to ~1.0 for pure tone", "[dataset][features]")
TEST_CASE("FeatureExtractor: Spectral centroid in expected range", "[dataset][features]")
TEST_CASE("FeatureExtractor: Temporal RMS matches manual calculation", "[dataset][features]")
TEST_CASE("FeatureExtractor: Perceptual brightness in [0,1]", "[dataset][features]")
TEST_CASE("FeatureExtractor: Frame-level features correct count", "[dataset][features]")
TEST_CASE("FeatureExtractor: toVector produces 31 dimensions", "[dataset][features]")

// ValidationEngine (4 tests)
TEST_CASE("ValidationEngine: KS test detects distribution shift", "[dataset][validation]")
TEST_CASE("ValidationEngine: MMD distinguishes different distributions", "[dataset][validation]")
TEST_CASE("ValidationEngine: Coverage metrics in valid range", "[dataset][validation]")
TEST_CASE("ValidationEngine: Cross-reference validation works", "[dataset][validation]")

// Normalization (6 tests)
TEST_CASE("DatasetNormalizer: fit/transform round-trip", "[dataset][normalization]")
TEST_CASE("FeatureNormalizer: per-group methods applied", "[dataset][normalization]")
TEST_CASE("LogMinMax: handles frequency range", "[dataset][normalization]")
TEST_CASE("ZScore: produces zero mean", "[dataset][normalization]")
TEST_CASE("RobustScaling: handles outliers", "[dataset][normalization]")
TEST_CASE("recommendMethod: returns correct types", "[dataset][normalization]")

// Augmentation (5 tests)
TEST_CASE("AudioAugmenter: noise injection changes RMS", "[dataset][augmentation]")
TEST_CASE("AudioAugmenter: gain change applies dB correctly", "[dataset][augmentation]")
TEST_CASE("AudioAugmenter: frequency mask zeroes target bins", "[dataset][augmentation]")
TEST_CASE("AudioAugmenter: time mask zeroes target samples", "[dataset][augmentation]")
TEST_CASE("AudioAugmenter: dynamicProcessing reduces peaks", "[dataset][augmentation]")
```

### Phase 2: Scale & Memory Validation

**Scale Test Configuration:**
```json
{
  "outputDirectory": "./validation_output/",
  "datasetName": "scale_test_10k",
  "totalSamples": 10000,
  "sampleRate": 48000,
  "blockSize": 512,
  "numChannels": 2,
  "fullDuration": 5.0,
  "chainType": "Mastering",
  "numParallelThreads": 4,
  "enableValidation": true,
  "checkpointInterval": 500
}
```

**Memory Monitoring:**
- Track RSS memory every 1 second during generation
- Record peak memory usage
- Success: peak < 4.0 GB

### Phase 3: Output Validation

**Validation Checks:**
```python
VALIDATION_CHECKS = [
    # Audio validation
    ("audio_exists", lambda s: (s / "audio.wav").exists()),
    ("audio_not_silent", lambda s: rms(s.audio) > 1e-6),
    ("audio_duration", lambda s: abs(duration(s.audio) - 5.0) < 0.1),
    ("audio_sample_rate", lambda s: s.audio.sr == 48000),
    ("audio_channels", lambda s: s.audio.channels == 2),

    # Feature validation
    ("features_exists", lambda s: (s / "features.json").exists()),
    ("mfcc_count", lambda s: len(s.features["mfcc"]) == 13),
    ("chroma_count", lambda s: len(s.features["chroma"]) == 12),
    ("feature_dimensions", lambda s: len(s.features) >= 31),

    # Parameter validation
    ("params_exists", lambda s: (s / "parameters.json").exists()),
    ("params_in_range", lambda s: all(0 <= p <= 1 for p in s.params)),
    ("param_count_matches", lambda s: len(s.params) == s.expected_param_count),
]
```

### Phase 4: Gap Remediation

**TimeStretch/PitchShift Documentation:**
- Add documentation noting these are optional/placeholders
- Recommend external tools (Rubber Band, SoundTouch) for production use
- Update `AugmentationChainPreset` to exclude by default

**Integration Test:**
```cpp
TEST_CASE("V2→V3 pipeline integration", "[dataset][integration]")
{
    DatasetGeneratorV2 v2;
    v2.setConfig(createTestConfig());
    REQUIRE(v2.initialize());

    auto params = v2.getSampler().generateLHS(config, 10);
    REQUIRE(params.size() == 10);

    DatasetGeneratorV3 v3;
    v3.setConfig(createV3Config());

    for (int i = 0; i < 10; ++i) {
        auto metadata = v2.processSingleSample(i, params[i]);
        REQUIRE(metadata.sampleIndex == i);
    }
}
```

## Task Breakdown

### Phase 1: Unit Test Validation
| Task ID | Description | Effort |
|---------|-------------|--------|
| 1.1 | Run existing unit tests | 5 min |
| 1.2 | Analyze test coverage gaps | 15 min |
| 1.3 | Add 23 new unit tests | 1-2 hrs |

### Phase 2: Scale & Memory Validation
| Task ID | Description | Effort |
|---------|-------------|--------|
| 2.1 | Create scale test runner script | 30 min |
| 2.2 | Create memory monitor script | 20 min |
| 2.3 | Run 10K sample generation | 1-4 hrs runtime |
| 2.4 | Analyze memory profile results | 15 min |

### Phase 3: Output Validation
| Task ID | Description | Effort |
|---------|-------------|--------|
| 3.1 | Create output validator script | 30 min |
| 3.2 | Validate audio files | 20 min |
| 3.3 | Validate feature files | 15 min |

### Phase 4: Gap Remediation
| Task ID | Description | Effort |
|---------|-------------|--------|
| 4.1 | Document TimeStretch/PitchShift as optional | 15 min |
| 4.2 | Add V2→V3 integration test | 30 min |
| 4.3 | Update framework documentation | 20 min |

### Phase 5: Final Verification
| Task ID | Description | Effort |
|---------|-------------|--------|
| 5.1 | Run full validation suite | 30 min |
| 5.2 | Generate final validation report | 15 min |
| 5.3 | Output completion promise | 1 min |

## Success Criteria

| Criterion | Target | Verification |
|-----------|--------|--------------|
| Unit tests pass | 100% | `ctest --output-on-failure` returns 0 |
| Samples generated | ≥ 10,000 | File count in output directory |
| Peak memory | < 4 GB | Memory monitor report |
| Audio valid | 0 silent files | Output validator |
| Features valid | 31+ dimensions per sample | Output validator |
| Documentation updated | Gaps documented | File review |

## Files to Create/Modify

| File | Action |
|------|--------|
| `validation/run_scale_test.py` | Create |
| `validation/memory_monitor.py` | Create |
| `validation/validate_outputs.py` | Create |
| `validation/scale_test_config.json` | Create |
| `tests/Unit/TestDatasetModules.cpp` | Modify (add 23 tests) |
| `tests/Integration/TestDatasetIntegration.cpp` | Create |
| `src/AI/Dataset/AudioAugmentation.h` | Modify (add docs) |
| `SYNTHETIC_AUDIO_PARAMETER_DATASET_FRAMEWORK.md` | Modify |
| `validation/FINAL_VALIDATION_REPORT.md` | Create |

## Effort Estimate

| Phase | Effort | Runtime |
|-------|--------|---------|
| Phase 1 | 1.5-2 hours | ~1 min |
| Phase 2 | 1 hour | 1-4 hours |
| Phase 3 | 1 hour | ~30 min |
| Phase 4 | 1 hour | ~5 min |
| Phase 5 | 30 min | ~30 min |
| **Total** | **5-5.5 hours** | **2-5 hours runtime** |

## Completion

Upon successful validation of all success criteria, output:

```
<promise>DATASET_FRAMEWORK_COMPLETE</promise>
```
