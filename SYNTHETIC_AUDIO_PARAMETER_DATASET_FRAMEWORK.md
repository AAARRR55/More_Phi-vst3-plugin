# Synthetic Audio-Parameter Dataset Generation Framework

> **Version**: 3.3.0  
> **Last Updated**: March 2026

## Overview

This framework provides a comprehensive methodology for generating high-quality synthetic datasets that map continuous numerical parameters from VST plugins to complex high-dimensional audio array outputs. The framework is designed for machine learning applications including parameter estimation, audio style transfer, and plugin behavior modeling.

This framework is implemented in **MorphSnap v3.3.0**, a JUCE 8-based VST3/AU plugin. The C++ implementation is in [`src/AI/Dataset/`](src/AI/Dataset/), and Python dataset generators are available in the project root.

---

## Implementation Reference

This framework is fully implemented in MorphSnap v3.3.0. The following sections reference the actual C++ and Python implementations.

### C++ Implementation (src/AI/Dataset/)

| Component | File | Purpose |
|-----------|------|---------|
| V2 Generator | [`DatasetGeneratorV2.h`](src/AI/Dataset/DatasetGeneratorV2.h) | Sequential pipeline orchestrator |
| V3 Generator | [`DatasetGeneratorV3.h`](src/AI/Dataset/DatasetGeneratorV3.h) | Modular async pipeline (always compiled) |
| Parameter Sampler | [`ParameterSampler.h`](src/AI/Dataset/ParameterSampler.h) | Latin Hypercube, stratified sampling |
| Audio Library | [`AudioContentLibrary.h`](src/AI/Dataset/AudioContentLibrary.h) | Source audio with genre classification |
| Plugin Chain | [`PluginChainEngine.h`](src/AI/Dataset/PluginChainEngine.h) | Sequential multi-plugin chains |
| Render Pipeline | [`EnhancedRenderPipeline.h`](src/AI/Dataset/EnhancedRenderPipeline.h) | Multi-segment rendering |
| Feature Extraction | [`FeatureExtractor.h`](src/AI/Dataset/FeatureExtractor.h) | MFCC, LUFS, spectral, temporal, perceptual |
| Metadata Writer | [`MetadataWriter.h`](src/AI/Dataset/MetadataWriter.h) | JSON/CSV/Parquet export |
| Validation Engine | [`ValidationEngine.h`](src/AI/Dataset/ValidationEngine.h) | KS test, MMD, coverage metrics |
| Dataset Organizer | [`DatasetOrganizer.h`](src/AI/Dataset/DatasetOrganizer.h) | Train/Val/Test splits |
| Configuration | [`DatasetConfig.h`](src/AI/Dataset/DatasetConfig.h) | CLI interface, JSON schema |

### Python Dataset Generators

| Script | Purpose |
|--------|---------|
| [`auto_dataset_generator.py`](auto_dataset_generator.py) | Fully automated with crash-resistant retry |
| [`big_dataset_generator.py`](big_dataset_generator.py) | Large-scale generation (500+ samples) |
| [`generate_rendered_dataset.py`](generate_rendered_dataset.py) | Rendered audio through hosted plugin |
| [`create_all_datasets.py`](create_all_datasets.py) | Multi-batch dataset creation |
| [`dataset_crashproof.py`](dataset_crashproof.py) | Crash-safe generation |

### Configuration Schema

See [`DatasetConfig.h`](src/AI/Dataset/DatasetConfig.h) for the complete JSON schema including:
- Audio settings (sample rate, channels, duration)
- Plugin chain types (EQOnly, DynamicsOnly, Mastering, Mixing, Creative)
- Parameter space definitions
- Feature extraction configuration
- Train/Val/Test split settings

---

### Known Limitations

| Component | Status | Notes |
|-----------|--------|-------|
| TimeStretch augmentation | Optional/Placeholder | Requires phase vocoder; use external tools |
| PitchShift augmentation | Optional/Placeholder | Requires phase vocoder |

**Recommended Alternatives:**
- **Rubber Band Library** (GPL/commercial)
- **SoundTouch** (LGPL)
- **librubberband**: C wrapper for Rubber Band

These augmentations are included in `AugmentationChainPreset::createCreative()` but disabled by default.

---

## 1. Data Acquisition Methodology

### 1.1 Parameter Sweep Automation

The parameter sweep system automates the systematic exploration of plugin parameter spaces to generate diverse audio outputs.

#### 1.1.1 Sampling Strategies

| Strategy | Description | Use Case |
|----------|-------------|----------|
| **Latin Hypercube Sampling (LHS)** | Space-filling design ensuring uniform coverage across all parameter dimensions | General-purpose dataset generation |
| **Stratified Sampling** | Divides parameter space into strata based on genre/intensity categories | Genre-specific datasets |
| **Random Uniform Sampling** | Simple random selection within parameter bounds | Quick prototyping |
| **Grid Sampling** | Discrete grid across parameter space | Exhaustive coverage |
| **Sobol Sequence** | Quasi-random low-discrepancy sequence | High-dimensional parameter spaces |

#### 1.1.2 Parameter Space Definition

```cpp
struct ParameterSpace {
    std::vector<ParameterDefinition> parameters;
    SamplingStrategy strategy;
    size_t sampleCount;
    uint32_t randomSeed;
};

struct ParameterDefinition {
    std::string name;           // Parameter identifier
    ParameterType type;         // Continuous, Discrete, Binary, Frequency, Decibel
    float minValue;             // Minimum normalized value (0.0)
    float maxValue;             // Maximum normalized value (1.0)
    float defaultValue;         // Plugin default
    std::vector<float> steps;   // Valid discrete values (if Discrete)
    bool logarithmic;          // Logarithmic scale for frequency/Hz parameters
};
```

#### 1.1.3 Automation Pipeline

```
┌─────────────────┐    ┌──────────────────┐    ┌─────────────────┐
│ ParameterSampler│───▶│ PluginChainEngine│───▶│RenderPipeline   │
│ (LHS/Stratified)│    │ (Multi-plugin)   │    │ (Multi-segment) │
└─────────────────┘    └──────────────────┘    └─────────────────┘
         │                                               │
         ▼                                               ▼
┌─────────────────┐                            ┌─────────────────┐
│ Sample Queue    │                            │ Audio Buffer    │
│ (ParameterSets) │                            │ (Rendered Audio)│
└─────────────────┘                            └─────────────────┘
```

#### 1.1.4 Plugin Chain Configuration

Support for sequential plugin chains:

- **EQOnly**: Parametric EQ, graphic EQ, dynamic EQ
- **DynamicsOnly**: Compressor, limiter, gate, expander
- **Mastering**: Full mastering chain (EQ → Dynamics → Saturation → Limiter)
- **Mixing**: Mixing-focused plugins (EQ → Compressor → Sends)
- **Creative**: Creative effects (Reverb → Delay → Modulation)
- **Custom**: User-defined plugin chains

#### 1.1.5 Source Audio Management

```cpp
class AudioContentLibrary {
    std::map<GenreCategory, std::vector<AudioFile>> content;
    GenreClassifier classifier;
    
    // Stratified sampling across genres
    std::vector<AudioFile> sampleByGenre(const std::vector<std::pair<Genre, float>>& ratios);
};
```

Genre stratification example:
- Electronic: 25%
- Rock/Pop: 20%
- Jazz/Acoustic: 15%
- Hip-Hop/R&B: 15%
- Classical: 5%
- Stems: 15%
- Test Signals: 5%

---

## 2. Audio Feature Extraction

### 2.1 Multi-Dimensional Representation

Audio features provide a compact yet expressive representation of the rendered audio for machine learning tasks.

#### 2.1.1 Feature Categories

| Category | Features | Dimensionality | Use Case |
|----------|----------|----------------|----------|
| **Temporal** | RMS, Zero-crossing rate, Transient detection | 3-10 | Dynamics, transients |
| **Spectral** | Spectral centroid, flux, rolloff, flatness | 4-20 | Timbre, brightness |
| **MFCC** | Mel-frequency cepstral coefficients | 13-40 | Speaker/instrument ID |
| **Chroma** | Chromagram, pitch class profiles | 12-24 | Harmonic content |
| **Perceptual** | Loudness (LUFS), CREST factor, dynamic range | 3-10 | Perceived quality |
| **Wavelet** | Wavelet coefficients at multiple scales | 32-128 | Multi-resolution |

#### 2.1.2 Feature Extraction Configuration

```cpp
struct FeatureExtractionConfig {
    size_t frameSize = 2048;           // FFT window size
    size_t hopSize = 512;               // Overlap between frames
    bool computeMFCC = true;             // Enable MFCC
    bool computeChroma = true;           // Enable chroma features
    bool computePerceptual = true;       // Enable loudness/LUFS
    size_t mfccCoefficients = 13;       // Number of MFCCs
    bool deltaFeatures = true;           // Include velocity features
    bool deltaDeltaFeatures = true;      // Include acceleration features
};
```

#### 2.1.3 Feature Extraction Pipeline

```
┌─────────────┐    ┌─────────────┐    ┌─────────────┐    ┌─────────────┐
│ Audio Buffer│───▶│ Windowing  │───▶│ FFT/Mel     │───▶│ Feature     │
│ (Time Domain)│    │ (Hann/Hamming)│   │ Transform   │    │ Vectors     │
└─────────────┘    └─────────────┘    └─────────────┘    └─────────────┘
                          │                  │
                          ▼                  ▼
                   ┌─────────────┐    ┌─────────────┐
                   │ Pre-emphasis│    │ Spectral    │
                   │ (optional)  │    │ Features    │
                   └─────────────┘    └─────────────┘
```

#### 2.1.4 Statistical Aggregation

For variable-length audio, compute frame-level statistics:

```cpp
struct AggregatedFeatures {
    // Central tendency
    std::vector<float> mean;      // Mean of each feature
    std::vector<float> median;   // Median of each feature
    
    // Dispersion
    std::vector<float> std;       // Standard deviation
    std::vector<float> range;    // Max - Min
    
    // Shape
    std::vector<float> skewness;  // Asymmetry
    std::vector<float> kurtosis;  // Tail heaviness
    
    // Temporal evolution
    std::vector<float> deltaMean;  // Mean of first derivative
    std::vector<float> segStd[3];   // Std per segment (transient/steady/release)
};
```

---

## 3. Normalization Strategies

### 3.1 Parameter Value Scaling

Proper normalization ensures that parameters from different plugins and parameter types are on comparable scales.

#### 3.1.1 Parameter Type Classification

```cpp
enum class ParameterType {
    Continuous,    // Smooth interpolation (gain, frequency)
    Discrete,      // Must snap to valid steps
    Binary,        // On/Off toggles
    Frequency,     // Hz values (logarithmic)
    Decibel,       // dB values (logarithmic)
    Enumeration    // Named states
};
```

#### 3.1.2 Normalization Methods

| Parameter Type | Method | Formula | Example |
|----------------|--------|---------|---------|
| **Continuous** | Min-Max | `n = (v - min) / (max - min)` | Gain: -60dB to +12dB |
| **Frequency** | Log-MinMax | `n = log(v/min) / log(max/min)` | 20Hz to 20kHz |
| **Decibel** | Log-MinMax | `n = log(v/min) / log(max/min)` | -60dB to +12dB |
| **Discrete** | Ordinal | `n = idx / (count - 1)` | Preset selection |
| **Binary** | Boolean | `n = v ? 1.0 : 0.0` | Bypass switches |

#### 3.1.3 Plugin-Specific Normalization

```cpp
class ParameterNormalizer {
public:
    // Convert from normalized [0,1] to plugin value
    static float denormalize(const ParameterDefinition& param, float normalized);
    
    // Convert from plugin value to normalized [0,1]
    static float normalize(const ParameterDefinition& param, float raw);
    
    // Handle logarithmic parameters
    static float logarithmicScale(float value, float min, float max);
    
    // Handle discrete snapping
    static float snapToValid(const ParameterDefinition& param, float value);
};
```

#### 3.1.4 Audio Feature Normalization

| Feature Type | Normalization | Rationale |
|--------------|----------------|-----------|
| **MFCC** | Mean-variance (per dataset) | Zero mean, unit variance |
| **Chroma** | L2 normalization | Intensity-invariant |
| **Spectral** | Min-Max per feature | Bounded ranges |
| **Loudness** | LUFS scaling | Perceptual relevance |
| **Raw Audio** | RMS normalization | Consistent energy |

```python
# Example: Feature normalization
class AudioFeatureNormalizer:
    def __init__(self, method='standard'):
        self.method = method
        self.mean = None
        self.std = None
        self.min = None
        self.max = None
    
    def fit(self, features):
        """Compute normalization parameters from training data"""
        if self.method == 'standard':
            self.mean = np.mean(features, axis=0)
            self.std = np.std(features, axis=0) + 1e-8
        elif self.method == 'minmax':
            self.min = np.min(features, axis=0)
            self.max = np.max(features, axis=0)
    
    def transform(self, features):
        """Apply normalization"""
        if self.method == 'standard':
            return (features - self.mean) / self.std
        elif self.method == 'minmax':
            return (features - self.min) / (self.max - self.min + 1e-8)
    
    def inverse_transform(self, normalized):
        """Reverse normalization"""
        if self.method == 'standard':
            return normalized * self.std + self.mean
        elif self.method == 'minmax':
            return normalized * (self.max - self.min) + self.min
```

---

## 4. Augmentation Approaches

### 4.1 Dataset Diversity Expansion

Augmentation increases dataset diversity without additional plugin rendering, improving model generalization.

#### 4.1.1 Audio-Level Augmentations

| Augmentation | Description | Parameters | Intensity Control |
|--------------|-------------|-----------|-------------------|
| **Time Stretching** | Change duration without pitch | ±10-30% | Uniform sampling |
| **Pitch Shifting** | Shift pitch without duration | ±2-6 semitones | Uniform sampling |
| **Noise Injection** | Add Gaussian/prosynth noise | SNR: 10-40dB | Linear fade |
| **Frequency Masking** | Mask spectral regions | Bandwidth, location | Random selection |
| **Time Masking** | Mask temporal segments | Duration, position | Random selection |
| **Mixing** | Blend with other samples | Mix ratio 0.1-0.5 | Random |
| **Room Impulse Response** | Apply reverb/room char | IR library | Random selection |
| **Dynamic Processing** | Apply compression/limiting | Threshold, ratio | Random |

#### 4.1.2 Parameter-Level Augmentations

| Augmentation | Description | Implementation |
|--------------|-------------|----------------|
| **Parameter Noise** | Add small noise to parameters | `p' = p + N(0, 0.01)` |
| **Parameter Interpolation** | Create intermediate samples | Linear/cubic blend |
| **Parameter Extrapolation** | Edge case exploration | Slight overshoot |
| **Parameter Coupling** | Correlated parameter groups | Based on plugin design |

#### 4.1.3 Augmentation Pipeline

```python
class AudioAugmentation:
    def __init__(self, augmentation_config):
        self.augmentations = self._build_augmentation_pipeline(augmentation_config)
    
    def __call__(self, audio, sample_rate=48000):
        """
        Apply random augmentation pipeline
        Returns: augmented audio, augmentation_metadata
        """
        aug_audio = audio.copy()
        metadata = {'applied': []}
        
        for aug in self.augmentations:
            if random.random() < aug.probability:
                aug_audio = aug.apply(aug_audio, sample_rate)
                metadata['applied'].append(aug.name)
        
        return aug_audio, metadata
```

#### 4.1.4 SpecAugment for Audio

Adapted from speech recognition:

```python
def specaugment(mel_spectrogram, num_freq_masks=2, num_time_masks=2):
    """Apply frequency and time masking to spectrogram"""
    freq_mask = np.random.randint(0, F, size=(num_freq_masks, 2))
    time_mask = np.random.randint(0, T, size=(num_time_masks, 2))
    
    for f0, f1 in freq_mask:
        mel_spectrogram[:, f0:f1] = 0
    
    for t0, t1 in time_mask:
        mel_spectrogram[t0:t1, :] = 0
    
    return mel_spectrogram
```

#### 4.1.5 Chained Augmentations

```cpp
struct AugmentationChain {
    std::vector<Augmentation> steps;
    float probability;          // Probability of applying this chain
    bool preserve_parameters;   // Whether to adjust parameters for augmentation
    
    // Example chains:
    // 1. Clean: no augmentation (20%)
    // 2. Noisy: noise injection (20%)
    // 3. Room: IR reverb (20%)
    // 4. Processed: dynamic processing (20%)
    // 5. Combined: multiple augmentations (20%)
};
```

---

## 5. Validation Procedures

### 5.1 Output Quality Assurance

Validation ensures generated datasets meet quality standards for downstream machine learning tasks.

#### 5.1.1 Audio Quality Metrics

| Metric | Description | Threshold | Action |
|--------|-------------|-----------|-------|
| **SNR** | Signal-to-noise ratio | > 60 dB | Flag low SNR samples |
| **Clipping** | Sample values exceed 1.0 | 0 samples | Regenerate or trim |
| **DC Offset** | Mean value ≠ 0 | < 0.01 | Apply high-pass filter |
| **Silence Detection** | All-zero or near-zero | RMS > 0.001 | Flag for review |
| **Distortion** | THD measurement | < 1% | Check for clipping |
| **Phase Issues** | Excessive phase cancellation | Correlation > 0.95 | Review stereo |

#### 5.1.2 Parameter Validation

```cpp
struct ParameterValidation {
    bool validateParameterSet(const ParameterSet& params) {
        // Check all parameters within valid range
        for (auto& p : params) {
            if (!isValidRange(p)) return false;
        }
        
        // Check for parameter coherence
        if (!validateCoherence(params)) return false;
        
        // Check for impossible combinations
        if (!validateCompatibility(params)) return false;
        
        return true;
    }
    
private:
    bool isValidRange(const Parameter& p);
    bool validateCoherence(const ParameterSet& params);
    bool validateCompatibility(const ParameterSet& params);
};
```

#### 5.1.3 Feature Consistency Checks

```python
class FeatureValidator:
    def validate_features(self, features, audio_metadata):
        """Validate extracted features"""
        issues = []
        
        # Check for NaN/Inf
        if np.any(np.isnan(features)):
            issues.append("NaN values in features")
        if np.any(np.isinf(features)):
            issues.append("Inf values in features")
        
        # Check dimensionality consistency
        expected_dim = self.config.expected_feature_dim
        if features.shape[-1] != expected_dim:
            issues.append(f"Dimension mismatch: {features.shape[-1]} vs {expected_dim}")
        
        # Check for feature drift
        if self._detect_drift(features, audio_metadata):
            issues.append("Feature drift detected")
        
        # Check loudness consistency
        if not self._validate_loudness(features, audio_metadata):
            issues.append("Loudness mismatch with metadata")
        
        return ValidationResult(passed=len(issues)==0, issues=issues)
```

#### 5.1.4 Dataset-Level Validation

| Check | Description | Severity |
|-------|-------------|----------|
| **Class Balance** | Parameter distribution is uniform | Warning |
| **Coverage** | All parameter ranges sampled | Error |
| **Duplicates** | No duplicate parameter/audio pairs | Warning |
| **Corruption** | No corrupted audio files | Error |
| **Metadata** | All entries have complete metadata | Error |
| **Split Validity** | Train/val/test splits are valid | Error |

#### 5.1.5 Automated Sanity Checks

```python
def sanity_check_dataset(dataset_path):
    """Run comprehensive sanity checks"""
    checks = []
    
    # 1. File integrity
    checks.append(check_audio_files_integrity(dataset_path))
    
    # 2. Parameter ranges
    checks.append(check_parameter_ranges(dataset_path))
    
    # 3. Audio quality
    checks.append(check_audio_quality(dataset_path))
    
    # 4. Feature validity
    checks.append(check_features_validity(dataset_path))
    
    # 5. Metadata completeness
    checks.append(check_metadata_completeness(dataset_path))
    
    # 6. Statistical properties
    checks.append(check_statistical_properties(dataset_path))
    
    # Generate report
    return generate_validation_report(checks)
```

---

## 6. Storage Formats

### 6.1 Efficient Large-Scale Data Handling

Efficient storage is critical for handling large-scale audio-parameter datasets.

#### 6.1.1 Recommended Formats

| Format | Use Case | Pros | Cons |
|--------|----------|------|------|
| **WAV (Float32)** | Raw audio storage | Lossless, widely supported | Large file size |
| **FLAC** | Compressed audio | 50% smaller than WAV, lossless | Slower random access |
| **HDF5** | Large arrays, metadata | Efficient binary, built-in compression | Complex tooling |
| **Parquet** | Tabular data, features | Columnar, excellent compression | Not for audio |
| **TensorFlow Record** | ML pipelines | Fast loading, sharding support | TensorFlow-only |
| **WebDataset** | Large-scale training | Sharded, streaming-friendly | Requires special loader |

#### 6.1.2 Hybrid Storage Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Dataset Storage Architecture              │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  /dataset_root/                                             │
│  ├── train/                                                │
│  │   ├── audio/           # Sharded audio files           │
│  │   │   ├── shard_0001.tar                                  │
│  │   │   ├── shard_0002.tar                                  │
│  │   │   └── ...                                            │
│  │   ├── features/        # Pre-extracted features (HDF5)  │
│  │   │   ├── train_features.h5                              │
│  │   │   └── train_features.h5.idx                         │
│  │   └── metadata/        # Parameter mappings              │
│  │       ├── metadata.parquet                               │
│  │       └── index.json                                     │
│  ├── val/                                                  │
│  │   └── (same structure)                                  │
│  └── test/                                                 │
│      └── (same structure)                                  │
│                                                             │
│  ├── dataset_config.json    # Full configuration           │
│  ├── normalization_params.json  # Saved normalizers        │
│  └── dataset_manifest.json  # File listing, checksums      │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

#### 6.1.3 HDF5 Structure for Features

```python
import h5py

def create_feature_dataset(output_path, samples):
    """Create HDF5 dataset with audio features"""
    with h5py.File(output_path, 'w') as f:
        # Create datasets with extendable size
        f.create_dataset('features', 
                        data=features,
                        maxshape=(None, feature_dim),
                        compression='gzip',
                        compression_opts=4)
        
        f.create_dataset('parameters',
                        data=parameters,
                        maxshape=(None, num_params),
                        compression='gzip',
                        compression_opts=4)
        
        f.create_dataset('audio_hash',
                        data=[h.encode() for h in audio_hashes],
                        maxshape=(None,))
        
        # Metadata as attributes
        f.attrs['sample_rate'] = 48000
        f.attrs['feature_dim'] = feature_dim
        f.attrs['num_params'] = num_params
        f.attrs['created'] = datetime.now().isoformat()
        f.attrs['feature_names'] = json.dumps(feature_names)
```

#### 6.1.4 Parquet Schema for Metadata

```python
import pyarrow.parquet as pq
import pyarrow as pa

# Define schema
schema = pa.schema([
    ('sample_id', pa.string()),
    ('audio_path', pa.string()),
    ('parameters', pa.list_(pa.float32())),
    ('parameter_names', pa.list_(pa.string())),
    ('genre', pa.string()),
    ('intensity', pa.float32()),
    ('duration', pa.float32()),
    ('sample_rate', pa.int32()),
    ('loudness_lufs', pa.float32()),
    ('features_mean', pa.list_(pa.float32())),
    ('features_std', pa.list_(pa.float32())),
    ('augmentation', pa.string()),
    ('split', pa.string()),  # train/val/test
])

# Write metadata
table = pa.Table.from_pandas(df, schema=schema)
pq.write_table(table, 'metadata.parquet', compression='snappy')
```

#### 6.1.5 WebDataset for Large-Scale Training

```python
import webdataset as wds

# Create sharded dataset
with wds.TarWriter('dataset/train/shard_%04d.tar', maxsize=1e9) as sink:
    for sample in dataset:
        sample_dict = {
            '__key__': sample['id'],
            'audio.wav': sample['audio_bytes'],
            'parameters.json': json.dumps(sample['params']),
            'features.npy': sample['features'].tobytes(),
            'metadata.json': json.dumps(sample['metadata']),
        }
        sink.write(sample_dict)

# Loading in training
dataset = wds.WebDataset('dataset/train/shard_{0000..0999}.tar')
            .decode(wds.audio)
            .to_tuple('audio.wav', 'parameters.json', 'features.npy')
            .map(MyPreprocessor)
```

#### 6.1.6 Storage Format Comparison

| Dataset Size | Recommended Format | Rationale |
|--------------|-------------------|-----------|
| < 10 GB | HDF5 + WAV | Simple, good performance |
| 10-100 GB | WebDataset + FLAC | Sharded, parallel loading |
| 100+ GB | WebDataset + MP3/AAC | Maximum compression |
| Streaming | TensorFlow Record | Efficient pipeline |

#### 6.1.7 Indexing and Fast Access

```python
class DatasetIndex:
    """Fast random access to dataset samples"""
    
    def __init__(self, dataset_path):
        self.dataset_path = dataset_path
        self.metadata = pq.read_table(f'{dataset_path}/metadata.parquet').to_pandas()
        self.metadata.set_index('sample_id', inplace=True)
        
        # Create parameter kd-tree for fast nearest-neighbor search
        self.param_tree = KDTree(self.metadata['parameters'].tolist())
    
    def get_sample(self, sample_id):
        """Retrieve single sample by ID"""
        meta = self.metadata.loc[sample_id]
        audio = load_audio(meta['audio_path'])
        features = load_features(meta['features_path'])
        return audio, meta['parameters'], features
    
    def find_nearest_params(self, target_params, k=10):
        """Find k nearest parameter sets"""
        distances, indices = self.param_tree.query([target_params], k=k)
        return [self.metadata.iloc[i] for i in indices[0]]
    
    def get_by_split(self, split='train'):
        """Get all samples from a split"""
        return self.metadata[self.metadata['split'] == split]
```

---

## 7. Implementation Architecture

### 7.1 Complete Pipeline

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    Synthetic Audio-Parameter Dataset Pipeline               │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐                │
│  │ Configuration│───▶│  Parameter    │───▶│    Source     │                │
│  │   (JSON)     │    │   Sampler     │    │   Audio       │                │
│  └──────────────┘    └──────────────┘    └──────────────┘                │
│         │                   │                   │                           │
│         ▼                   ▼                   ▼                           │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐                │
│  │ Plugin Chain │    │  Rendering   │    │  Augment      │                │
│  │   Engine     │◀───│  Pipeline    │◀───│  (Optional)   │                │
│  └──────────────┘    └──────────────┘    └──────────────┘                │
│         │                   │                   │                           │
│         ▼                   ▼                   ▼                           │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐                │
│  │   Feature    │◀───│    Audio      │    │   Feature    │                │
│  │  Extraction  │    │   Output     │    │   Norm.       │                │
│  └──────────────┘    └──────────────┘    └──────────────┘                │
│         │                                       │                           │
│         ▼                                       ▼                           │
│  ┌──────────────┐                        ┌──────────────┐                │
│  │  Validation  │─────────────────────▶│    Storage   │                │
│  │   Checks     │                        │   (HDF5/WS)  │                │
│  └──────────────┘                        └──────────────┘                │
│         │                                       │                           │
│         ▼                                       ▼                           │
│  ┌──────────────┐                        ┌──────────────┐                │
│  │  Validation  │                        │   ML Model   │                │
│  │   Report      │                        │   Training   │                │
│  └──────────────┘                        └──────────────┘                │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 7.2 Configuration Schema

```json
{
  "datasetName": "synthetic_audio_params_v1",
  "totalSamples": 10000,
  "randomSeed": 42,
  
  "audioSettings": {
    "sampleRate": 48000,
    "blockSize": 512,
    "numChannels": 2,
    "duration": 5.0,
    "outputFormat": "WAV32Float"
  },
  
  "pluginChain": {
    "type": "Mastering",
    "plugins": [
      {"name": "ParametricEQ", "instance": 1},
      {"name": "Compressor", "instance": 1},
      {"name": "Limiter", "instance": 1}
    ]
  },
  
  "parameterSpace": {
    "samplingStrategy": "LatinHypercube",
    "sampleCount": 10000,
    "parameters": [
      {"name": "eq_gain_1", "type": "Continuous", "min": -12, "max": 12},
      {"name": "eq_freq_1", "type": "Frequency", "min": 20, "max": 20000, "logarithmic": true},
      {"name": "comp_threshold", "type": "Decibel", "min": -60, "max": 0},
      {"name": "comp_ratio", "type": "Continuous", "min": 1.0, "max": 20.0}
    ]
  },
  
  "sourceAudio": {
    "directory": "./source_audio",
    "genreStratification": {
      "Electronic": 0.25,
      "RockPop": 0.20,
      "JazzAcoustic": 0.15
    }
  },
  
  "featureExtraction": {
    "frameSize": 2048,
    "hopSize": 512,
    "computeMFCC": true,
    "mfccCoefficients": 13,
    "computeChroma": true,
    "computePerceptual": true
  },
  
  "normalization": {
    "parameterMethod": "MinMax",
    "featureMethod": "Standard",
    "saveParams": true
  },
  
  "augmentation": {
    "enabled": true,
    "augmentations": [
      {"type": "NoiseInjection", "probability": 0.3, "snrDb": 30},
      {"type": "RoomReverb", "probability": 0.2},
      {"type": "DynamicProcessing", "probability": 0.2}
    ]
  },
  
  "validation": {
    "enableQualityChecks": true,
    "minSnrDb": 60,
    "checkClipping": true,
    "checkSilence": true
  },
  
  "storage": {
    "format": "WebDataset",
    "compression": "gzip",
    "shardSize": 1073741824,
    "trainRatio": 0.7,
    "valRatio": 0.15,
    "testRatio": 0.15
  }
}
```

---

## 8. Best Practices and Recommendations

### 8.1 Quality Assurance

1. **Always validate** generated audio for clipping and distortion
2. **Monitor parameter coverage** to ensure all ranges are sampled
3. **Check feature distributions** for anomalies
4. **Use consistent normalization** parameters across train/val/test splits

### 8.2 Performance Optimization

1. **Batch rendering** with multiple parallel plugin instances
2. **Pre-extract features** during generation to avoid reprocessing
3. **Use sharded formats** for large-scale training
4. **Cache normalized parameters** for quick iteration

### 8.3 Dataset Documentation

Maintain a dataset card documenting:

- Dataset purpose and intended use
- Data collection methodology
- Preprocessing steps
- Statistical properties
- Known limitations or biases
- Suggested evaluation metrics

---

## 9. References

- **MorphSnap**: JUCE 8-based VST3/AU plugin (https://github.com/morphsynth/morphsnap)
- **Librosa**: Python library for audio feature extraction (https://librosa.org)
- **WebDataset**: Large-scale data loading (https://webdataset.github.io/webdataset/)
- **HDF5**: Hierarchical Data Format (https://www.hdfgroup.org)
- **AudioLM**: Audio generation from audio prompts (Google Research)
- **Jukebox**: Generative music model (OpenAI)

---

## Appendix: Quick Reference

### Sample Count Estimation

For N parameters with k samples each:
- Grid sampling: k^N samples (exponential blowup)
- LHS: N × 100 samples minimum
- Recommended: 100-1000 samples per parameter dimension

### Feature Dimensionality

| Representation | Dimensions | Use Case |
|----------------|------------|----------|
| Raw audio (5s @ 48kHz) | 480,000 | End-to-end models |
| Mel spectrogram | 256 × 188 | CNN-based models |
| MFCC | 13-40 | Compact representation |
| Aggregated features | 50-200 | Parameter regression |

### Storage Estimates

| Dataset Size | Audio Format | Features | Total |
|--------------|--------------|----------|-------|
| 1,000 samples | WAV (50 MB) | HDF5 (5 MB) | 55 MB |
| 10,000 samples | WAV (500 MB) | HDF5 (50 MB) | 550 MB |
| 100,000 samples | WebDataset (5 GB) | HDF5 (500 MB) | 5.5 GB |
# Synthetic Audio-Parameter Dataset Framework

## Purpose

This framework defines how to build synthetic datasets that map continuous VST plugin parameter vectors to high-dimensional audio outputs in a reproducible, ML-ready form.

The core dataset target is:

`y = f(x, p, c)`

Where:
- `x` is the input audio stimulus
- `p` is the continuous plugin parameter vector
- `c` is execution context such as sample rate, block size, plugin version, oversampling mode, and render seed
- `y` is the output audio representation, stored as one or more arrays such as waveform, spectrogram, frame-wise features, and global descriptors

This document is narrower than the general dataset notes in `docs/DATASET_GENERATION.md`. It focuses specifically on continuous parameter-to-audio-array supervision.

## 1. Dataset Design Principles

The dataset should satisfy six constraints:

1. Parameter values must be known exactly and recoverable from metadata.
2. Audio renders must be deterministic enough to support regression and reproducibility checks.
3. The parameter space must be sampled with both global coverage and local resolution.
4. Audio outputs must be stored in multiple representations so the same corpus supports waveform, spectrogram, and feature-based models.
5. Augmentation must expand diversity without corrupting the relationship between parameter values and rendered audio.
6. Storage must scale from local experimentation to large sharded training corpora.

## 2. Canonical Sample Definition

Each dataset sample should contain:

- `sample_id`: stable unique identifier
- `plugin_id`, `plugin_name`, `plugin_version`, `plugin_format`
- `source_id`: input audio identifier
- `render_context`: sample rate, block size, offline/live mode, channel count, oversampling, render seed
- `parameter_vector_host`: host-normalized vector in `[0, 1]^d`
- `parameter_vector_canonical`: transformed vector after unit-aware normalization
- `parameter_vector_raw`: plugin-native plain values when available
- `parameter_schema_ref`: reference to a static schema describing units, ranges, transforms, and parameter semantics
- `audio_arrays`: paths or chunk references for waveform and derived tensors
- `feature_summary`: flat global feature vector
- `validation_flags`: silence, clipping, NaN, duplicate, failed round-trip, nondeterministic render, outlier

Recommended tensor views per sample:

| View | Shape | Purpose |
|------|-------|---------|
| Waveform | `[channels, samples]` | End-to-end waveform models |
| STFT magnitude | `[channels, freq_bins, frames]` | Spectral regression or diffusion models |
| Log-mel | `[channels, n_mels, frames]` | Compact perceptual representation |
| Frame feature sequence | `[frames, f]` | Sequence models over descriptors |
| Global feature vector | `[f_global]` | Fast baselines, QC, retrieval |

## 3. Parameter Schema and Scaling

Before rendering any data, build a parameter schema for each plugin. The schema is more important than the render loop because it determines how parameter values are interpreted and normalized.

Each parameter entry should include:

- `index`
- `name`
- `unit`
- `host_min = 0`, `host_max = 1`
- `plain_min`, `plain_max` if exposed
- `default_value`
- `is_automatable`
- `is_discrete`
- `step_count` if stepped
- `transform_type`
- `zero_point` for bipolar controls
- `display_examples`

Recommended `transform_type` values:

| Parameter family | Canonical transform |
|------------------|--------------------|
| Linear mix, depth, width | Linear to `[0, 1]` |
| Frequency | Log scaling |
| Time constants | Log scaling |
| Gain in dB | Piecewise linear around `0 dB` or signed normalized |
| Ratio or Q | Log scaling |
| Bipolar pan or tilt | Linear to `[-1, 1]` |
| Cyclic phase | Encode as `sin` and `cos` |
| Boolean or stepped | Keep separate from continuous regression target |

Recommended normalization formulas:

Linear:

```text
x_norm = (x - x_min) / (x_max - x_min)
```

Log:

```text
x_norm = (ln x - ln x_min) / (ln x_max - ln x_min)
```

Bipolar:

```text
x_norm = 2 * (x - x_min) / (x_max - x_min) - 1
```

Important rule: store both `parameter_vector_host` and `parameter_vector_canonical`. Host values preserve exact plugin automation values. Canonical values make cross-plugin learning and downstream normalization consistent.

## 4. Data Acquisition via Parameter Sweep Automation

### 4.1 Automation Goals

The acquisition loop should capture four behaviors:

- Univariate sensitivity: how each parameter changes audio when moved alone
- Pairwise interaction: how coupled parameters behave together
- Global manifold coverage: broad exploration of the valid parameter volume
- Local neighborhood smoothness: how small parameter perturbations alter the output

### 4.2 Recommended Sweep Schedule

Use a mixed schedule rather than a single strategy:

| Sweep type | Role | Typical count |
|------------|------|---------------|
| Baseline/default | Reference anchor | 1 per source clip |
| One-at-a-time linear or log sweep | Parameter sensitivity | 16 to 64 points per continuous parameter |
| Pairwise grid sweep | Interaction discovery | 8x8 or 12x12 for selected parameter pairs |
| Latin hypercube or Sobol sampling | High-dimensional global coverage | 1k to 100k samples depending on scale |
| Local jitter around anchor presets | Smoothness and local interpolation | 8 to 32 neighbors per anchor |
| Boundary oversampling | Edge-case robustness | 5 to 15 percent of total samples |

### 4.3 Automation Procedure

1. Load the plugin in an offline-capable host.
2. Enumerate all parameters and build the schema.
3. Exclude non-automatable, purely textual, and obviously discrete controls from the continuous target vector.
4. For each source clip, reset the plugin state and render a default reference pass.
5. Apply sweep plans using exact host-normalized values.
6. After each parameter assignment, allow warm-up or settle blocks before recording output.
7. Render a fixed-length clip or multiple segments from the processed output.
8. Read back the parameter values if the host supports round-trip verification.
9. Extract derived features and write metadata immediately.
10. Hash the render and QC outputs before marking the sample complete.

### 4.4 Practical Automation Rules

- Prefer offline rendering over realtime capture for determinism.
- Fix sample rate, block size, oversampling, and plugin quality mode per run.
- Warm up stateful effects. Compression, modulation, reverb, and lookahead processors need pre-roll.
- Reinitialize random or analog-mode plugins with fixed seeds when possible.
- For plugins with hidden dependencies, record the full preset or state blob in addition to individual parameters.
- For stepped controls that masquerade as continuous host values, store them separately and do not train them as ordinary regression dimensions.
- Render multiple input classes for the same parameter vector. A compressor behavior learned only on drum loops will not generalize to sustained pads or vocals.

### 4.5 Recommended Source Stimuli

Each sweep plan should run across a controlled stimulus library:

- Musical full mixes
- Isolated stems: drums, bass, vocals, guitar, synth, pads
- Test signals: sine sweeps, impulses, pink noise, white noise, tone bursts, stepped sines
- Dynamics probes: transient-heavy loops, sustained tones, sparse material

This turns the dataset into a mapping from `(input, parameters)` to output rather than a brittle mapping from parameters alone.

### 4.6 Automation Pseudocode

```python
for plugin in plugins:
    schema = inspect_parameters(plugin)
    sweep_plan = build_sweep_plan(schema, anchors, lhs_count, pair_rules)

    for source_clip in source_library:
        for assignment in sweep_plan:
            reset_plugin(plugin)
            apply_parameters(plugin, assignment.host_values)
            preroll(plugin, source_clip, settle_blocks=32)

            audio = render_offline(
                plugin=plugin,
                input_audio=source_clip,
                duration_seconds=clip_duration,
                sample_rate=48000,
                block_size=512,
            )

            round_trip = read_back_parameters(plugin)
            arrays = derive_audio_arrays(audio)
            features = extract_features(audio)
            qc = run_quality_checks(audio, assignment, round_trip)
            write_sample(plugin, schema, source_clip, assignment, arrays, features, qc)
```

## 5. Audio Output Representation

A single output representation is usually not enough. Use a multi-view representation so the same corpus supports different model families and QA workflows.

### 5.1 Primary Array Views

1. Raw waveform
2. Complex STFT or magnitude plus phase
3. Log-mel spectrogram
4. Frame-wise feature sequence
5. Global summary features

### 5.2 Feature Extraction Layers

Use three layers of feature extraction:

| Layer | Examples | Use |
|------|----------|-----|
| Low-level spectral | STFT bins, mel bins, MFCC, chroma, centroid, rolloff, flatness, flux | Captures timbre and resonant movement |
| Temporal and dynamics | RMS, peak, crest factor, attack time, zero-crossing rate, transient density, envelope features | Captures dynamics processing behavior |
| Perceptual and high-level | LUFS, true peak, dynamic range, brightness, roughness, sharpness, stereo width, learned embeddings | Captures listener-relevant changes |

The existing MorphSnap `FeatureExtractor` already supports a practical global feature layer with 31 scalars plus frame-level sequences:

- Spectral: MFCC, centroid, rolloff, flux, spread, flatness, chroma
- Temporal: RMS, peak, crest factor, attack time, transient density, zero-crossing rate
- Perceptual: LUFS, true peak, roughness, sharpness, brightness, dynamic range

### 5.3 Representation Strategy by Model Type

| Model objective | Preferred target/input storage |
|-----------------|-------------------------------|
| Direct waveform modeling | Waveform plus loudness-normalized variant |
| Parameter regression from audio | Log-mel, global features, frame features |
| Invertible or conditional synthesis | Waveform, STFT magnitude, phase proxy |
| Fast QC and retrieval | Global feature vector and thumbnail spectrogram |

## 6. Normalization Strategy

Normalization should be separated into three layers: parameter normalization, audio normalization, and feature normalization.

### 6.1 Parameter Normalization

Store three versions of each control when possible:

- `host_normalized`: exact host automation value
- `raw_plain`: plugin-native value in displayed units
- `canonical_normalized`: transform-aware normalized value used by ML code

Recommended practice:

- Use `host_normalized` for exact reproducibility.
- Use `canonical_normalized` for learning targets.
- Preserve `raw_plain` for auditing, charts, and inverse transforms.

Do not mix incompatible units into a single untyped vector without schema information.

### 6.2 Audio Normalization

Normalize source and output audio consistently:

- Resample all material to a fixed rate such as `48 kHz`
- Fix channel topology, usually stereo
- Trim or crop to a fixed duration per sample
- Remove DC offset before feature extraction
- Store a raw render and an optional loudness-normalized analysis copy

Important distinction:

- The raw render should preserve the plugin's true gain behavior.
- The analysis copy may be LUFS-normalized for feature stability and model training.

### 6.3 Feature Normalization

Apply feature normalization after extraction, not during rendering:

- Standardize dense continuous features with dataset mean and standard deviation
- Use robust scaling for heavy-tailed features such as crest factor or true peak outliers
- Log-compress power-domain features before z-scoring
- Save normalization statistics per split version and never recompute them using validation or test data

Recommended stored statistics:

- `mean`
- `std`
- `median`
- `iqr`
- `min`
- `max`
- `fit_sample_count`
- `schema_version`

## 7. Augmentation Strategy

Augmentation is useful only if label integrity is preserved or explicitly modeled.

### 7.1 Safe Augmentation Categories

| Category | Apply to | Label impact |
|----------|----------|--------------|
| Source-content augmentation | Input stimulus before plugin processing | Safe if source identity is tracked |
| Parameter-space augmentation | Generate new renders from perturbed parameter vectors | Safe and preferred |
| Post-render robustness augmentation | Output audio after rendering | Use only in separate robustness splits |

### 7.2 Recommended Augmentations

Source-side:

- Time stretch
- Pitch shift
- Gain staging variation
- Mid-side balance variation
- Transient shaping of the source
- Stem remixing and source recombination

Parameter-side:

- Small Gaussian or Sobol jitter around anchor vectors
- Interpolation between valid presets
- Boundary oversampling near min, max, and neutral positions
- Hard-example mining from regions with steep output gradients

Post-render, optional and separated:

- Codec simulation
- Background noise
- Sample-rate conversion artifacts
- Mild room coloration

If the goal is exact inversion of plugin parameters, do not mix post-render degradations into the main supervised split. Put them in an auxiliary robustness set with explicit tags.

### 7.3 Diversity Expansion Without Leakage

- Split by source family, not just by rendered clip
- Keep augmentation descendants of the same raw clip in the same dataset split
- Keep all renders from the same preset anchor together when evaluating generalization
- Optionally hold out entire plugin versions or source categories for transfer tests

## 8. Validation and Quality Control

Validation should occur at parameter, audio, feature, and dataset levels.

### 8.1 Per-Sample Validation

Check every render for:

- No NaN or Inf samples
- No complete silence unless intentionally tagged
- No hard clipping beyond threshold
- Acceptable DC offset
- Expected duration and channel count
- Valid parameter round-trip from host to plugin state
- Successful feature extraction
- Stable file hash and metadata completeness

### 8.2 Sweep Consistency Validation

For one-at-a-time sweeps:

- Verify monotonic controls behave plausibly where expected
- Detect flat parameters that do not audibly change output
- Detect hidden quantization in supposedly continuous controls
- Detect hysteresis or state retention across resets

For local neighborhood sweeps:

- Measure output sensitivity to small parameter perturbations
- Flag discontinuities caused by unstable plugin states or hidden mode switches

### 8.3 Distribution and Coverage Validation

Recommended coverage checks:

- Marginal histograms per parameter
- Pairwise scatter coverage for selected interactions
- Boundary coverage ratio
- Low-discrepancy metrics for global samplers
- Feature-space coverage using PCA or UMAP for visualization only
- Duplicate and near-duplicate detection using audio hashes or feature distance

### 8.4 Reproducibility Validation

Re-render a random subset, typically `1 to 5 percent`, under the same environment and compare:

- Parameter vectors
- Audio hash or sample-wise distance
- Feature deltas
- Loudness and peak deviations

If a plugin is intentionally nondeterministic, tag it and store repeated renders as stochastic samples rather than treating them as failures.

### 8.5 Human Listening Validation

Automated metrics are not sufficient for creative audio tools. Add spot checks:

- Random batch listening after each generation run
- Focused review of boundary and outlier samples
- AB comparisons of repeated renders
- Review of silent, clipped, or highly distorted outliers before deletion

## 9. Recommended Storage Formats

No single format is ideal for every component. Use a hybrid layout.

### 9.1 Format Recommendations by Data Type

| Data type | Recommended format | Why |
|-----------|--------------------|-----|
| Raw or analysis audio | FLAC for integer audio, WAV for float-critical renders | Lossless and broadly supported |
| Metadata tables | Parquet | Columnar, compact, fast filtering |
| Small manifests and debugging | JSONL | Human-readable and streamable |
| Large dense feature tensors | Zarr | Chunked, parallel-friendly, cloud-ready |
| Local monolithic experiments | HDF5 | Good local random access, weaker concurrency |
| Training shards | WebDataset tar shards or tarred FLAC plus Parquet index | Efficient streaming in distributed training |

### 9.2 Preferred Large-Scale Layout

```text
dataset_root/
  schemas/
    parameter_schema.json
    dataset_schema.json
  manifests/
    train.parquet
    val.parquet
    test.parquet
    qc_summary.json
  audio_shards/
    train-000000.tar
    train-000001.tar
    val-000000.tar
    test-000000.tar
  features/
    global_features.zarr
    frame_features.zarr
    logmel.zarr
  stats/
    parameter_normalization.json
    feature_normalization.json
  previews/
    spectrogram_thumbnails/
```

### 9.3 Metadata Columns to Preserve

At minimum:

- `sample_id`
- `plugin_id`
- `plugin_version`
- `source_id`
- `split`
- `audio_uri`
- `feature_uri`
- `parameter_vector_host`
- `parameter_vector_canonical`
- `parameter_vector_raw`
- `render_seed`
- `sample_rate`
- `duration_seconds`
- `validation_flags`
- `hash_audio`
- `hash_metadata`

For Parquet, keep scalar columns flat and move large vectors to sidecar chunk stores if row size becomes excessive.

## 10. Reference Generation Pipeline

Recommended end-to-end pipeline:

1. Curate source library and assign source-family IDs.
2. Inspect plugin parameters and write immutable parameter schema.
3. Build sweep plans combining one-at-a-time, pairwise, global, and local sampling.
4. Render offline with fixed environment settings.
5. Write raw audio plus analysis copies.
6. Extract waveform-derived arrays and feature tensors.
7. Validate per-sample integrity and aggregate coverage metrics.
8. Split the dataset by source family and anchor preset lineage.
9. Fit normalization statistics on train only.
10. Export train, validation, and test manifests with immutable schema versions.

## 11. Minimum Acceptance Criteria

The dataset is ready for model training when all of the following are true:

- Continuous parameters have explicit schema, units, and transforms.
- Every sample has reproducible parameter metadata and a valid audio reference.
- Feature extraction succeeds for essentially all retained samples.
- Train, validation, and test splits are leakage-resistant.
- Coverage reports confirm both broad space exploration and boundary sampling.
- Audio QC flags are present and audited, not silently discarded.
- Storage supports random access for analysis and streamed access for training.

## 12. MorphSnap-Aligned Implementation Notes

This framework aligns with the current repository structure:

- `src/AI/Dataset/ParameterSampler.*` for LHS and constrained parameter generation
- `src/AI/Dataset/FeatureExtractor.*` for global and frame-level audio descriptors
- `src/AI/Dataset/MetadataWriter.*` for JSON schema and Parquet-oriented export
- `src/AI/Dataset/EnhancedRenderPipeline.*` for controlled rendering
- `src/AI/Dataset/ValidationEngine.*` for distribution and coverage checks

For this repo, the most practical implementation path is:

- Use the existing host-normalized parameter control path as the ground-truth automation source
- Add explicit canonical transform metadata per parameter
- Store audio separately from metadata tables
- Keep feature tensors chunked instead of embedding large arrays directly in JSON

This keeps the synthetic dataset compatible with both plugin QA workflows and ML training pipelines.

---

## Validation Status

This framework has been validated against the following criteria:

| Criterion | Target | Status |
|-----------|--------|--------|
| Unit test coverage | 100% pass | ✓ Validated |
| Scale test | 10,000+ samples | ✓ Validated |
| Memory usage | < 4 GB | ✓ Validated |
| Audio output | Non-silent, correct format | ✓ Validated |
| Feature dimensions | 31+ per sample | ✓ Validated |

See `validation/FINAL_VALIDATION_REPORT.md` for detailed results.
