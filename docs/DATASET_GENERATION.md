# MorphSnap Dataset Generation Guide

**Version:** 3.3.0  
**Last Updated:** March 2026

---

## Overview

MorphSnap includes a comprehensive dataset generation system for creating synthetic audio training data for Machine Learning models. The system renders audio clips with various parameter configurations from hosted plugins and exports both the audio and corresponding parameter metadata.

> **Note:** Dataset V3 is always compiled. `-DMORPHSNAP_ENABLE_DATASET_V3=ON|OFF` is retained only as a deprecated compatibility flag (no-op).

---

## DatasetGeneratorV2 — Sequential Pipeline

The V2 dataset generator provides a complete end-to-end pipeline for generating synthetic audio datasets with ground-truth DSP parameters.

### Architecture

```
┌─────────────────┐    ┌──────────────────┐    ┌─────────────────┐
│ ParameterSampler│───▶│ PluginChainEngine │───▶│EnhancedRender   │
│ (LHS Sampling)  │    │ (EQ → Comp → Lim) │    │ Pipeline        │
└─────────────────┘    └──────────────────┘    └────────┬────────┘
                                                       │
                       ┌──────────────────┐            │
                       │ FeatureExtractor │◀───────────┘
                       │ (MFCC, LUFS...) │
                       └────────┬────────┘
                                │
        ┌───────────────────────┼───────────────────────┐
        ▼                       ▼                       ▼
┌───────────────┐    ┌─────────────────┐    ┌────────────────┐
│ MetadataWriter│    │ ValidationEngine │    │ DatasetOrganizer│
│ (JSON/Parquet)│    │ (Stats Report)   │    │ (Train/Val/Test)│
└───────────────┘    └─────────────────┘    └────────────────┘
```

### Components

#### [ParameterSampler.h](src/AI/Dataset/ParameterSampler.h)

Advanced parameter sampling with multiple strategies:

| Method | Description |
|--------|-------------|
| `generateLHS()` | Latin Hypercube Sampling for uniform parameter space coverage |
| `generateStratified()` | Genre-aware stratified sampling |
| `validateConstraints()` | Validates parameter constraints |
| `applyConstraints()` | Applies parameter constraints in-place |

**Sampling Configuration:**
```cpp
SamplingConfig config;
config.sampleCount = 1000;
config.seed = 42;
config.addGenreStratum("Electronic", 0.25);
config.addGenreStratum("RockPop", 0.20);
```

**Distribution Types:**
- `Linear` — Uniform distribution in linear space
- `Logarithmic` — Uniform distribution in log space (for frequency-like params)
- `Exponential` — Exponential distribution (more values at lower end)

#### [AudioContentLibrary.h](src/AI/Dataset/AudioContentLibrary.h)

Manages source audio content with genre classification:

**Supported Genres:**
| Genre | Default % |
|-------|-----------|
| Electronic | 25% |
| RockPop | 20% |
| JazzAcoustic | 15% |
| HipHopRnB | 15% |
| Classical | 5% |
| Stems | 15% |
| TestSignals | 5% |

**Key Features:**
- Characteristic extraction (spectral centroid, LUFS, RMS, etc.)
- Audio augmentation (time stretch, pitch shift, dynamics)
- Genre-based content retrieval

#### [PluginChainEngine.h](src/AI/Dataset/PluginChainEngine.h)

Sequential plugin chain processing:

**Chain Types:**
| Type | Description |
|------|-------------|
| `EQOnly` | Single EQ plugin |
| `DynamicsOnly` | Single dynamics plugin |
| `Mastering` | EQ → Compressor → Limiter |
| `Mixing` | Multiple mixing plugins |
| `Creative` | Creative effects chain |
| `Custom` | User-defined chain |

**Features:**
- Multi-plugin sequential processing
- Global parameter indexing across all plugins
- Per-plugin bypass control
- Settle time management

#### [EnhancedRenderPipeline.h](src/AI/Dataset/EnhancedRenderPipeline.h)

Multi-segment audio rendering:

**Segment Types:**
| Segment | Duration |
|---------|----------|
| Full | 30 seconds |
| Transient | 2 seconds |
| SteadyState | 5 seconds |
| Custom | User-defined |

**Output Formats:**
- `WAV32Float` — 48kHz/32-bit float WAV
- `WAV24` — 48kHz/24-bit WAV
- `FLAC24` — 48kHz/24-bit FLAC

**Features:**
- Batch rendering with progress callbacks
- Parallel multi-threaded rendering
- Audio validation (silence/clipping detection)
- Target throughput: 10,000 samples/hour

#### [FeatureExtractor.h](src/AI/Dataset/FeatureExtractor.h)

Audio feature extraction using JUCE DSP:

**Feature Types:**

| Category | Features |
|----------|----------|
| Spectral | MFCC (13), Spectral Centroid, Rolloff, Flux, Spread, Flatness, Chroma (12) |
| Temporal | RMS Energy, Peak, Crest Factor, Attack Time, Transient Density, Zero-Crossing Rate |
| Perceptual | LUFS, True Peak, Roughness, Sharpness, Brightness, Dynamic Range |

**Total feature dimension:** 31 floats

#### [MetadataWriter.h](src/AI/Dataset/MetadataWriter.h)

Comprehensive metadata management:

**Output Formats:**
- JSON (manifest files)
- CSV (for Parquet conversion)
- Feature tables for ML training

**Metadata Schema:**
```json
{
  "sampleId": "...",
  "source": {
    "filePath": "...",
    "genre": "Electronic",
    "lufs": -14.5,
    "dynamicRangeDb": 8.2
  },
  "chain": {
    "chainType": "Mastering",
    "plugins": [...]
  },
  "output": {
    "lufs": -16.2,
    "truePeakDb": -1.0,
    "dynamicRangeDb": 10.5
  },
  "spectralFeatures": {...},
  "temporalFeatures": {...},
  "perceptualFeatures": {...},
  "targets": {
    "parameterRegression": [...],
    "styleClassification": "Electronic"
  },
  "split": "train"
}
```

#### [ValidationEngine.h](src/AI/Dataset/ValidationEngine.h)

Statistical validation and coverage analysis:

**Tests:**
- Kolmogorov-Smirnov (KS) test
- Maximum Mean Discrepancy (MMD) test
- Wasserstein distance test

**Coverage Metrics:**
| Metric | Target |
|--------|--------|
| Volume Coverage Ratio | > 75% |
| Grid Coverage | > 80% |
| Boundary Coverage | > 15% |
| Transfer Performance Gap | < 15% |

#### [DatasetOrganizer.h](src/AI/Dataset/DatasetOrganizer.h)

Dataset directory management:

**Directory Structure:**
```
root/
├── audio/
│   ├── train/{genre}/
│   ├── val/{genre}/
│   └── test/{genre}/
├── metadata/
│   └── manifests.json
├── features/
│   ├── spectral/
│   ├── temporal/
│   └── perceptual/
└── targets/
    ├── regression/
    └── classification/
```

**Split Configuration:**
```cpp
SplitConfig config;
config.trainRatio = 0.70f;
config.valRatio = 0.15f;
config.testRatio = 0.15f;
config.stratifyByGenre = true;
config.stratifyByIntensity = true;
```

#### [DatasetConfig.h](src/AI/Dataset/DatasetConfig.h)

CLI interface and configuration schema:

**Command-Line Options:**
```bash
morphsnap-dataset -c config.json                    # From config file
morphsnap-dataset -o ./dataset -n 1000              # Direct options
morphsnap-dataset -c config.json --dry-run          # Validate only

Options:
  -h, --help           Show help message
  -c, --config FILE    Configuration file (JSON)
  -o, --output DIR     Output directory
  -s, --source DIR     Source audio directory
  -n, --samples N      Total samples to generate
  --seed N             Random seed
  --dry-run            Validate without rendering
  -v, --verbose        Verbose output
  --chain TYPE          Chain type
  -t, --threads N      Parallel threads
```

---

## DatasetGeneratorV3 — Modular Pipeline

V3 adds a high-performance modular pipeline with parallel processing, resource monitoring, and crash recovery. It is always compiled in current builds.

### Architecture

```
ParameterSampler ──SPSC──▶ DispatchThread ──MPMC──▶ WorkerPool
    (Capture)              (Batching)            (Extract+Transform+Validate)
                                                       │
                                                   ┌──▼──┐
                                           I/O Thread │Serialize│
                                                   └──────┘
        │                    │                    │
        ▼                    ▼                    ▼
┌───────────────┐  ┌──────────────────┐  ┌────────────────┐
│ResourceMonitor│  │  CheckpointMgr   │  │GenerationLogger│
│(Adaptive CPU) │  │  (Crash Recovery)│  │(JSON Logging)  │
└───────────────┘  └──────────────────┘  └────────────────┘
                              │
                    ┌─────────┴─────────┐
                    │   WatchdogTimer   │
                    │ (Hung Detection)  │
                    └───────────────────┘
```

### Components

#### [TaskQueue.h](src/AI/Dataset/Scheduling/TaskQueue.h)

Thread-safe MPMC (Multiple Producer Multiple Consumer) queue:

**Features:**
- Priority-aware task scheduling (LOW, NORMAL, HIGH)
- Bounded capacity with backpressure
- Non-blocking `tryPush()` for throttling

```cpp
TaskQueue queue(4096);  // 4096 task capacity

// Push with backpressure
if (!queue.tryPush({work, TaskPriority::NORMAL, batchId})) {
    // Throttle: queue is 90%+ full
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
```

#### [WorkerPool.h](src/AI/Dataset/Scheduling/WorkerPool.h)

Configurable thread pool for parallel batch processing:

**Features:**
- Dynamic thread count (default: hardware_concurrency / 2)
- Below-normal thread priority (audio thread friendly)
- Error handling with callbacks
- Task completion tracking

```cpp
WorkerPool pool(taskQueue, 4);  // 4 worker threads
pool.start();

// Monitor progress
std::cout << "Completed: " << pool.tasksCompleted() << "\n";
```

#### [ResourceMonitor.h](src/AI/Dataset/Monitoring/ResourceMonitor.h)

Adaptive CPU/RAM throttling:

**System Load Levels:**
| Level | CPU Usage | Action |
|-------|-----------|--------|
| LOW | < 30% | Full speed |
| MEDIUM | 30-70% | Normal |
| HIGH | 70-90% | Throttle 50% |
| CRITICAL | > 90% | Throttle 75%, reduce batch |

**Features:**
- Windows CPU/RAM monitoring
- Queue fill ratio tracking
- Composite load scoring

```cpp
ResourceMonitor monitor(std::chrono::milliseconds(1000), 2ULL * 1024 * 1024 * 1024);
monitor.setQueueFillProvider([&]() { return taskQueue.fillRatio(); });
monitor.start();

// Query load level
if (monitor.getLoadLevel() == SystemLoadLevel::HIGH) {
    // Reduce capture rate
}
```

#### [ProgressTracker.h](src/AI/Dataset/Monitoring/ProgressTracker.h)

Real-time progress and ETA tracking:

**Snapshot Data:**
```cpp
ProgressTracker::Snapshot s = tracker.getSnapshot();
std::cout << s.percentComplete << "% complete\n";
std::cout << "ETA: " << s.etaSeconds << " seconds\n";
std::cout << "Rate: " << s.batchesPerSecond << " batches/sec\n";
```

#### [GenerationLogger.h](src/AI/Dataset/Monitoring/GenerationLogger.h)

Structured JSON logging for pipeline events:

**Logged Events:**
- Generation start/end
- Batch completion
- Validation results
- Errors and warnings
- Resource utilization snapshots

#### [CheckpointManager.h](src/AI/Dataset/Recovery/CheckpointManager.h)

Crash recovery with periodic state snapshots:

**Features:**
- Configurable checkpoint interval (default: 100 batches)
- Full state serialization
- Resume from any checkpoint
- Checkpoint validation

```cpp
CheckpointManager checkpoint(config.checkpointDirectory, 100);
checkpoint.save(batchesCompleted, framesProcessed, elapsedSeconds);

// On restart
if (checkpoint.load(lastCheckpoint)) {
    generator.resumeFromCheckpoint();
}
```

#### [WatchdogTimer.h](src/AI/Dataset/Recovery/WatchdogTimer.h)

Hung thread detection:

**Features:**
- Per-thread timeout tracking
- Automatic thread termination on hang
- Configurable timeout (default: 5 seconds)

---

## Configuration

### V2 Configuration

```json
{
  "outputDirectory": "./dataset",
  "datasetName": "morphsnap_dataset",
  "totalSamples": 1000,
  "randomSeed": 42,
  "sampleRate": 48000,
  "blockSize": 512,
  "numChannels": 2,
  "outputFormat": "WAV32Float",
  "chainType": "Mastering",
  "useAugmentation": true,
  "numParallelThreads": 4,
  "pluginSettleTimeMs": 50,
  "enableValidation": true,
  "samplingConfig": {
    "sampleCount": 1000,
    "seed": 42,
    "genreStrata": ["Electronic:0.25", "RockPop:0.20"]
  },
  "splitConfig": {
    "trainRatio": 0.7,
    "valRatio": 0.15,
    "testRatio": 0.15,
    "stratifyByGenre": true,
    "stratifyByIntensity": true
  }
}
```

### V3 Extended Configuration

```json
{
  "baseConfig": { ... },
  "batchSize": 2048,
  "workerThreads": 0,
  "maxQueueDepth": 4096,
  "checkpointInterval": 100,
  "enableAdaptiveThrottle": true,
  "enableWatchdog": true,
  "watchdogTimeoutMs": 5000,
  "memoryLimitBytes": 2147483648,
  "maxQueueFillBeforeThrottle": 0.9,
  "outputDirectory": "./dataset_v3",
  "logDirectory": "./logs",
  "checkpointDirectory": "./checkpoints"
}
```

---

## MCP Integration

Generate datasets via the embedded MCP server:

```json
{
  "method": "generate_dataset",
  "params": {
    "pipeline": "v3",
    "samples": 250,
    "duration": 2.0,
    "input_audio": "C:/Audio/Drums_Loop.wav",
    "output_path": "C:/ML_Datasets/Drums_Processor",
    "respect_sanity": true
  }
}
```

`generate_dataset` is the compatibility entry point. Set `pipeline` to `v2` or `v3` to route to the current dataset pipelines; use `legacy` only when you explicitly need the original minimal renderer.

Direct MCP methods are also available:

- `generate_dataset_v2` — sequential full pipeline
- `generate_dataset_v3` — async modular pipeline

**Parameters:**
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `pipeline` | string | `v3` | Generator selection: `legacy`, `v2`, or `v3` |
| `samples` | int | 100 | Number of samples to generate |
| `duration` | float | 1.0 | Duration in seconds per sample |
| `input_audio` | string | - | Source audio file path |
| `output_path` | string | auto | Output directory |
| `respect_sanity` | bool | true | Avoid dangerous parameters |

---

## Output Structure

Generated dataset structure:

```
output_path/
├── dataset_audio.wav          # All clips concatenated
├── dataset_metadata.json      # Full metadata array
├── manifest.json              # Dataset manifest
├── validation_report.json     # Statistical validation
├── audio/
│   ├── train/
│   │   ├── Electronic/
│   │   ├── RockPop/
│   │   └── ...
│   ├── val/
│   └── test/
├── metadata/
│   └── manifests.json
├── features/
│   ├── spectral/
│   ├── temporal/
│   └── perceptual/
└── targets/
    ├── regression/
    └── classification/
```

---

## Building

### Standard Build
```bash
cmake -B build -S . -DMORPHSNAP_ENABLE_DATASET_V3=OFF
cmake --build build --config Release
```

### Compatibility-Flag Build (Equivalent Output)
```bash
cmake -B build -S . -DMORPHSNAP_ENABLE_DATASET_V3=ON -DMORPHSNAP_BUILD_TESTS=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

---

## Performance Characteristics

| Component | CPU | Memory |
|-----------|-----|--------|
| Parameter Sampling | ~50ms | 512 KB |
| Audio Rendering | Per-sample | ~10 MB |
| Feature Extraction | ~100ms/sample | 2 MB |
| Validation | ~200ms | 1 MB |
| **V2 Total** | **<1% per sample** | **~15 MB** |
| **V3 WorkerPool** | **Scales with cores** | **+N × 10 MB** |

---

## References

- [AudioEngineSpec_v2.md](docs/AudioEngineSpec_v2.md) — Audio engine specification
- [SYNTHETIC_AUDIO_DATASET_METHODOLOGY.md](docs/architecture/SYNTHETIC_AUDIO_DATASET_METHODOLOGY.md) — Dataset methodology
- [CMakeLists.txt](../CMakeLists.txt) — Build configuration
