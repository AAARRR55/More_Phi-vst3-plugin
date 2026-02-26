# MorphSnap V2 — Technical Specification & Implementation Guide

**Version**: 2.0.0 (Draft)
**Status**: Specification Phase
**Date**: 2026-02-26

---

## Executive Summary

MorphSnap V2 represents a ground-up reimagination of the MorphSnap audio plugin, evolving from a parameter-domain morph engine into a full audio-domain creative processing platform. V2 retains the core innovation of snapshot-based morphing while adding spectral processing, granular synthesis, a modulation matrix, ML-powered sound matching, and a modern visualization layer.

### What's New in V2

| Capability | V1 (Current 3.3.0) | V2 (Target) |
|-----------|-------------------|-------------|
| Morphing Domain | Parameter interpolation only | Parameter + Spectral + Granular |
| Snapshot Slots | 12 fixed | 12 primary + 64 extended library |
| Morph Control | 2D XY pad, 1D fader | 2D pad + gesture trails + automation recorder |
| Physics Modes | Direct, Elastic, Drift | + Orbital, Flocking, Gravity Well |
| Modulation | None (static routing only) | Full mod matrix: 16 sources x unlimited routes |
| Mod Sources | — | 4 LFOs, 2 Envelopes, 2 Step Sequencers, MIDI, XY position |
| Visualization | Minimal (dots on pad) | Real-time spectrogram, waveform, mod routing view |
| AI/ML | MCP server (parameter access) | + Latent space morphing, sound matching, generative presets |
| Preset System | XML-based meta-presets | JSON presets with metadata, tags, cloud sharing |
| Audio Engine | Single hosted plugin passthrough | Oversampling, spectral processing, granular stage |
| Plugin Formats | VST3, AU | + CLAP, AAX consideration |
| Build System | CMake + FetchContent | + CMake presets, CI/CD, pluginval validation |

### Design Principles

1. **Audio Thread Sanctity** — Zero allocations, no locks, no exceptions in `processBlock()`. Non-negotiable, carries forward from V1.
2. **Additive Complexity** — New processing stages (spectral, granular) are optional and bypassable. CPU cost scales with features enabled.
3. **Backwards Compatibility** — V1 presets load in V2 with automatic migration. The parameter-domain morph engine remains the default mode.
4. **Testability** — Every subsystem has an abstract interface for mock injection. New components follow the established `IInterface` pattern.

---

## Table of Contents

1. [Overall Architecture & Core Framework](#1-overall-architecture--core-framework)
2. [Morphing Algorithms & DSP](#2-morphing-algorithms--dsp)
3. [User Interface Design](#3-user-interface-design)
4. [Parameter Modulation System](#4-parameter-modulation-system)
5. [Preset System Architecture](#5-preset-system-architecture)
6. [Audio Engine Implementation](#6-audio-engine-implementation)
7. [Compatibility & Deployment](#7-compatibility--deployment)
8. [Development Workflow & Build Process](#8-development-workflow--build-process)

---

## 1. Overall Architecture & Core Framework

### 1.0 Architectural Philosophy

MorphSnap V2 evolves the existing V1 architecture additively. The core parameter-domain morph pipeline remains intact and continues to work unchanged. V2 layers three new processing domains (spectral, granular, formant) alongside the parameter path, plus a modulation matrix that can modulate any destination in the system. The guiding constraint is that **no V2 feature may violate the audio-thread contract**: zero allocations, no locks, no exceptions, no blocking I/O in `processBlock()`.

#### V1 → V2 Migration Contract

| Guarantee | Detail |
|-----------|--------|
| V1 preset compatibility | V1 `.xml` state loads in V2 via `setStateInformation()`. Missing V2 fields default to bypass values. |
| V1 MIDI mapping preservation | C3-B3 trigger + CC1 fader routing carries forward unchanged. V2 adds CC2-CC16 routing for mod sources. |
| V1 MCP API backwards-compat | All V1 JSON-RPC tools remain. V2 adds new tool namespaces (`spectral.*`, `modulation.*`, `preset.library.*`). |
| Default mode = V1 behaviour | On first load, spectral/granular/formant engines are bypassed. The plugin behaves identically to V1. |

### 1.1 V2 Processing Pipeline

#### Signal Flow Diagram

```
                    ┌────────────────────────────────────────────────────────┐
                    │              processBlock() — audio thread            │
                    └────────────────────────────────────────────────────────┘
                                           │
           ┌───────────────────────────────┼──────────────────────────────┐
           ▼                               ▼                              ▼
  ① Drain LockFreeQueue        ② MIDIRouter.process()         ③ Sync atomics
     <ParamCommand, 8192>         ├─ snapshot triggers            ├─ physics params
     (MCP/UI → audio)             ├─ CC routing                   ├─ morph position
                                  └─ mod MIDI input               └─ toggle states
                                           │
                                           ▼
                          ┌────────────────────────────────┐
                          │  ④ MorphProcessor::process()   │
                          │    physics → interpolation →   │
                          │    smoothing → morphOutput[]   │
                          └────────────────────────────────┘
                                           │
                     ┌─────────────────────┼─────────────────────┐
                     ▼                     ▼                     ▼
         ⑤ ModulationEngine     ⑥ DiscreteHandler      ⑦ Touch Detection
            .processBlock()        .snap()                (cooldown filter)
            modifies output        quantize discrete      skip manually-
            in-place               params to steps        touched params
                     │                     │                     │
                     └─────────────────────┼─────────────────────┘
                                           ▼
                          ┌────────────────────────────────┐
                          │  ⑧ ParameterBridge::applyAll() │
                          │    write normalized floats to   │
                          │    hosted Plugin A              │
                          └────────────────────────────────┘
                                           │
                     ┌─────────────────────┼─────────────────────┐
                     ▼                     ▼                     ▼
              ⑨ Plugin A              ⑩ Plugin B           ⑪ Morph Position
                 processBlock()          processBlock()        → Link broadcast
                 → bufferA               → bufferB             → Drift output
                                                               → Trail ring
                     │                     │
                     ▼─────────────────────▼
         ┌──────────────────────────────────────────┐
         │   ⑫ Audio-Domain Morph Stage (NEW V2)    │
         │                                          │
         │   ┌──────────────┐  ┌────────────────┐   │
         │   │  Spectral    │  │   Granular     │   │
         │   │  MorphEngine │  │   MorphEngine  │   │
         │   │  (STFT)      │  │   (grain cloud)│   │
         │   └──────┬───────┘  └───────┬────────┘   │
         │          │                  │             │
         │          └────────┬─────────┘             │
         │                   ▼                       │
         │   ┌───────────────────────────────┐       │
         │   │  Formant Preserve (cepstral)  │       │
         │   └──────────────┬────────────────┘       │
         │                  ▼                        │
         │   ┌──────────────────────────────┐        │
         │   │  HybridBlend                 │        │
         │   │  equal-power crossfade of    │        │
         │   │  param / spectral / granular │        │
         │   └──────────────┬───────────────┘        │
         └──────────────────┼────────────────────────┘
                            ▼
                    ⑬ OversamplingWrapper
                       .downsample()
                            ▼
                    ⑭ Output buffer
                       → RMS meter
                       → DAW
```

#### Per-Block Execution Order (Code-Level)

```cpp
void MorphSnapProcessor::processBlock(AudioBuffer<float>& buffer, MidiBuffer& midi)
{
    ScopedAudioCallback allocGuard;  // Debug: tracks heap allocations
    ScopedNoDenormals noDenormals;   // Flush denormals to zero

    if (!prepared || isRestoring_.load(acquire)) {
        hostManagerA_.processBlock(buffer, midi); return;
    }

    // ① Drain command queue (bounded: max 1024 per block)
    drainCommandQueue();

    // ② MIDI routing: note triggers → snapshot recall, CC → morph/mod
    MidiBuffer filtered;
    midiRouter_.processMidi(midi, filtered);
    modulationEngine_.processMIDI(filtered);            // NEW V2

    // ③ Sync atomic parameters to local variables (one load per block)
    const auto [mx, my, fp, src, mode, dt] = loadMorphState();

    // ④ Parameter-domain morph
    morphProcessor_.process(mx, my, fp, src, mode, dt, morphOutput_);

    // ⑤ Modulation matrix: modifies morphOutput_ in-place
    modulationEngine_.processBlock(morphOutput_, dt);   // NEW V2

    // ⑥ Snap discrete params to valid steps
    discreteHandler_.snap(morphOutput_);

    // ⑦ Touch detection + apply to Plugin A
    applyWithTouchDetection(morphOutput_);

    // ⑧ Process Plugin A audio
    hostManagerA_.processBlock(buffer, filtered);

    // ⑨-⑫ Audio-domain morph (NEW V2, conditional)
    if (audioDomainEnabled_) {
        hostManagerB_.processBlock(bufferB_, filtered);
        float alpha = morphAlpha_.load(relaxed);
        spectralEngine_.processBlock(buffer, bufferB_, alpha);
        granularEngine_.processBlock(buffer, bufferB_, alpha);
        formantEngine_.processBlock(buffer);
        hybridBlend(buffer, spectralOut_, granularOut_, blendWeights_);
    }

    // ⑬ Oversampling downsample (if active)
    if (oversampling_.isActive())
        oversampling_.downsample(outputBlock_);

    // ⑭ Output metering
    setRmsLevel(buffer.getRMSLevel(0, 0, buffer.getNumSamples()));
}
```

### 1.2 Component Ownership (V2)

```
MorphSnapProcessor (juce::AudioProcessor, juce::Timer)
│
│── APVTS                        (automatable DAW parameters)
│── InstanceIdentity             (per-instance MCP identity: port, token, morphCode)
│
├── PluginHostManager  A         (hosts Plugin A — primary, inherits V1)
├── PluginHostManager  B         (NEW — hosts Plugin B for audio-domain morphing)
├── ParameterBridge    A         (reads/writes Plugin A params via IParameterBridge)
├── ParameterBridge    B         (NEW — reads/writes Plugin B params)
│
├── SnapshotBank                 (12 slots, seqlock-protected, heap-allocated ~384 KB)
│   ├── ParameterState[12]       (std::array<float, 2048> per slot, no heap alloc)
│   └── stateChunks_[12]         (MemoryBlock for Full recall VST3 opaque data)
│
├── MorphProcessor               (physics → interpolation → smoothing → morphOutput[])
│   ├── PhysicsEngine            (static methods: updateElastic, updateDrift, perlin)
│   │   ├── ElasticState         (position + velocity for spring-damper)
│   │   └── DriftMode            (Free / Locked / Orbit — Perlin noise patterns)
│   ├── InterpolationEngine      (IDW 2D, fader linear, clock positions)
│   └── GeneticEngine            (crossover, mutation, smartRandomize)
│       └── SanityConfig         (protects Volume/Pitch/Bypass from breed)
│
├── ModulationEngine             (NEW — per-sample modulation accumulator)
│   ├── ModulationMatrix         (128 routing slots, source→dest→depth)
│   ├── LFO[4]                   (sine/tri/saw/square/S&H, tempo-syncable)
│   ├── EnvelopeFollower[2]      (RMS-based, attack/release controls)
│   ├── MacroKnob[16]            (user-assignable summing knobs)
│   ├── StepSequencer[2]         (up to 32 steps, tempo-synced)
│   └── DriftRandom[2]           (Perlin noise mod source)
│
├── SpectralMorphEngine          (NEW — STFT phase vocoder, PFFFT backend)
│   ├── TransientDetector        (spectral flux → alpha switching)
│   ├── FFTProcessor             (2048-point, 75% overlap Hann)
│   └── PhaseVocoder             (instantaneous frequency interpolation)
│
├── GranularMorphEngine          (NEW — grain cloud crossfade, 128 grains max)
│   ├── GrainPool                (pre-allocated, position/pitch/env per grain)
│   └── GrainScheduler           (density control, randomized onset)
│
├── FormantMorphEngine           (NEW — cepstral envelope extraction + transplant)
│
├── VAEMorphEngine               (NEW — ONNX Runtime latent space, message thread only)
│   ├── ONNXSession              (loaded once, inference per morph request)
│   └── LatentSpaceNavigator     (dimension reduction → 2D pad mapping)
│
├── OversamplingWrapper          (JUCE dsp::Oversampling, x1/x2/x4/x8, FIR/IIR)
├── LatencyManager               (oversampling + FFT + hosted plugin → setLatencySamples)
│
├── MIDIRouter                   (C3-B3 triggers, CC1 fader, V2: CC2-16 mod routing)
├── MCPServer                    (JSON-RPC 2.0, juce::Thread, bearer auth)
│   └── MCPToolHandler           (dispatches tools: snapshot.*, morph.*, plugin.*, etc.)
├── LinkBroadcaster              (cross-instance morph sync via shared memory)
├── TokenOptimizer               (AI token budget management)
│
├── MetaPresetManager            (preset save/load/export)
│   └── PresetLibrary            (NEW — indexed search, tags, ratings, cloud sync)
│
├── ParameterClassifier          (categorizes params: Continuous/Discrete/Binary/Freq/dB/Enum)
├── DiscreteParameterHandler     (snap discrete params to valid steps during morph)
│
├── LockFreeQueue<ParamCommand, 8192>  (SPSC ring buffer, cache-line aligned)
│
└── commandQueueProducerMutex_   (serializes MCP + UI writes into SPSC queue)
```

### 1.3 Threading Model

V2 expands from three thread domains to five:

```
┌─────────────────────────────────────────────────────────────────────┐
│ THREAD DOMAINS                                                       │
├──────────────────┬──────────────────────────────────────────────────┤
│ Audio Thread     │ processBlock(), MorphProcessor::process(),       │
│ (DAW callback)   │ InterpolationEngine, PhysicsEngine,              │
│                  │ SpectralMorphEngine, GranularMorphEngine,        │
│                  │ ModulationEngine::processBlock(),                │
│                  │ OversamplingWrapper::upsample/downsample         │
│                  │                                                  │
│                  │ CONTRACT: noexcept, zero alloc, no locks,        │
│                  │ no I/O, no exceptions. All buffers pre-allocated │
│                  │ in prepareToPlay(). Maximum 1024 command queue   │
│                  │ drains per block.                                │
├──────────────────┼──────────────────────────────────────────────────┤
│ Message Thread   │ UI components (MorphPad, SnapshotRing, panels), │
│ (JUCE event loop)│ Timer callbacks (deferred plugin loading),       │
│                  │ MCP connection handling, PluginHostManager       │
│                  │ loadPlugin/unloadPlugin, prepareToPlay,          │
│                  │ state persistence, ParameterClassifier refresh   │
│                  │                                                  │
│                  │ NEW V2: VAEMorphEngine inference (sync on msg    │
│                  │ thread — ONNX models must not block audio).      │
│                  │ PresetLibrary indexing and cloud sync.           │
├──────────────────┼──────────────────────────────────────────────────┤
│ MCP Thread       │ MCPServer::run() — juce::Thread, accepts TCP    │
│ (dedicated)      │ connections on localhost, reads JSON-RPC,        │
│                  │ enqueues ParamCommand via LockFreeQueue,         │
│                  │ serialized by commandQueueProducerMutex_         │
├──────────────────┼──────────────────────────────────────────────────┤
│ FFT Thread (NEW) │ Background STFT computation for SpectralMorph   │
│                  │ when hop size allows pre-computation. Optional   │
│                  │ — falls back to inline audio-thread FFT if       │
│                  │ latency budget permits.                          │
├──────────────────┼──────────────────────────────────────────────────┤
│ Scanner Thread   │ PluginScanner background scan for discovering   │
│ (NEW, optional)  │ installed VST3/AU plugins. Uses JUCE's          │
│                  │ ChildProcessPluginScanner to isolate crashes.    │
└──────────────────┴──────────────────────────────────────────────────┘
```

#### Inter-Thread Communication Primitives

| Primitive | Location | Direction | Mechanism |
|-----------|----------|-----------|-----------|
| `LockFreeQueue<ParamCommand, 8192>` | `MorphSnapProcessor` | MCP/UI → Audio | SPSC ring buffer, power-of-2, cache-line aligned head/tail |
| Seqlock (`std::atomic<uint32_t>`) | `SnapshotBank` | UI/MCP writes, Audio reads | Odd = write in progress, even = stable. Audio retries up to 8x with `_mm_pause()` |
| `SpinLock` | `SnapshotBank::writeLock_` | UI/MCP serialization | JUCE SpinLock — serializes multiple writers before seqlock increment |
| `std::atomic<float/int/bool>` | `MorphSnapProcessor` | UI ↔ Audio | `memory_order_relaxed` for morph position, physics params, toggle states |
| `std::mutex` | `commandQueueProducerMutex_` | MCP + UI serialization | Guards SPSC push from multiple producers (converts MPSC to SPSC) |
| `std::atomic<bool> isRestoring_` | `MorphSnapProcessor` | Message → Audio | `acquire/release` — blocks morph engine during async plugin reload |
| Shared memory | `LinkBroadcaster` | Instance ↔ Instance | Cross-process morph position sync for Link Mode |
| Trail ring buffer | `MorphProcessor::trail_` | Audio → UI | `std::array<TrailPoint, 64>` with atomic head index |
| Double buffer (NEW V2) | `SpectralMorphEngine` | FFT thread → Audio | Two FFT output buffers, atomic swap pointer |

### 1.4 Memory Model & Allocation Strategy

```
PHASE 1: Construction (message thread)
  ├── SnapshotBank: heap-allocates slots_ (12 × ParameterState = ~384 KB)
  │   └── Reason: avoid stack overflow in FL Studio (small thread stacks)
  ├── LockFreeQueue: std::array<ParamCommand, 8192> (inline, ~64 KB)
  └── All other members: stack-allocated within MorphSnapProcessor

PHASE 2: prepareToPlay() (message thread)
  ├── morphOutput_.resize(2048)         — float vector, once
  ├── lastApplied_.resize(2048)          — touch detection baseline
  ├── touchCooldown_.resize(2048)        — per-param block counter
  ├── morphProcessor_.prepare(2048)      — pre-allocate smoothedValues_
  ├── snapshotBank_.prepare(2048)        — set param count ceiling
  ├── hostManager_.prepare(sr, bs, ch)   — JUCE plugin hosting buffers
  │
  │   NEW V2:
  ├── oversampling_.prepare(bs, ch, sr)  — JUCE dsp::Oversampling FIR design
  ├── spectralEngine_.prepare(sr, bs)    — FFT twiddle factors + window
  ├── granularEngine_.prepare(sr, bs)    — grain pool pre-alloc (128 grains)
  ├── modulationEngine_.prepare(sr, bs)  — mod source state + routing table
  └── latencyManager_.recompute()        — sum all latency contributors

PHASE 3: processBlock() (audio thread)
  └── ZERO allocations. All vectors pre-sized. std::fill for clearing.
      No std::vector::push_back, no new, no shared_ptr, no std::string.
      Only stack variables + pre-allocated member buffers.
```

### 1.5 Interface Design (Testability)

V1 established three abstract interfaces (`IPluginHostManager`, `IParameterBridge`, `IMCPServer`) in `Host/IPluginHostManager.h`. V2 extends this pattern for every new subsystem:

```cpp
// New V2 interfaces (each in its own header):

class IModulationEngine {
public:
    virtual ~IModulationEngine() = default;
    virtual void prepare(double sampleRate, int blockSize) = 0;
    virtual void processBlock(std::vector<float>& output, float dt) = 0;
    virtual void processMIDI(const juce::MidiBuffer& midi) = 0;
    virtual int  addRoute(int sourceId, int destParamIndex, float depth) = 0;
    virtual void removeRoute(int routeId) = 0;
    virtual void setMacro(int macroIndex, float value) = 0;
};

class ISpectralMorphEngine {
public:
    virtual ~ISpectralMorphEngine() = default;
    virtual void prepare(double sampleRate, int blockSize) = 0;
    virtual void processBlock(juce::AudioBuffer<float>& bufA,
                              juce::AudioBuffer<float>& bufB,
                              float alpha) = 0;
    virtual int  getLatencyInSamples() const = 0;
    virtual void setFFTSize(int size) = 0;  // 512, 1024, 2048, 4096
};

class IGranularMorphEngine {
public:
    virtual ~IGranularMorphEngine() = default;
    virtual void prepare(double sampleRate, int blockSize) = 0;
    virtual void processBlock(juce::AudioBuffer<float>& bufA,
                              juce::AudioBuffer<float>& bufB,
                              float alpha) = 0;
    virtual void setGrainDensity(float grainsPerSecond) = 0;
    virtual void setGrainSize(float ms) = 0;
};

class IPresetLibrary {
public:
    virtual ~IPresetLibrary() = default;
    virtual std::vector<PresetEntry> search(const std::string& query) = 0;
    virtual bool save(const PresetEntry& preset) = 0;
    virtual bool load(const std::string& presetId) = 0;
};
```

### 1.6 Error Handling & Recovery

| Failure Mode | Detection | Recovery |
|-------------|-----------|----------|
| Hosted plugin crash | `PluginHostManager::getPlugin()` returns null after crash | Unload, set `isRestoring_`, Timer-based reload (max 10 retries, 50ms interval) |
| MCP socket failure | `MCPServer::errorCount_` > `MAX_CONSECUTIVE_ERRORS` (5) | `attemptRecovery()`: close socket, rebind after `RECOVERY_DELAY_MS` (1000ms) |
| Seqlock starvation | `tryReadLocked()` exhausts 8 retries | Audio thread uses stale snapshot data (acceptable: max 8 × `_mm_pause()` delay ≈ 0.5μs) |
| Command queue full | `LockFreeQueue::push()` returns false | Drop command, MCP returns error to client. Queue drains at 1024/block = ~48k/sec at 48kHz/1024. |
| State restore race | `isRestoring_ == true` in processBlock | Forward audio through host passthrough, skip morph computation |
| FFT overflow (V2) | Spectral engine input exceeds buffer | Clamp to pre-allocated FFT size, log once per session |
| ONNX inference timeout (V2) | VAE model >100ms latency | Cancel inference, return last valid latent vector |

### 1.7 Plugin Format & Host Compatibility

| Format | V1 | V2 | Notes |
|--------|----|----|-------|
| VST3 | Yes | Yes | Primary format, all platforms |
| AU | Yes (macOS) | Yes (macOS) | AudioUnit v2 via JUCE wrapper |
| CLAP | No | Planned | Via `clap-juce-extensions`, post-MVP |
| AAX | No | Under evaluation | Requires iLok SDK, Pro Tools market |

| DAW | Verified | Known Issues |
|-----|----------|--------------|
| FL Studio | Yes | Requires `/STACK:4194304` (4 MB), small thread stacks cause stack overflow without heap-allocated SnapshotBank |
| Ableton Live | Yes | — |
| Reaper | Yes | — |
| Logic Pro | Yes (AU) | — |
| Bitwig | Yes | — |
| Studio One | Yes | Plugin-in-plugin hosting may require increased buffer size |

### 1.8 V2 Implementation Phases

```
Phase 0 — Infrastructure (Weeks 1-3)
  ├── OversamplingWrapper ✓ (implemented)
  ├── LatencyManager ✓ (implemented)
  ├── CMakePresets.json ✓ (implemented)
  ├── CI/CD pipeline ✓ (implemented)
  ├── .clang-tidy RT-safety checks ✓ (implemented)
  └── Second PluginHostManager instance (Plugin B)

Phase 1 — Modulation Engine (Weeks 4-6)
  ├── ModulationMatrix (128 slots, per-sample accumulation)
  ├── LFO × 4 (6 shapes, tempo sync, phase offset)
  ├── EnvelopeFollower × 2 (RMS-based, attack/release)
  ├── MacroKnob × 16 (summing knobs, APVTS-backed)
  └── StepSequencer × 2 (32 steps, swing, direction modes)

Phase 2 — Spectral Morph (Weeks 7-9)
  ├── SpectralMorphEngine (STFT phase vocoder)
  ├── PFFFT integration (SSE/NEON optimized FFT)
  ├── TransientDetector (spectral flux → alpha switching)
  ├── Formant preservation (cepstral envelope)
  └── Audio-domain blend stage in processBlock

Phase 3 — Granular + Preset Library (Weeks 10-11)
  ├── GranularMorphEngine (128-grain pool, density/size controls)
  ├── HybridBlend (equal-power crossfade: param/spectral/granular)
  ├── PresetLibrary (JSON format, indexing, search, tags)
  └── Cloud sync API (optional, requires server backend)

Phase 4 — ML Integration + Polish (Weeks 12-14)
  ├── VAEMorphEngine (ONNX Runtime, message thread inference)
  ├── LatentSpaceNavigator (2D projection for morph pad)
  ├── Extended physics modes (Orbital, Flocking, Gravity Well)
  └── UI visualization layer (spectrogram, waveform, mod routing)
```

---

## 2. Morphing Algorithms & DSP

### 2.0 V1 Limitations and V2 Motivation

The current `MorphProcessor::process()` pipeline operates exclusively in the parameter domain. Its output is a `std::vector<float>` of normalized parameter values that `ParameterBridge` writes to the hosted plugin. V2 adds an orthogonal audio-domain processing layer. The new `SpectralMorphEngine`, `GranularMorphEngine`, and `FormantMorphEngine` classes slot into the processing chain without breaking the existing parameter path.

### 2.1 Mathematical Foundations

#### IDW Formula (Formalized)

`compute2D` implements Shepard's method with `mu = 2`:

```
w_i(q) = 1 / ||q - p_i||^2
output(k) = sum(w_i * s_i(k)) / sum(w_i)
```

Dimension-agnostic — extends directly to N-dimensional latent space navigation.

#### Spring-Damper System

Continuous: `x''(t) + c*x'(t) + k*(x(t) - x_target) = 0`

| Preset | k | c | zeta | Behaviour |
|--------|---|---|------|-----------|
| Slow | 0.5 | 0.3 | 0.212 | Underdamped, oscillates |
| Medium | 2.0 | 0.7 | 0.247 | Underdamped, bouncy |
| Heavy | 8.0 | 0.95 | 0.168 | Underdamped, fast snap |

V2: expose `zeta` directly (`c = 2*zeta*sqrt(k)`), switch to RK4 integration for stability.

#### Genetic Crossover

V2 adds proper two-point crossover with Gaussian mutation `N(0, mu^2)` instead of uniform, making large mutations exponentially unlikely.

### 2.2 Spectral Morphing via STFT Phase Vocoder

#### Spectral Interpolation (Log-Magnitude Geometric Mean)

```
|M_morph[m,k]| = |A[m,k]|^(1-alpha) * |B[m,k]|^alpha
```

Perceptually linear because the ear perceives loudness logarithmically.

#### Phase Alignment: Instantaneous Frequency Vocoder (Recommended)

```
IF_morph[m,k] = (1-alpha)*IF_A[m,k] + alpha*IF_B[m,k]
phi_morph[m,k] = phi_morph[m-1,k] + IF_morph[m,k] * H
```

#### Transient Detection (Spectral Flux)

```cpp
struct TransientDetector
{
    static constexpr float kThreshold = 3.0f;
    static constexpr float kReleaseMs = 50.0f;
    float prevMag[2048] = {};
    float releaseCoeff = 0.0f, transientMix = 0.0f;

    void prepare(float sampleRate, int hopSize) noexcept {
        releaseCoeff = std::exp(-1.0f / ((kReleaseMs/1000.0f) * sampleRate / hopSize));
    }

    float process(const float* mag, int numBins, float alpha) noexcept {
        float sf = 0.0f, rms = 0.0f;
        for (int k = 0; k < numBins; ++k) {
            float diff = mag[k] - prevMag[k];
            if (diff > 0.0f) sf += diff;
            rms += mag[k] * mag[k];
            prevMag[k] = mag[k];
        }
        rms = std::sqrt(rms / numBins + 1e-10f);
        transientMix = (sf/rms > kThreshold) ? 1.0f : transientMix * releaseCoeff;
        float alphaHard = alpha >= 0.5f ? 1.0f : 0.0f;
        return alphaHard * transientMix + alpha * (1.0f - transientMix);
    }
};
```

#### STFT Parameters

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| FFT size N | 2048 | 46ms, good freq resolution |
| Hop size H | N/4 = 512 | 75% overlap, perfect Hann |
| Total latency | ~58ms | N + H = 2560 samples at 44.1kHz |
| Low-latency mode | N=1024 | ~29ms for live performance |

#### Dual-Plugin Architecture

Memory per SpectralMorphEngine: ~88 KB/channel, ~176 KB stereo, allocated once in `prepare()`.

### 2.3 Granular Morphing Engine

#### Source Selection by Morph Position

```
P(source = A) = 1 - alpha
P(source = B) = alpha
```

Stochastic grain assignment produces natural crossfades.

#### Scheduling

- **Synchronous**: Regular intervals + jitter — produces pitched output
- **Asynchronous**: Poisson-distributed inter-onset — produces textures

#### Lock-Free Grain Pool

```cpp
struct GrainPool
{
    static constexpr int MAX_GRAINS = 256;  // 16 KB, fits L1 cache
    struct Grain {
        std::atomic<bool> active{false};
        int source, onset, duration, readPos;
        float amplitude, pitchRatio, pan;
        char padding[/*cache line alignment*/];
    };
    alignas(64) Grain grains[MAX_GRAINS];

    int claim() noexcept {  // CAS-based, no heap allocation
        for (int i = 0; i < MAX_GRAINS; ++i) {
            bool expected = false;
            if (grains[i].active.compare_exchange_strong(expected, true,
                    std::memory_order_acq_rel))
                return i;
        }
        return -1;
    }
    void release(int idx) noexcept {
        grains[idx].active.store(false, std::memory_order_release);
    }
};
```

### 2.4 Formant-Preserving Morphing

Cepstral envelope extraction + independent formant interpolation:

```
E_morph[k] = E_A[k]^(1-alpha) * E_B[k]^alpha
X_morph[k] = ex_morph[k] * E_morph[k]
```

LPC alternative: morph via **Line Spectral Pairs (LSPs)** for guaranteed filter stability.

### 2.5 Hybrid Mode (Parameter + Spectral + Granular)

Equal-power crossfade: `w_param = cos(blend*pi/2)^2`, `w_spectral = sin(blend*pi/2)^2`

Automatic domain selection via score-based softmax with temperature T=0.5.

### 2.6 Machine Learning Enhanced Morphing

#### VAE for Latent Space Morphing

```
Encoder: [2048] → 512 → 128 → 32 (mu + logvar)
Decoder: [32] → 128 → 512 → 2048 (Sigmoid)
~1.5M params, ~6MB ONNX, <1ms inference
```

SLERP interpolation in latent space (constant-speed traversal). ONNX Runtime on message thread only — transfer decoded params via `LockFreeQueue<ParamCommand>`.

#### AI Sound Matching

Features: MFCCs[0..12] + spectral centroid + flux + RMS + ZCR (17 total).
Nelder-Mead in 2D morph space, 30-80 evaluations, 3-8 seconds offline.
Real-time: precomputed 16x16 feature grid, nearest-neighbor lookup.

#### Generative Sound Design

Genetic breeding with ML fitness = target proximity + novelty search. User preference learning via Bradley-Terry model.

### 2.7 Performance

#### CPU Budget (44.1kHz, 512-sample block)

| Stage | Budget |
|-------|--------|
| Plugin A + B processing | ~6ms |
| STFT analysis + morph + ISTFT | ~0.7ms |
| Granular scheduling + synthesis | ~0.7ms |
| Physics + IDW + smoothing | ~0.1ms |
| **Headroom** | **~3.35ms (29%)** |

Graceful degradation: spectral dropped first, then granular density reduced, then Plugin B disabled.

#### Library Recommendations

| Need | Library | License |
|------|---------|---------|
| FFT | PFFFT (~3x faster than JUCE FFT) | BSD |
| ML Inference | ONNX Runtime 1.18+ | MIT |
| SIMD log/exp | SVML (MSVC) / Cephes poly (GCC) | Free |
| Optimizer | nlopt | LGPL |

### 2.8 V2 Class Architecture

```cpp
namespace morphsnap {

class SpectralMorphEngine {
public:
    void prepare(double sampleRate, int maxBlockSize, int fftSize = 2048);
    void processBlock(const float* inputA, const float* inputB,
                      float* output, int numSamples, float alpha) noexcept;
    void setMorphMode(SpectralMorphMode mode);
    bool popAnalysisFrame(SpectralFrame& outFrame) noexcept;
private:
    // All buffers allocated in prepare(), zero-alloc processBlock()
    TransientDetector transientDet_;
    LockFreeQueue<SpectralFrame, 32> frameExportQueue_;
    PFFFT_Setup* fftSetup_ = nullptr;
};

class GranularMorphEngine {
public:
    void prepare(double sampleRate, int maxBlockSize);
    void processBlock(float* output, int numSamples, float alpha) noexcept;
    void setGrainDensity(float grainsPerSec);
    void setGrainDuration(float durationMs);
private:
    GrainPool pool_;  // 16 KB, pre-allocated
};

} // namespace morphsnap
```

---

## 3. User Interface Design

### 3.1 Visual Design Philosophy: "Gravitational Field"

The v2 identity moves from flat-dark to **deep space + bioluminescent accents** — dark enough to feel premium, lit enough to feel alive. The morph cursor is a mass, snapshots are attractors, and the visual language makes the physics tangible.

### 3.2 Extended Color System (18 tokens)

```
BACKGROUNDS                          ACCENTS
  --bg-void          #070e19           --accent-coral     #ec415d  (primary)
  --bg-deep          #0d1b2a           --accent-violet    #7c3aed  (new)
  --bg-surface       #16213e           --accent-cyan      #22d3ee  (spectral)
  --bg-raised        #1a2742           --accent-emerald   #4ade80  (MCP online)
  --bg-elevated      #1f2f52           --accent-amber     #fbbf24  (warnings)
  --bg-pad           #0a1628           --accent-trail     #e94560  (trail)

TEXT                                  BORDERS / GLOW
  --text-primary     #e8eaed           --border-base      #1e3a5f
  --text-secondary   #8b95a5           --border-active    #ec415d60
  --text-dim         #4a5568           --glow-coral       #ec415d25
  --text-ghost       #2d3748
```

### 3.3 View System (5 tabbed views)

A tab strip of 5 icon-buttons in the title bar switches the main content area:

1. **Main View** (default) — enhanced morph pad + snapshot grid
2. **Spectral View** — real-time spectrogram + spectrum analyzer
3. **Modulation View** — visual cable routing matrix
4. **Preset Browser** — searchable library with thumbnails
5. **Advanced Panel** — learn mode, genetics, AI/MCP config

### 3.4 Main View Layout (1200x800)

```
┌────────────────────────────────────────────────────────────────────────────────┐
│ TITLE BAR: [M] MorphSnap v2  [PLUGIN NAME]  [Load][Show]  [●][▤][◈][☰][⚙]   │
├────────────────────────────────────────────────────────────────────────────────┤
│ BROWSER BAR: [Load Plugin...] [Show Editor] [Capture] Plugin: Serum          │
├──────────┬────────────────────────────────────────┬───────────────────────────┤
│ LEFT     │         MORPH PAD AREA                 │ RIGHT PANEL (240px)       │
│ RAIL     │                                        │                           │
│ (52px)   │   ╔════════════════════════════╗       │ SNAPSHOT GRID (3x4)       │
│          │   ║  12 snapshots in clock     ║       │ ┌──┬──┬──┬──┐            │
│ SNAP     │   ║  layout with trails,       ║       │ │1●│2○│3●│4○│            │
│ FADER    │   ║  heatmap overlay,          ║       │ └──┴──┴──┴──┘            │
│          │   ║  grid crosshairs           ║       │ ┌──┬──┬──┬──┐            │
│          │   ║            ◎ cursor        ║       │ │5○│6●│7○│8●│            │
│ PHYSICS  │   ╚════════════════════════════╝       │ └──┴──┴──┴──┘            │
│ MODE     │                                        │ ┌──┬──┬──┬──┐            │
│ ICONS    │   [● Direct] [~ Elastic] [~ Drift]    │ │9●│10│11│12│            │
│          │   Smooth: ──●──  Tension: ──●──        │ └──┴──┴──┴──┘            │
│          │                                        │                           │
│          │   MACRO KNOBS (8 rotaries)             │ WAVEFORM MINI             │
│          │   ○1  ○2  ○3  ○4  ○5  ○6  ○7  ○8      │ ┌──────────────┐         │
│          │   Cutoff Res Env LFO Att Dec Sus Rel   │ │input ─┐ ┌─   │         │
│          ├────────────────────────────────────────┤ │output ─┘ └─   │         │
│          │ BOTTOM: [Sanity][Listen][Sustain][Link]│ └──────────────┘         │
│          │ [Fast|Full] SC:─●─ [Breed][Mutate]    │ AI: MCP ● ON :30001      │
└──────────┴────────────────────────────────────────┴───────────────────────────┘
```

Key changes from v3.3.0:
- `SnapshotRing` moves to dedicated right-panel 3x4 grid with right-click menus
- Mini waveform display (input vs output) in right panel
- `ModeBar` physics toggles become vertical icon buttons in left rail
- `BreedingPanel` buttons relocate to bottom strip

### 3.5 Spectral View

```
┌───────────────────────────────────────────┬────────────────────────────────┐
│  SPECTROGRAM (2/3 width)                  │ SPECTRUM ANALYZER (1/3)        │
│  20kHz ────────────────────────────       │  0dB ─┐           ┌─          │
│         [scrolling spectrogram]           │ -24   └──────────┘            │
│         [morph cursor line overlay]       │ -48    [A overlay] [B overlay]│
│  20Hz ─────────────────────────────       │       20  100  1k  5k  20k   │
│       TIME → (15 sec history)             │                                │
│                                           │  MINI MORPH PAD (120x120)     │
│  [3D Waterfall toggle]                    │  [read-only cursor display]   │
└───────────────────────────────────────────┴────────────────────────────────┘
```

Implementation: `juce::Image` offscreen render, shifted 1px per 30Hz timer tick, column coloured cyan→coral→white by magnitude. No OpenGL required.

### 3.6 Modulation View

```
┌───────────────────┬──────────────────────────────┬──────────────────────┐
│ SOURCES (180px)   │  ROUTING CANVAS (center)     │ TARGETS (200px)      │
│                   │                              │                      │
│ ◉ Morph X ─────►  │  [animated bezier cables     │ [●] Cutoff Freq     │
│ ◉ Morph Y ─────►  │   color-coded by source:     │ [●] Resonance       │
│ ○ Macro 1         │   coral=morph, violet=macro   │ [ ] Env Attack      │
│ ○ Macro 2 ────►   │   cyan=sidechain, amber=MIDI]│ [●] LFO Rate        │
│ ...               │                              │ [ ] Pitch            │
│ ○ Sidechain       │  [drag source→target to      │ ...                  │
│ ○ MIDI CC 1       │   create connection]          │ [Search params...]   │
└───────────────────┴──────────────────────────────┴──────────────────────┘
```

Cables drawn as cubic Bezier splines in `paint()`. Drag-to-connect interaction. Right-click connections for depth/invert/delete.

### 3.7 Preset Browser

```
┌────────────────┬─────────────────────────────────┬──────────────────────┐
│ CATEGORY TREE  │ PRESET LIST (scrollable)        │ PREVIEW PANEL        │
│ (180px)        │                                 │ (280px)              │
│ ▼ Factory      │ NAME           TAGS    RATING   │ [PRESET NAME]        │
│   ▶ Pads       │ ● Glacial      pad    ★★★★☆   │ by: Author           │
│   ▶ Bass       │ ● Deep Rumble  bass   ★★★☆☆   │                      │
│   ▶ Leads      │ ● Harmonic     lead   ★★★★★   │ SNAPSHOT THUMBNAILS  │
│ ▼ User         │                                 │ [color gradient bars]│
│   ▶ My Presets │ [Load More] [Sort: Name▼]      │                      │
│ ▶ Cloud        │                                 │ [Load] [Preview] [★]│
└────────────────┴─────────────────────────────────┴──────────────────────┘
```

### 3.8 Morph Pad 2.0 Enhancements

- **Path Record/Replay**: Record cursor movements as `std::vector<TimedPoint>`, replay via timer
- **Heatmap Mode**: 32x32 grid with exponential decay, coloured cold→violet→coral
- **Snap-to-Grid**: Quantize mouse position to 1/8 divisions
- **Right-Click Context Menu**: Capture, clear trail, toggle grid/path, switch vis mode
- **Touch Support**: `mouseMagnify` for pinch-zoom sensitivity, `mouseWheelMove` for clock rotation

### 3.9 Keyboard Shortcuts

```
Space         → Toggle record/play on MorphPad
1-9, 0, -, = → Recall snapshots 1-12
Shift+1..=   → Capture to slots 1-12
Ctrl+Z/Y     → Undo/Redo learn assignments
Ctrl+B       → Breed (Slots A+B)
Ctrl+L       → Toggle Listen Mode
Tab          → Cycle views
F1           → Shortcut reference overlay
```

### 3.10 Size Presets

| Mode | Size | Notes |
|------|------|-------|
| Compact | 920x590 | Current default, small screens |
| Standard | 1200x800 | V2 default |
| Large | 1440x960 | |
| Full HD | 1600x1080 | Max |

### 3.11 Component Hierarchy

```
MorphSnapEditor
├── ViewContainer
│   ├── MainView
│   │   ├── BrowserBar
│   │   ├── LeftRail (SnapFader + PhysicsModeRail)
│   │   ├── MorphPadArea (MorphPad + SnapshotRing)
│   │   ├── SnapshotSlotGrid[12]          ← NEW
│   │   ├── WaveformMiniDisplay           ← NEW
│   │   ├── MacroKnobStrip
│   │   ├── ModeBar
│   │   └── BottomControlStrip
│   ├── SpectralView                      ← NEW
│   │   ├── SpectrogramDisplay
│   │   ├── SpectrumAnalyzer
│   │   └── MorphPadMini
│   ├── ModulationView                    ← NEW
│   │   ├── ModSourceList
│   │   ├── RoutingCanvas
│   │   └── ModTargetList
│   ├── PresetBrowser                     ← NEW
│   │   ├── SearchBar
│   │   ├── CategoryTree
│   │   ├── PresetList (PresetRow[N])
│   │   └── PresetPreviewPanel
│   └── AdvancedPanel
│       ├── LearnModeSubView (existing ParameterMapPanel + LearnModePanel)
│       ├── GeneticsSubView (expanded BreedingPanel)
│       ├── AISubView (existing AIStatusPanel, full-size)
│       └── PerformanceSubView            ← NEW
├── HostedPluginWindow (floating)
└── KeyboardShortcutOverlay
```

---

## 4. Parameter Modulation System

### 4.1 Architectural Overview

The modulation system sits between `MorphProcessor` output and `ParameterBridge`. It is a per-sample transform: reads source values, accumulates into destination sums, adds to base parameter values.

```
processBlock() per sample:
  MorphProcessor → morphOutput[0..N]
  ModulationEngine → modifies morphOutput in-place
  DiscreteParameterHandler → snap discrete params
  ParameterBridge → apply to hosted plugin
```

### 4.2 Core Data Structures

#### Destination Address

```cpp
enum class ModDstKind : uint8_t {
    HostedParam = 0,   // Index into hosted plugin [0..2047]
    MorphX = 1, MorphY = 2, FaderPos = 3,
    DriftSpeed = 4, DriftDistance = 5, DriftChaos = 6,
    SmoothingRate = 7, MacroKnob = 8
};

struct ModDestination {
    ModDstKind kind = ModDstKind::HostedParam;
    uint16_t index = 0;
};
```

#### Source Identity

```cpp
enum class ModSrcKind : uint8_t {
    LFO = 0, Envelope = 1, Macro = 2, StepSeq = 3,
    Random = 4, MorphX = 5, MorphY = 6, MorphDist = 7,
    MidiVelocity = 8, MidiAftertouch = 9, MidiPitchBend = 10, MidiCC = 11
};

struct ModSource {
    ModSrcKind kind = ModSrcKind::LFO;
    uint8_t index = 0;
};
```

#### Routing Slot (16 bytes, trivially copyable)

```cpp
struct alignas(4) ModulationSlot {
    ModSource src;           // 2 bytes
    ModDestination dst;      // 3 bytes
    CurveShape curve;        // Linear/Exponential/Logarithmic/SCurve/Sine
    uint8_t polarity;        // 0=unipolar [0,1], 1=bipolar [-1,1]
    uint8_t enabled;
    float depth;             // [-1.0 .. +1.0]
    float offset;            // [-1.0 .. +1.0]
    uint8_t _pad[4]{};
};
static_assert(sizeof(ModulationSlot) == 16);
static_assert(std::is_trivially_copyable_v<ModulationSlot>);
```

### 4.3 ModulationMatrix Class

```cpp
class ModulationMatrix {
public:
    // Message thread: add/remove/modify routing slots
    int  addSlot(const ModulationSlot& s);      // returns slot index or -1
    bool removeSlot(int slotIndex);
    bool modifySlot(int slotIndex, const ModulationSlot& s);

    // Audio thread: drain command queue, then process per-sample
    void syncFromCommands() noexcept;
    void processSample(std::span<float> morphOutput, float sampleRate) noexcept;

private:
    static constexpr int MAX_MOD_SLOTS = 128;
    std::array<ModulationSlot, MAX_MOD_SLOTS> audioSlots_{};
    std::atomic<int> activeSlotCount_{0};

    // Command queue: same SPSC pattern as existing commandQueue
    LockFreeQueue<ModMatrixCommand, 256> cmdQueue_;
    std::mutex cmdQueueProducerMutex_;

    // Source value table: 160 floats (LFOs, envs, macros, MIDI, morph position)
    SourceValueTable srcTable_;

    // Per-destination accumulator (2080 floats = 8.3 KB on stack, within 4MB limit)
    std::array<float, 2080> modSum_{};
};
```

### 4.4 Modulation Sources

#### LFO (4 instances)

```cpp
struct LFOConfig {
    LFOShape shape;          // Sine/Triangle/SawUp/SawDown/Square/SampleHold/Custom
    uint8_t tempoSync;       // 0=free Hz, 1=beat fraction
    uint8_t retrigger;       // reset phase on note-on
    float rateHz;
    float beatDivision;
    float phaseOffset;       // [0..1]
    float symmetry;          // pulse width / triangle skew
};

class LFO {
public:
    void prepare(double sampleRate) noexcept;
    float tick(double bpm) noexcept;  // returns [-1..1]
    void applyConfig(const LFOConfig& cfg) noexcept;
    void retrigger() noexcept;
private:
    double phase_ = 0.0;
    std::array<float, 64> customTable_{};  // user-drawable waveform
};
```

#### Envelope Follower (2 instances)

```cpp
class EnvelopeFollower {
public:
    void prepare(double sampleRate) noexcept;
    float processSample(float inputSample) noexcept;  // returns [0..1]
    // Sidechain input support via processBlock sidechain buffer
private:
    float attackCoeff_, releaseCoeff_, envelope_ = 0.0f;
};
```

#### Macro Knobs (expanded 8 → 16)

```cpp
class MacroKnob {
public:
    void setTarget(float v) noexcept;         // message thread, atomic store
    float tick() noexcept;                     // audio thread, slew-smoothed
    void setMidiCC(int cc) noexcept;          // MIDI learn
    void setSlewMs(float ms) noexcept;        // smoothing rate
private:
    std::atomic<float> target_{0.0f};
    float current_ = 0.0f, slewCoeff_ = 1.0f;
};
```

#### Step Sequencer (2 instances)

Up to 64 steps, variable step length, swing/groove, tempo sync with DAW BPM.

#### Drift Random (2 instances)

Perlin noise-based smooth random with speed, depth, bias, and seed controls.

#### MIDI Sources

Velocity, aftertouch, pitch bend, any CC → modulation (set from MIDIRouter callbacks).

#### XY Pad Position

Morph X, Morph Y, and distance-from-center as modulation sources.

### 4.5 ModulationEngine (Top-Level Owner)

```cpp
class ModulationEngine {
public:
    void prepare(double sampleRate, int maxBlockSize) noexcept;

    // Audio thread entry point — modifies morphOutput in-place
    void processBlock(std::span<float> morphOutput,
                      const juce::AudioBuffer<float>* sidechainBuffer,
                      int numSamples, float morphX, float morphY,
                      double bpm, double ppqPosition) noexcept;

    ModulationMatrix& getMatrix();

    // Message thread configuration
    void configureLFO(int index, const LFOConfig& cfg);
    void configureEnvFollower(int index, const EnvFollowerConfig& cfg);
    void setMacroValue(int index, float v);

    // MIDI inputs
    void onNoteOn(int note, float velocity) noexcept;
    void onCC(int cc, float v) noexcept;

    juce::var toVar() const;     // serialisation
    bool fromVar(const juce::var& v);

private:
    ModulationMatrix matrix_;
    std::array<LFO, 4> lfos_;
    std::array<EnvelopeFollower, 2> envFollowers_;
    std::array<MacroKnob, 16> macros_;
    std::array<StepSequencer, 2> stepSeqs_;
    std::array<DriftRandom, 2> drifts_;
};
```

### 4.6 Thread Safety Summary

| Operation | Thread | Mechanism |
|-----------|--------|-----------|
| `addSlot/removeSlot/modifySlot` | Message | `cmdQueue_.push()` behind `cmdQueueProducerMutex_` |
| `syncFromCommands` (apply) | Audio | `cmdQueue_.pop()` — SPSC, no lock |
| `processSample` | Audio | Reads `audioSlots_[]` copy, no lock |
| `MacroKnob::setTarget` | Message | Atomic store |
| `MacroKnob::tick` | Audio | Atomic load + slew |
| MIDI source writes | Audio | Direct writes to `srcTable_` — single writer |
| `toVar/fromVar` | Message | No audio access |

---

## 5. Preset System Architecture

### 5.1 Preset Format v2 (JSON)

```json
{
  "format_version": 2,
  "metadata": {
    "name": "Pad Morph 01",
    "author": "Jane Smith",
    "description": "Warm pad morphing between filter states",
    "uuid": "550e8400-e29b-41d4-a716-446655440000",
    "created_at": "2026-02-26T10:00:00Z",
    "modified_at": "2026-02-26T12:30:00Z",
    "rating": 4,
    "tags": ["pad", "ambient", "filter"],
    "genre": ["electronic", "ambient"],
    "instrument_type": "synthesizer",
    "required_plugin": {
      "name": "Serum", "manufacturer": "Xfer Records",
      "format": "VST3", "uid_hex": "58666572536D61"
    },
    "morphsnap_version_min": "2.0.0"
  },
  "apvts": "<APVTS ValueTree XML>",
  "snapshots": [
    {
      "slot": 0, "name": "Bright", "occupied": true,
      "param_count": 312,
      "values_b64": "<base64 float32 LE>",
      "state_chunk_b64": "<base64 VST3 opaque state>"
    }
  ],
  "modulation": {
    "slots": [
      { "src": {"kind":"LFO","index":0}, "dst": {"kind":"HostedParam","index":42},
        "depth": 0.35, "curve": "Linear", "polarity": "bipolar" }
    ],
    "lfos": [ { "shape":"Sine", "rate_hz":0.5, "tempo_sync":false } ],
    "envelope_followers": [ { "attack_ms":10, "release_ms":100 } ],
    "macros": [ { "name":"Filter Cutoff", "value":0.6, "midi_cc":74 } ],
    "step_sequencers": [ { "num_steps":16, "step_length_beats":0.25, "swing":0.1 } ],
    "drift_randoms": [ { "speed":0.3, "depth":0.5, "seed":42 } ]
  },
  "physics": { "mode":"Elastic", "elastic_preset":1 },
  "ui_state": { "morph_x":0.5, "morph_y":0.5, "view_mode":"MorphPad" },
  "sanity_config": { "enabled":true, "protected_indices":[0,1,127] },
  "ai_analysis": { "token_budget":4096, "exposed_param_indices":[0,12,42] }
}
```

File extension: `.morphsnap`

### 5.2 PresetSerializer v2

```cpp
class PresetSerializer {
public:
    static constexpr int CURRENT_FORMAT_VERSION = 2;

    static juce::var serialize(const SnapshotBank&, APVTS&,
                               const ModulationEngine&, const MorphSnapProcessor&);
    static DeserialiseResult deserialize(const juce::var&, SnapshotBank&, APVTS&,
                                         ModulationEngine&, MorphSnapProcessor&);

    // V1 → V2 migration
    static bool upgradeV1ToV2(juce::var& presetVar);

    // Structural validation (no JSON Schema dependency)
    static bool validate(const juce::var& preset, juce::String& errorOut);
};
```

Defensive restoration: each subsystem (snapshots, modulation, physics, UI) restored independently. Failure in one subsystem logs a warning but doesn't abort others.

### 5.3 Preset Library System

```cpp
class PresetLibrary {
public:
    void initialise(const juce::File& factoryDir, const juce::File& userDir);
    void rescan(std::function<void(int progress, int total)> onProgress = {});

    std::vector<PresetMetadata> search(const PresetQuery& q) const;
    std::vector<PresetMetadata> getRecent(int maxCount = 10) const;
    std::vector<PresetMetadata> getFavourites() const;

    juce::File savePreset(const juce::var& presetData, const PresetMetadata& meta);
    bool deletePreset(const juce::String& uuid);
    bool setFavourite(const juce::String& uuid, bool fav);
    bool setRating(const juce::String& uuid, int rating);

    juce::StringArray getAllTags() const;
    juce::StringArray getAllBanks() const;
};
```

Directory structure:
```
%APPDATA%\MorphSnap\Presets\
  Factory\
    Pads\, Leads\, FX\
  User\
    My Presets\, Packs\
  .library_index.json    ← ratings, favourites, recents
```

### 5.4 Cloud / Community API

```
Base: https://cloud.morphsnap.dev/api/v1
Auth: OAuth2 + PKCE

GET  /presets?q=<text>&tags=pad,ambient&sort=rating
GET  /presets/{uuid}/file     ← download .morphsnap
POST /presets                 ← upload (multipart)
POST /presets/{uuid}/ratings  ← { score: 1..5 }
POST /presets/{uuid}/fork     ← creates attributed copy
GET  /packs                   ← browse curated collections
```

In-plugin client uses `juce::URL` + `juce::ThreadPool{2}` for async I/O on message thread.

### 5.5 DAW State Integration

`getStateInformation` serializes: APVTS + snapshot bank (existing) + modulation engine (new, as JSON child element) + physics state. `setStateInformation` branches on `state_version` attribute (1 = legacy path, 2 = full restore including modulation).

### 5.6 Undo/Redo

```cpp
class PresetUndoManager {
public:
    static constexpr int MAX_UNDO_LEVELS = 32;

    void pushState(UndoCategory cat, const juce::var& before,
                   const juce::var& after, const juce::String& description);
    std::optional<UndoablePresetState> undo();
    std::optional<UndoablePresetState> redo();
};
```

Categories: SnapshotBank, ModulationSlot, MacroAssignment, PhysicsConfig, FullState.

---

## 6. Audio Engine Implementation

> Full detail: [`docs/AudioEngineSpec_v2.md`](AudioEngineSpec_v2.md)
> Implementation files: `src/Core/OversamplingWrapper.h`, `src/Core/LatencyManager.h`

### 6.1 Sample Rate & Bit Depth

All rates (44.1–192 kHz) use the same code paths. Physics engine uses time-delta `dt = numSamples / sampleRate`, not hard-coded timing.

**Internal precision**: float32 (24-bit dynamic range in [0,1] space, 2x SIMD throughput vs double). Use `double` only for time accumulators (`driftTimeAccum_`) to prevent clock drift over long sessions.

### 6.2 Buffer Size Management

Supported range: 32–8192 samples. All algorithms are block-size independent. For FFT spectral processing, an input ring buffer bridges the size mismatch between host blocks and FFT frames.

```
grainBufferSize = nextPowerOf2(4 * maxBlockSize + fftSize)
```

### 6.3 Oversampling Strategy

Production default: **x4 FIR** (linear-phase, ~100 dB stopband, suitable for 24-bit). Implementation: `OversamplingWrapper` wrapping `juce::dsp::Oversampling<float>`.

| Factor | CPU Overhead | Stopband | Use Case |
|--------|-------------|----------|----------|
| x1 | 0% | N/A | Direct mode, no audio processing |
| x2 | +130% | ~80 dB | Light saturation |
| x4 | +380% | ~100 dB | **Default** — production quality |
| x8 | +850% | ~120 dB | Mastering-grade |

**When to oversample**: Only nonlinear stages (spectral waveshaping). Linear operations (parameter interpolation, physics) never need oversampling.

### 6.4 Latency Compensation

```
totalLatency = oversamplingLatency + fftWindowLatency + hostedPluginLatency
```

Centralized in `LatencyManager` with `std::atomic<int>` fields. Call `setLatencySamples(latencyManager_.getTotal())` in `prepareToPlay()` and whenever hosted plugin or processing mode changes. Mode switches trigger `updateHostDisplay(ChangeDetails::withLatencyChanged())`.

### 6.5 CPU Architecture Optimizations

**Runtime SIMD dispatch** (single binary for all CPUs):

| Architecture | Register Width | Hot Loops |
|-------------|----------------|-----------|
| SSE2 (baseline x64) | 128-bit / 4 floats | Smoothing, IDW interpolation |
| AVX2 (modern x64) | 256-bit / 8 floats | Grain mixing, FFT windowing, spectral lerp |
| NEON (Apple Silicon) | 128-bit / 4 floats | Same as SSE2, different intrinsics |

**Cache-line alignment**: `alignas(64)` on hot arrays (`smoothedValues_`, `LockFreeQueue` indices) to prevent false sharing.

**Thread priority**: MCP thread set to below-normal (`juce::Thread::setCurrentThreadPriority(3)`) so audio thread always wins CPU.

### 6.6 Audio Quality Assurance

- **Denormal flush**: `juce::ScopedNoDenormals` as first line of `processBlock()`
- **DC blocking**: First-order high-pass (`R = 1 - 2*pi*10/sr`) for audio pass-through
- **TPDF dithering**: For sub-24-bit delivery contexts
- **Bypass**: Latency-compensated (maintains DAW delay compensation graph)
- **Level metering**: Peak hold with 3-second hold time, configurable release

---

## 7. Compatibility & Deployment

### 7.1 Plugin Format Support

| Format | Status | Notes |
|--------|--------|-------|
| VST3 | Required | Primary format, Windows + macOS |
| AU | Required | macOS only (`JUCE_BUILD_AU` gated on `APPLE`) |
| CLAP | Planned | Via JUCE CLAP extensions; growing DAW support (Bitwig, Reaper) |
| AAX | Evaluate | Requires Avid developer agreement and signing; Pro Tools only |

JUCE 8 builds all formats from a single `juce_add_plugin()` declaration. Format-specific features:
- **VST3**: Note expression support for polyphonic morph control
- **AU**: Parameter tree auto-generated from APVTS
- **CLAP**: Polyphonic modulation, lower latency event handling

### 7.2 Platform Matrix

| Platform | Architecture | Status | Compiler |
|----------|-------------|--------|----------|
| Windows 10+ | x64 | Required | MSVC 2022 (v143) |
| Windows 11 | ARM64 | Planned | MSVC ARM64 toolchain |
| macOS 11+ | Universal (x64 + arm64) | Required | Apple Clang 14+ |
| Linux | x64 | Experimental | GCC 11+ / Clang 14+ (CLAP only) |

Windows: `/STACK:4194304` for FL Studio compatibility (existing). macOS: Universal binary via `CMAKE_OSX_ARCHITECTURES="x86_64;arm64"`.

### 7.3 DAW Compatibility Testing Matrix

| DAW | Windows | macOS | Priority | Known Issues |
|-----|---------|-------|----------|--------------|
| Ableton Live 12 | VST3 | VST3/AU | P0 | |
| FL Studio 24 | VST3 | — | P0 | 4MB stack required |
| Logic Pro X | — | AU | P0 | |
| Reaper 7 | VST3/CLAP | VST3/AU/CLAP | P0 | |
| Cubase 14 | VST3 | VST3 | P1 | |
| Studio One 7 | VST3 | VST3/AU | P1 | |
| Bitwig 5 | VST3/CLAP | VST3/CLAP | P1 | |
| Pro Tools | AAX | AAX | P2 | Requires AAX SDK |

### 7.4 Installer & Distribution

- **Windows**: NSIS or WiX installer copying `MorphSnap.vst3` to `C:\Program Files\Common Files\VST3\`
- **macOS**: `.pkg` installer for `/Library/Audio/Plug-Ins/VST3/` and `/Library/Audio/Plug-Ins/Components/`
- **Validation**: pluginval strictness 5 in CI before release
- **Code signing**: Windows Authenticode + macOS notarization via `xcrun notarytool`

### 7.5 V1 → V2 Migration Path

- V1 presets (XML format) load via `PresetSerializer::upgradeV1ToV2()` automatic migration
- `setStateInformation` checks `state_version` attribute — branches to legacy path for v1
- Modulation engine initializes to default (no active routings) when loading v1 presets
- Snapshot data format unchanged (base64 float arrays) — zero data loss

---

## 8. Development Workflow & Build Process

> Full detail: [`docs/AudioEngineSpec_v2.md`](AudioEngineSpec_v2.md)
> Implementation files: `CMakePresets.json`, `.github/workflows/ci.yml`, `.clang-tidy`

### 8.1 Build System

**CMake Presets** (`CMakePresets.json`) — 10 named presets:

```bash
# Windows MSVC Release
cmake --preset windows-msvc-release
cmake --build --preset windows-release

# macOS Universal Binary (x86_64 + arm64)
cmake --preset macos-universal-release
cmake --build --preset macos-release

# Linux Clang with ASAN/UBSAN
cmake --preset linux-clang-asan
cmake --build build/linux-clang-asan
```

**New v2 dependencies** (FetchContent):
- **pffft** — FFT library (public domain, ~3x faster than JUCE FFT)
- **Eigen** — Header-only linear algebra for spectral bin manipulation
- **ONNX Runtime** — Pre-built binaries (never build from source — takes 45+ min)

### 8.2 CI/CD Pipeline (`.github/workflows/ci.yml`)

| Job | Platform | Tools |
|-----|----------|-------|
| Windows x64 | `windows-2022` | MSVC 2022, FetchContent cache |
| macOS Universal | `macos-14` (Apple Silicon) | Clang, universal binary `lipo` verification |
| ASAN/UBSAN | Linux Clang 17 | Address + undefined behavior sanitizers |
| pluginval | Both platforms | Strictness level 5 |
| Release | On `v*` tags | GitHub Release with prerelease detection |

FetchContent cache keyed on `CMakeLists.txt` hash. Concurrency group with `cancel-in-progress`.

### 8.3 Testing Methodology

**Unit Tests** (Catch2 v3 — 42+ test cases across 7 files):
- DSP correctness: interpolation within 1e-5f of scalar reference
- Smoothing convergence: within 1% of target after 200 steps at rate=0.95
- SNR > 80 dB for x4 FIR round-trip on 1 kHz sine
- Aliasing detection: no energy above 22 kHz after x4 oversampling
- Buffer-size independence: same output for 32–1024 sample blocks

**Integration Tests**: Plugin lifecycle, state save/restore round-trip, DAW simulation.

**Audio Quality Tests**: SNR measurement, aliasing detection, latency verification, monotonicity.

**Fuzzing** (libFuzzer):
- `FuzzPresetParser.cpp` — feed arbitrary bytes to `setStateInformation`
- `FuzzMCPHandler.cpp` — feed arbitrary JSON to MCP handler

**Thread Safety**: TSan (Thread Sanitizer) on Linux Clang.

### 8.4 Code Quality

**`.clang-tidy`** configuration with 13 error-promoted checks targeting RT-unsafe and data-race-prone patterns. Key rules:
- No `new`/`delete`/`malloc`/`free` in `noexcept` audio path
- No `std::vector::push_back`, `std::string`, `juce::String` construction on audio thread
- No `std::mutex::lock()` on audio thread
- Existing `AllocationTracker.h` provides runtime assertion in debug builds

**Coverage targets**: Core DSP > 90%, GeneticEngine > 95%, MCP > 70%, UI > 50%.

### 8.5 Version Control Strategy

```
main (or mvp)    Production-ready releases only. Protected.
develop          Integration branch. Features merge here first.
feature/*        One feature per branch. Rebased onto develop.
release/v3.4.0   Short-lived stabilization.
hotfix/*         Critical fixes. Branch from main, PR to both.
```

**Semantic versioning**: MAJOR (breaking preset/API change), MINOR (new backward-compatible feature), PATCH (bug fix only).

**Changelog**: `git cliff` with conventional commits. **Artifacts**: `MorphSnap-{version}-{platform}.zip`.

---

## Implementation Sequencing

### Phase 1 — Foundation (no visible user changes)
1. `ModulationTypes.h` — pure header types
2. `ModulationSource.h/.cpp` — LFO, EnvFollower, MacroKnob, StepSeq, DriftRandom
3. `ModulationMatrix.h/.cpp` — seqlock pattern from SnapshotBank
4. `ModulationEngine.h/.cpp` — wire into processBlock
5. Unit tests for each source type and matrix routing

### Phase 2 — UI Views
6. Extend `MorphSnapLookAndFeel` with 9 new color tokens
7. Add `ViewSwitcher` tab strip to title bar
8. Refactor `PluginEditor::resized()` for multi-view routing
9. Implement `SnapshotSlotGrid`, `WaveformMiniDisplay`, `ParameterDeltaStrip`
10. Implement MorphPad heatmap/timeline/record modes

### Phase 3 — Spectral & Granular Engines
11. `SpectralMorphEngine` with PFFFT integration
12. `GranularMorphEngine` with grain pool
13. `FormantMorphEngine` with cepstral processing
14. Dual-plugin hosting (`PluginHostManagerB`)
15. Hybrid blend controller

### Phase 4 — Preset System
16. `PresetSerializer` v2 with migration from v1
17. `PresetLibrary` with filesystem scanning and search
18. `PresetBrowser` UI with Viewport-based list
19. Updated `getStateInformation`/`setStateInformation`

### Phase 5 — Visualization
20. `SpectrogramDisplay` with offscreen image rendering
21. `SpectrumAnalyzer` using JUCE dsp::FFT
22. `RoutingCanvas` with bezier cable renderer

### Phase 6 — ML/AI
23. VAE model training pipeline (offline, Python)
24. ONNX Runtime integration for inference
25. Sound matching feature (Nelder-Mead + feature grid)
26. Generative breeding with ML fitness

### Phase 7 — Cloud & Polish
27. `CloudPresetClient` with REST API
28. Undo/redo system
29. Dark/light theme toggle
30. Performance profiling and SIMD optimization pass
