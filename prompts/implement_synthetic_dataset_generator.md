# Ralph-Loop Prompt: Synthetic Audio Dataset Generator

## Command

```bash
ralph-loop "Implement Synthetic Audio Dataset Generator for MorphSnap.

## Overview
Implement a comprehensive synthetic audio dataset generation system that produces ground-truth DSP parameter datasets for machine learning training. This system extends the existing DatasetGenerator to support systematic parameter space exploration, diverse audio content processing, and rich metadata labeling.

## Requirements

### 1. Parameter Sampler Module
- Implement Latin Hypercube Sampling (LHS) for uniform high-dimensional parameter space coverage
- Implement stratified sampling by genre/content type with configurable strata percentages
- Implement physics-informed constraint validation (attack < release, EQ spacing rules, makeup gain limits)
- Support log-spaced, linear-spaced, and exponential-spaced parameter distributions
- Export parameter configuration to JSON with reproducible random seeds

### 2. Audio Content Management
- Create SourceAudioLibrary class for managing diverse audio content taxonomy
- Implement genre classification: Electronic (25%), Rock/Pop (20%), Jazz/Acoustic (15%), Hip-Hop/R&B (15%), Classical (5%), plus stems and test signals
- Implement content characteristic extraction: dynamic range, spectral centroid, rhythmic density, harmonic complexity
- Create augmentation pipeline: time stretch (±20%), pitch shift (±6 semitones), dynamic range manipulation
- Support 48kHz/32-bit float WAV/FLAC source files

### 3. Multi-Plugin Chain Engine
- Extend existing IPluginHostManager to support sequential plugin chains
- Implement chain configuration DSL (JSON/YAML format)
- Support chain types: EQ-Only, Dynamics-Only, Mastering (EQ→Comp→Limiter), Mixing, Creative
- Implement plugin parameter mapping and normalization (0-1 range with denormalization bounds)
- Add plugin settle time processing between parameter changes
- Support VST3 and AU plugin formats via JUCE

### 4. Enhanced Rendering Pipeline
- Implement multi-segment rendering: 30s full, 2s transient, 5s steady-state
- Support multiple output formats: 48kHz/32-bit float WAV, 24-bit FLAC
- Implement batch rendering with progress callbacks
- Add parallel rendering support for multiple parameter configurations
- Implement audio output validation (no silence detection, clipping detection)
- Target throughput: 10,000 samples/hour on standard hardware

### 5. Feature Extraction System
- Implement spectral features: MFCC (13 coefficients), spectral centroid, spectral rolloff, spectral flux, chroma features using Librosa-equivalent algorithms
- Implement temporal features: RMS energy, peak amplitude, crest factor, attack time, transient density
- Implement perceptual features: LUFS (ITU-R BS.1770-4), true peak, roughness (Daniel-Weber), sharpness (Aures), brightness
- Export features as NumPy-compatible arrays and JSON metadata
- Support both frame-level and global feature aggregation

### 6. Ground-Truth Metadata Schema
- Design comprehensive JSON schema including:
  - Source audio provenance (file path, genre, content type, original LUFS, dynamic range)
  - Processing chain details (plugin IDs, vendor, version, all parameter values raw and normalized)
  - Output audio characteristics (LUFS, true peak, dynamic range, spectral centroid)
  - Extracted feature vectors (spectral, temporal, perceptual)
  - ML targets (parameter regression vectors, style classification, processing intensity)
- Implement metadata validation against JSON schema
- Support Parquet format for efficient large-scale storage

### 7. Validation Framework
- Implement distribution matching tests: Kolmogorov-Smirnov for parameter distributions, Maximum Mean Discrepancy for feature distributions, Wasserstein distance for spectral distributions
- Implement coverage metrics: volume coverage ratio (>0.75), grid coverage (>0.80), boundary coverage (>15%)
- Create synthetic-to-real transfer evaluation: zero-shot testing protocol, fine-tuning benchmarks
- Generate validation reports with visualizations (histograms, scatter plots, coverage maps)

### 8. Dataset Organization
- Implement directory structure: audio/train|val|test/genre/, metadata/manifests.json, features/{spectral,temporal,perceptual}/, targets/{regression,classification}/
- Create stratified train/validation/test splitting (70/15/15) by genre and processing intensity
- Implement dataset integrity verification (all files present, metadata valid, no corruption)
- Support incremental dataset extension and deduplication

### 9. Integration with MorphSnap
- Extend existing DatasetGenerator class in src/AI/Dataset/
- Integrate with IPluginHostManager for plugin hosting
- Add MCP server endpoints for remote dataset generation control
- Implement progress reporting via JUCE ChangeBroadcaster
- Ensure thread-safe audio processing

### 10. Configuration and CLI
- Create YAML/JSON configuration format for generation runs
- Implement command-line interface with options for: source audio directory, output directory, sample count, chain configuration, feature extraction settings
- Add dry-run mode for validation without rendering
- Support resumable generation (checkpoint/restart capability)

## Technical Specifications
- Language: C++20 with JUCE framework
- Audio Processing: JUCE audio processors, 48kHz sample rate, 512-sample block size
- Dependencies: nlohmann/json, JUCE 7+, optional: Intel IPP for accelerated feature extraction
- Testing: Catch2 or Google Test framework
- Documentation: Doxygen comments, markdown guides

## Architecture Integration
- Location: src/AI/Dataset/ (extend existing DatasetGenerator)
- New files: ParameterSampler.h/cpp, AudioContentLibrary.h/cpp, FeatureExtractor.h/cpp, MetadataWriter.h/cpp, ValidationEngine.h/cpp
- Integration points: IPluginHostManager, MorphProcessor, MCP server

## Success Criteria
- All requirements implemented and tested
- Unit tests passing with >80% code coverage
- Integration tests rendering 1000+ samples successfully
- No compiler warnings or linter errors
- Documentation complete: API reference, user guide, architecture diagram
- Validation report showing >80% parameter space coverage and <15% synthetic-to-real performance gap
- Throughput benchmark: minimum 5000 samples/hour on reference hardware

Output <promise>COMPLETE</promise> when done." --max-iterations 30 --completion-promise "COMPLETE"
```

## Usage Instructions

1. Copy the command above into your terminal
2. Ensure ralph-loop is installed and configured
3. Run from the project root directory (c:/Users/HP/morphy)
4. The implementation will follow the methodology defined in `docs/architecture/SYNTHETIC_AUDIO_DATASET_METHODOLOGY.md`

## Expected Output

After successful completion, the following should be available:

```
src/AI/Dataset/
├── DatasetGenerator.h/cpp (extended)
├── ParameterSampler.h/cpp (new)
├── AudioContentLibrary.h/cpp (new)
├── FeatureExtractor.h/cpp (new)
├── MetadataWriter.h/cpp (new)
├── ValidationEngine.h/cpp (new)
└── tests/
    ├── test_parameter_sampler.cpp
    ├── test_feature_extractor.cpp
    └── test_integration.cpp

docs/
├── API_REFERENCE.md
├── USER_GUIDE.md
└── VALIDATION_REPORT.md
```
