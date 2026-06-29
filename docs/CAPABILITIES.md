# More-Phi — Plugin Capabilities

> **Version:** 3.4.1 · **Format:** VST3 / AU (macOS) · **Framework:** JUCE 8 · **Language:** C++20

More-Phi is a next-generation audio plugin that **hosts other VST3/AU plugins and morphs between their parameter snapshots** using physics-based interpolation, genetic breeding, spectral and granular crossfading, AI-driven mastering, and an embedded multi-agent orchestration layer.

---

## Table of Contents

1. [Plugin Hosting](#1-plugin-hosting)
2. [Snapshot Morphing](#2-snapshot-morphing)
3. [Physics-Based Motion](#3-physics-based-motion)
4. [Genetic Breeding](#4-genetic-breeding)
5. [Audio-Domain Morphing](#5-audio-domain-morphing)
6. [Modulation System](#6-modulation-system)
7. [Neural Mastering](#7-neural-mastering)
8. [Built-in Mastering Chain](#8-built-in-mastering-chain)
9. [Multi-Agent AI Orchestration](#9-multi-agent-ai-orchestration)
10. [MCP Server & Remote Control](#10-mcp-server--remote-control)
11. [LLM Integration](#11-llm-integration)
12. [MIDI Control](#12-midi-control)
13. [Preset & State Management](#13-preset--state-management)
14. [Analysis & Metering](#14-analysis--metering)
15. [Learn Mode & Parameter Intelligence](#15-learn-mode--parameter-intelligence)
16. [Plugin Profile & Semantic Mapping](#16-plugin-profile--semantic-mapping)
17. [Real-Time Safety](#17-real-time-safety)
18. [Undo / Redo / A/B Compare](#18-undo--redo--ab-compare)
19. [Cross-Plugin IPC](#19-cross-plugin-ipc)
20. [Dataset Generation](#20-dataset-generation)
21. [Licensing & Security](#21-licensing--security)
22. [Performance Optimization](#22-performance-optimization)
23. [User Interface](#23-user-interface)

---

## 1. Plugin Hosting

More-Phi acts as a **plugin-in-plugin host**, loading any VST3/AU plugin and controlling its parameters in real time.

| Feature | Description |
|---------|-------------|
| **Plugin loading** | Load any VST3/AU plugin via scan or direct path; Timer-based deferred loading handles DAW threading constraints |
| **Parameter bridge** | Up to **4,096 parameters** per hosted plugin, mapped to a normalized float vector `[0, 1]` |
| **Parameter read/write** | Thread-safe parameter access via `acquirePluginForUse()`/`releasePluginFromUse()` with ref-counted safety |
| **Plugin health monitor** | `PluginHealthMonitor` watches for plugin crashes and reports status |
| **DAW transport sync** | `DAWTransportForwarder` provides JUCE-8 seqlock snapshot bus for accurate transport state |
| **Touch detection** | Per-parameter cooldown counters (~200 ms) prevent morph from overwriting manual knob changes; interleaved sampling reduces CPU cost by ~75% |
| **Discrete parameter handling** | `DiscreteParameterHandler` snaps discrete/binary parameters to valid steps during morphing instead of interpolating through invalid intermediates |
| **State persistence** | Full hosted plugin state serialized (including opaque VST3 state chunk) and restored on session reload with retry logic |

---

## 2. Snapshot Morphing

The core of More-Phi — capture up to **12 parameter snapshots** of the hosted plugin and smoothly morph between them.

| Feature | Description |
|---------|-------------|
| **12 snapshot slots** | Each stores 4,096 parameters as a `ParameterState` with per-param metadata (name, value, type) |
| **XY morph pad** | 2D pad (X/Y coordinates) drives interpolation weight across snapshots placed in 2D space |
| **Snap fader** | 1D fader for linear morphing between two snapshots |
| **Snapshot ring** | Circular visual for quick snapshot selection and comparison |
| **Voronoi morph** | Delaunay triangulation + barycentric interpolation on the XY pad — the three nearest snapshots blend with mass-weighted interpolation; out-of-hull positions fall back to IDW |
| **Physics modes** | Direct (raw cursor), Elastic (spring-damper with overshoot), Drift (Perlin noise wander) |
| **MIDI triggering** | Notes C3–B3 recall snapshots; CC1 drives fader position |
| **DAW program interface** | All 12 slots exposed as VST3 "programs" for DAW preset browser integration |
| **Smart randomize** | Randomize only user-learned parameters with configurable amount |
| **Undo/Redo** | 32-level undo/redo for snapshot changes |
| **Meta-presets** | `MetaPresetManager` and `PresetSerializer` save/load preset libraries with snapshot banks |

---

## 3. Physics-Based Motion

Three physics modes animate morph transitions with natural-feeling motion.

### Elastic Mode
Spring-damper system with position + velocity. Three presets:
- **Slow** — gentle, over-damped response
- **Medium** — balanced spring feel
- **Heavy** — high inertia with pronounced overshoot

### Drift Mode
Perlin noise-driven wandering around an anchor point. Three modes:
- **Free** — unbounded random walk
- **Locked** — stays near anchor, gentle wander
- **Orbit** — circular orbit around anchor with variable radius

Controls: **Speed**, **Distance**, **Chaos**

### Waypoint Sequencer
BPM-synced XY animation through up to **16 waypoints** — each with configurable hold and transition beat counts. Automates the morph pad for evolving textures.

---

## 4. Genetic Breeding

`GeneticEngine::breed()` crosses two snapshots to create a new child:

| Feature | Description |
|---------|-------------|
| **Crossover** | Configurable ratio `[0 = Parent A, 1 = Parent B]` per parameter |
| **Mutation** | Random perturbation with configurable strength |
| **Sanity protection** | Dangerous parameters (Volume, Pitch, Bypass, OutputGain) are never modified during breed/randomize |
| **Smart randomize** | Only mutates user-learned parameters, respects protected indices |

---

## 5. Audio-Domain Morphing

Beyond parameter interpolation, More-Phi morphs the **actual audio output** through three complementary engines, blended with equal-power mixing.

### Spectral Morph Engine
STFT-based phase-vocoder interpolation between two plugin outputs:
- **Log-magnitude geometric mean** for perceptually linear loudness blending
- **Instantaneous-frequency interpolation** for phase continuity
- Default FFT 2048, hop 512 (75% overlap), Hann window
- Transient preservation option (gates phase reset on detected transients)
- Formant preservation option (see below)
- ~58 ms latency @ 44.1 kHz

### Formant Morph Engine
Cepstral-liftering formant preservation:
- Extracts spectral envelope via FFT → real cepstrum → liftering
- Transplants source formant structure onto morphed audio
- Configurable preservation amount (0–1)
- Preserves vocal/instrument character during spectral morphing

### Granular Morph Engine
Grain-based crossfade between two audio streams:
- Circular buffers capture ~1.4 s of each source @ 48 kHz
- Per-grain amplitude, pitch, and position randomization
- Morph alpha controls A/B grain blend
- Configurable grain size, density, and randomness
- xorshift32 PRNG — no heap allocation on audio thread

### Hybrid Blend
Equal-power mixing of all three outputs (parameter-domain, spectral, granular) with automatic weight normalization.

---

## 6. Modulation System

A full modulation system with 30+ sources, 128 routes, and audio-thread-safe double-buffer updates.

### Sources (26 total)

| Category | Sources |
|----------|---------|
| **LFOs** | 4 independent LFOs (Sine, Triangle, Saw, Square, Sample & Hold, Noise) |
| **Envelopes** | 2 audio envelope followers |
| **Macros** | 16 macro knobs (UI-controllable) |
| **Step Sequencers** | 2 clock-driven sequencers (Forward, Backward, PingPong, Random) |
| **Drift** | 2 drift random sources |
| **Morph** | X position, Y position, fader position |
| **MIDI** | Velocity, Aftertouch, Mod Wheel |

### Features
- **LFO tempo sync** — sync to DAW BPM with selectable divisions (whole, half, quarter, etc.)
- **Step sequencers** — 32 steps, configurable direction, smoothing from hard-step to fully smooth
- **Modulation matrix** — up to **128 routes**, each mapping any source to any parameter with depth and bipolar/unipolar mode
- **Double-buffer pattern** — message-thread edits are swapped atomically to the audio thread with `memory_order_release/acquire`
- **Pipeline position** — modulation is applied AFTER morph computation, BEFORE discrete snapping and parameter bridge output

---

## 7. Neural Mastering

AI-powered mastering using an embedded ONNX neural network that analyzes audio and generates mastering decisions.

### SonicMaster Analysis Engine
- Captures a **5.94 s** audio window (~262,138 frames @ 44.1 kHz)
- Runs inference on a **3 s** cycle on a background thread
- **Rate-proportional capture ring** — size scales with sample rate (4 MiB @ 44.1 kHz, 16 MiB @ 192 kHz)
- **Transition guard** — discards windows that straddle parameter changes; 0.5 s settle period for plugin tails

### ONNX Decision Model
- Input: raw audio waveform
- Output: 44-float decision vector decoded into:
  - **8-band EQ gains** (±12 dB)
  - **Target LUFS**
  - **True-peak ceiling**
  - **3-band compressor settings**
  - **Stereo width**
  - **Limiter settings**
  - **Character/saturation amount**

### Genre-Conditioned Priors
Two priors fold genre information into the decode path:
1. **Target LUFS prior** — `GenreMasteringProfile` maps 12 genres to target LUFS values
2. **Tonal balance residual** — `TonalBalanceExtractor` integrates spectrum into 8 EQ bands and blends corrections

### Genre Classifier
- **Built-in time-domain heuristic** — active out of the box (low/high band split + zero-crossing rate)
- **Pluggable neural model** — drop `genre_classifier.onnx` into `%APPDATA%/MorePhi/models/`
- 12 genre slots; confidence-scaled prior application

### Decision Decoder
- Decodes the 44-float ONNX output into a structured mastering plan
- Closed-loop feedback > genre prior > model default precedence
- Safety caps: per-cycle delta limits (~0.6 LU loudness, ±12 dB EQ)
- Read-back verification with `Applied` / `AppliedPartial` classification

### HTTP Fallback
- `SonicMasterHttpInferenceSource` provides REST inference when ONNX Runtime is unavailable

---

## 8. Built-in Mastering Chain

A full mastering signal chain built from modular DSP components (dormant by default; drives hosted plugins like Ozone when active).

### Components

| Component | Description |
|-----------|-------------|
| **Multiband Splitter** | 4-band crossover (Linkwitz-Riley) for frequency-domain processing |
| **Multiband Dynamics** | Per-band feedforward VCA compressor with stereo-linked peak detection, transient preservation |
| **Adaptive EQ** | 32-band parametric EQ with three AI control layers (LLM warm-start, spectral balance, genetic refinement) |
| **Transient Shaper** | Single-band transient/sustain shaper (3 ms fast / 150 ms slow RMS envelope ratio) |
| **Stereo Imager** | Frequency-dependent stereo width (M/S processing) with mono compatibility guard |
| **MS Matrix** | Mid/Side encode/decode for stereo processing |
| **Harmonic Exciter** | Soft-saturation exciter (1 kHz HP → drive → tanh) for presence and air |
| **Brickwall Limiter** | Lookahead (4 ms) true-peak limiter with ISP detection, default ceiling −1.0 dBTP |
| **True Peak Estimator** | Measurement-grade 4-phase × 64-tap polyphase FIR (±0.02 dBTP accuracy) |
| **LUFS Meter** | BS.1770-4 compliant loudness measurement (integrated, short-term, momentary, LRA) |
| **Loudness Normalizer** | Post-limiter LUFS normalization with ±12 dB correction range and 1 s smoothing |
| **Oversampling** | FIR anti-aliasing wrapper for nonlinear processing stages (×2, ×4, ×8, ×16) |
| **A/B Compare Engine** | Capture/checkpoint mastering state, compare LUFS/LRA metrics, auto-rollback on degradation |
| **Mono Compatibility Checker** | 10-band 1/3-octave check; auto-corrects by reducing side signal when cancellation > 3 dB |
| **Output Protection** | Final brickwall safety limiter at the output |

### Rule-Based Mastering Resolver
- Heuristic mastering plans from analysis inputs (genre, dynamic range, spectral tilt, stereo correlation)
- `MasteringTargetCurves` and `GenreMasteringProfile` provide genre-specific references

### Ozone Plan Applicator
- Drives hosted iZotope Ozone via positional parameter indices
- Name-validated parameter mapping survives plugin swaps gracefully
- Plan boundary markers for observability (applied vs. partially applied)

---

## 9. Multi-Agent AI Orchestration

A **7-agent orchestration layer** that coordinates autonomous audio processing tasks above the MCP tool layer.

### Agent Roster

| Agent | Role |
|-------|------|
| **ConductorAgent** | Goal decomposition via `WorkflowOrchestrator` + LLM; the ONLY agent that delegates follow-ups |
| **AnalysisAgent** | Audio analysis — spectrum, loudness, dynamics, stereo field |
| **OptimizationAgent** | Parameter optimization — brute-force/batch rendering, candidate scoring |
| **CreativeAgent** | Creative exploration — randomization, breeding, generative experiments |
| **RealtimeControlAgent** | Real-time corrections — writes through `LockFreeQueue`, never touches audio thread directly |
| **QualitySafetyAgent** | Quality and safety enforcement — validates plans, monitors output |
| **MemoryAgent** | Persistent memory — remembers user preferences, past decisions, patterns |

### Infrastructure

| Component | Description |
|-----------|-------------|
| **AgentRuntime** | Container for all agents; owns 2-worker thread pool |
| **AgentRegistry** | Registers and looks up agents by name/type |
| **PriorityScheduler** | 4-level priority queue (RealtimeCritical > High > Normal > Low) with O(1) operations and 1000 ms starvation guard |
| **BlackboardBridge** | Typed pub/sub over `IntegrationEventBus`; pump thread drains at 50 ms intervals |
| **DefaultToolInvoker** | Wraps `MCPToolHandler`; enforces per-agent capability scope (fail-closed) + rate budget + attribution |
| **Structured Agent Logger** | JSONL file logging with in-memory ring fallback |

### MCP Agent Tools
Seven dedicated MCP tools:
- `agents.list` — list registered agents
- `agents.run_goal` — submit a goal for ConductorAgent to decompose
- `agents.run_task` — submit a task to a specific agent
- `agents.run_status` — poll task progress
- `agents.run_cancel` — cancel a running task
- `agents.blackboard.recent` — read recent blackboard publications
- `agents.set_autonomy` — adjust agent autonomy level

---

## 10. MCP Server & Remote Control

An embedded **JSON-RPC 2.0** server on `localhost:30001` provides comprehensive remote control.

### Tool Categories (50+ tools)

**Hosted Plugin Control:**
- `scan_hosted_plugin`, `load_hosted_plugin`, `capture_hosted_state`
- `get_parameter`, `set_parameter`, `set_parameters_batch`
- `sweep_parameter` — autonomous parameter sweep with live measurement capture

**Morph & Snapshots:**
- `capture_snapshot`, `recall_snapshot`
- `set_morph_position`, `get_morph_state`

**More-Phi Parameters:**
- `list_more_phi_parameters`, `get_more_phi_parameter`, `set_more_phi_parameter`, `set_more_phi_parameters`

**Analysis:**
- `get_analysis_summary`, `get_spectrum_analysis`, `get_stereo_field_analysis`
- `capture_analysis_window`, `compare_analysis`

**Mastering:**
- `get_mastering_state`, `apply_mastering_plan`, `preview_mastering_plan`
- `analyze_rule_based_mastering`, `render_mastering_batch`, `select_mastering_candidate`
- `sonicmaster_decision` — neural mastering analysis (dry-run, no auto-apply)
- `mastering.neural_apply` — apply validated neural plan with safety checks

**Ozone Track Assistant:**
- `ozone.track.get_info`, `ozone.track.update_status`, `ozone.track.analyze`, `ozone.track.search`

**Plugin Intelligence:**
- `audit_plugin_profile`, `describe_plugin_semantics`, `get_plugin_profile`, `save_plugin_profile`
- `describe_plugin_semantic_map`, `apply_safe_plugin_action`, `restore_safe_plugin_snapshot`

**IPC Assistant:**
- `more_phi.ipc.attach`, `more_phi.ipc.detach`, `more_phi.ipc.status`
- `more_phi.ipc.snapshot`, `more_phi.ipc.dump`, `more_phi.ipc.capture`, `more_phi.ipc.run_assistant`

**Agent Orchestration:** (see [§9](#9-multi-agent-ai-orchestration))

**Instance Management:**
- `get_instance_info`, `list_instances`

**Async Operations:**
- `submit_async_tool`, `get_async_tool_status`, `get_async_tool_result`

**Diagnostics:**
- `run_self_test`, `diagnose_parameter_pipeline`

### Security
- **PermissionKernel** classifies every tool call (read, write, mutate, dangerous)
- **Action ledger** records all parameter changes (capacity 4,096 entries)
- **AutomationControlPlane** governs the MCP↔plugin boundary

---

## 11. LLM Integration

More-Phi connects to external LLM providers for AI-assisted audio production.

### Transport Layer
- **`RestLlmClient`** — production REST client implementing `ILlmClient` (OpenAI, Anthropic, OpenAI-compatible APIs)
- **`DeterministicFallbackLlmClient`** — offline heuristic (Analysis → Memory → Optimization with warm/bright/sparkle keyword flags)
- **`LLMChatClient`** — separate REST client driving the AI Chat Panel UI

### Configuration
- Provider API key + model selection in UI (`LLMSettingsDialog`)
- `LLMConnectionValidator` tests connectivity asynchronously (UI-panel-only)
- Selection gates on *configured*, not *validated* — a configured-but-invalid key fails lazily on first call

### Usage
- `ConductorAgent::decomposeGoal` calls LLM for goal decomposition
- AI Chat Panel provides conversational interaction with the plugin

---

## 12. MIDI Control

### MIDIRouter

| Mapping | MIDI | Action |
|---------|------|--------|
| **Note C3–B3** | Note On | Recall snapshot slot 0–11 |
| **CC1** | Mod Wheel | Drive morph fader position (0–127 → 0.0–1.0) |
| **Velocity** | Note velocity | Modulation source |
| **Aftertouch** | Channel pressure | Modulation source |
| **Mod Wheel** | CC1 | Modulation source |

### Modulation Sources
MIDI Velocity, Aftertouch, and Mod Wheel are exposed as modulation sources (see [§6](#6-modulation-system)).

---

## 13. Preset & State Management

| Feature | Description |
|---------|-------------|
| **Meta-presets** | `MetaPresetManager` manages preset libraries containing snapshot banks |
| **Preset serialization** | `PresetSerializer` / `PresetSerializerV2` — XML-based with base64-encoded parameter floats |
| **Preset library** | `PresetLibrary` with browsing and search |
| **DAW state save/restore** | `getStateInformation`/`setStateInformation` — APVTS + snapshot bank + hosted plugin state + opaque VST3 chunk |
| **VST3 program interface** | 12 snapshot slots as DAW programs; `setCurrentProgram()` enqueues recall via `LockFreeQueue` |
| **Deferred plugin reload** | Timer-based retry logic (max 10 attempts) for DAW threading constraints |
| **Audio-thread-safe save** | Detects offline render/audio-thread callers and uses buffered state copy |

---

## 14. Analysis & Metering

### Real-Time Spectrum Analyzer
- 256-bin FFT analysis
- Metrics: spectral centroid, rolloff, flux, crest factor, tilt, THD%, program crest, noise floor, SNR
- DC-offset removal in spectrum/crest/THD path

### Stereo Field Analyzer
- 4-band stereo analysis (Sub, Low, Mid, High)
- Per-band correlation, M/S energy ratio, overall stereo width

### LUFS Metering
- BS.1770-4 compliant (integrated, short-term, momentary, Loudness Range)

### True Peak Estimator
- Measurement-grade 4-phase × 64-tap polyphase FIR
- ±0.02 dBTP accuracy across DC, step, near-Nyquist, and two-tone signals

### Meter Window Accumulator
- Windowed averaging for smooth meter displays

---

## 15. Learn Mode & Parameter Intelligence

### Parameter Classifier
Automatically categorizes each hosted plugin parameter:
- **Continuous** — smooth 0.0–1.0 range
- **Discrete** — integer-like steps (waveform selector)
- **Binary** — on/off (bypass, mute)
- **Frequency** — Hz-based (logarithmic)
- **Decibel** — dB-based (logarithmic)
- **Percentage** — 0–100% linear
- **Enumeration** — named options (cannot interpolate)

### Learn Mode Features
- **AI exposure control** — `isExposed` flag per parameter (only relevant parameters sent to AI)
- **Importance scoring** — 0–1 score learned from user behavior
- **Category tagging** — "Oscillator", "Filter", "Envelope", etc.
- **Interpolatability flag** — marks which parameters can be morphed

### Token Optimizer
- Manages AI token budgets by selecting which parameters to expose to MCP/agent tools
- Reduces API costs while preserving the most relevant parameters

---

## 16. Plugin Profile & Semantic Mapping

### Plugin Profile Database
- `PluginProfileDB` — stores and retrieves parameter profiles for known plugins
- `SemanticPluginProfile` — rich semantic descriptions of plugin parameters
- `PluginSemanticMapper` — maps plugin parameters to a universal semantic vocabulary

### Profile Features
- Automatic parameter categorization on plugin load
- Cross-plugin parameter translation (e.g., "Cutoff" in one plugin maps to "Filter Frequency" in another)
- Semantic descriptions for AI understanding

---

## 17. Real-Time Safety

### Thread Safety Architecture
- **Three thread domains** with strict boundaries: audio, message, MCP
- **No locks on audio thread** — uses atomics, seqlocks, and lock-free queues
- **SPSC LockFreeQueue** — 8,192 capacity ring buffer with cache-line-aligned indices
- **Seqlock in SnapshotBank** — audio thread reads lock-free with retry; UI writes serialize

### Audio Thread Guarantees
- `processBlock()` is `noexcept` — zero allocations after `prepare()`
- All morph computation, interpolation, physics, and modulation are allocation-free on audio thread
- Spin-lock try-lock gates prevent blocking (non-blocking retry paths)

### Safety Policies
- **Neural mastering safety policy** — per-cycle delta caps prevent wild parameter jumps
- **SanityConfig** — protects dangerous parameters from AI/modification
- **Hold against morph** — neural and MCP parameter writes are held from morph overwrite
- **Read-back verification** — parameter writes verified after application; drift detection
- **Transition guard** — analysis windows straddling parameter changes are discarded

### Diagnostic Watchdog
- `MorePhiDiagnostics` — 250 ms timer detects message-thread stalls
- Writes diagnostic logs when enabled

---

## 18. Undo / Redo / A/B Compare

### Undo/Redo Manager
- **32-level undo/redo** for snapshot changes
- Per-slot tracking with labels
- `pushSnapshotState()`, `undo()`, `redo()`, `clear()`

### A/B Compare Engine
1. **Capture checkpoint** — save current state to reserved slot 11
2. **Apply candidate** — from GeneticOptimizer or ChainPlanExecutor
3. **Compare** — after 2 s analysis, compare LUFS/LRA/spectral score
4. **Auto-rollback** — if candidate is worse on ≥2 metrics, restore checkpoint
5. **Manual override** — UI exposes commit/rollback buttons

---

## 19. Cross-Plugin IPC

### VST3 IPC Bridge
- `VST3IPCBridge` enables communication between multiple More-Phi instances in a session
- Tools: `attach`, `detach`, `status`, `snapshot`, `dump`, `capture`, `run_assistant`
- **LinkBroadcaster** synchronizes morph state across instances

### Multi-Instance Support
- `InstanceRegistry` tracks all running More-Phi instances
- Each instance has a unique identity (`InstanceIdentity`)
- `list_instances`, `get_instance_info` for discovery

---

## 20. Dataset Generation

Comprehensive system for creating synthetic audio training data.

### V2 Pipeline (Sequential)
- **`DatasetGeneratorV2`** — orchestrator
- **`ParameterSampler`** — Latin Hypercube / stratified sampling
- **`AudioContentLibrary`** — source audio with genre classification
- **`PluginChainEngine`** — sequential multi-plugin chains (EQ, Dynamics, Mastering)
- **`EnhancedRenderPipeline`** — multi-segment rendering (Full / Transient / Steady-State)
- **`FeatureExtractor`** — MFCC, LUFS, spectral, temporal, perceptual features
- **`MetadataWriter`** — JSON / CSV / Parquet export
- **`ValidationEngine`** — KS test, MMD, coverage metrics
- **`DatasetOrganizer`** — train/val/test splits

### V3 Pipeline (Async/Parallel)
- **`DatasetGeneratorV3`** — high-performance async orchestrator
- **`TaskQueue`** — MPMC priority queue with backpressure
- **`WorkerPool`** — parallel batch processing
- **`ResourceMonitor`** — adaptive CPU/RAM throttling
- **`ProgressTracker`** — real-time progress and ETA
- **`CheckpointManager`** — crash recovery
- **`WatchdogTimer`** — hung thread detection

---

## 21. Licensing & Security

### Licensing System
- **Ed25519 digital signatures** — production keys injected via CI (`MORE_PHI_PROD_ED25519_KEY_HEX`)
- **Machine fingerprinting** — hardware-bound license activation
- **DPAPI (Windows)** — secure license storage using OS credential management
- **License envelope encryption** — encrypted license payload
- **Activation client** — online activation with server verification
- **License manager** — trial, activated, and expired states

### Security Validator
- `SecurityValidator` in the orchestrator layer enforces security policies
- **SecureString** utility for sensitive data handling
- **PermissionKernel** — tool-level classification and gating

---

## 22. Performance Optimization

### CPU Optimization
- **CPU Saver mode** — `cpuSaver` APVTS parameter; halves FFT size (min 512), caps oversampling at ×2, reduces audio-domain CPU by 40–60%
- **Interleaved touch sampling** — only samples 1/4 of parameters per block (rotating phase); ~20 ms detection latency within ~200 ms cooldown
- **SIMD support** — `SIMDAudio` module for vectorized DSP operations
- **Denormalalization guard** — prevents subnormal float performance penalties

### Memory Optimization
- **Rate-proportional capture ring** — 4 MiB @ 44.1 kHz, 16 MiB @ 192 kHz (vs fixed 16 MiB)
- **Throttle states reduction** — reduced from 8,192 to 4,096 entries (~64 KB saved)
- **Fixed-size arrays** — `ParameterState` uses `std::array<float, 4096>` (no heap allocation)
- **Eager ring allocation** — capture ring allocated in `prepare()` for deterministic memory

### Latency Management
- **`LatencyManager`** — centralized latency accounting:
  - Oversampling latency
  - FFT window latency (spectral morph)
  - Hosted plugin latency
  - Output protection limiter latency
  - Mastering chain latency
- Correct total latency reported to DAW

### Profiling
- **19 registered profiler sections** — from command queue drain through spectral/granular/spectral engines to output protection
- **`MORE_PHI_PROFILE()`** RAII macro — zero overhead in Release without flag
- **`getProfilingReport()`** — formatted per-section avg/max/percentage
- **`MorePhiDiagnostics`** — 250 ms stall detection watchdog

---

## 23. User Interface

### Main Components

| Component | Description |
|-----------|-------------|
| **MorphPad** | 2D XY pad for snapshot morphing with physics visualization |
| **SnapFader** | 1D fader for linear A/B morphing |
| **SnapshotRing** | Circular snapshot selector with visual comparison |
| **ModeBar** | Physics mode selection (Direct / Elastic / Drift) |
| **BreedingPanel** | Genetic breeding controls — parent selection, crossover, mutation |
| **ModulationMatrixPanel** | Visual modulation routing grid |
| **MacroKnobStrip** | 16 macro knob controllers |
| **AIChatPanel** | Conversational AI interface |
| **AIStatusPanel** | Agent runtime status display |
| **PluginBrowserPanel** | Hosted plugin browser |
| **PerformancePanel** | CPU/memory/profiler display |
| **ParameterMapPanel** | Visual parameter layout |
| **OnboardingOverlay** | First-run guided setup |
| **LicenseActivationOverlay** | License activation UI |
| **LLMSettingsDialog** | LLM provider configuration |
| **V2PresetBrowserPanel** / **V2TabBar** | Preset browsing with tab navigation |

### Engine Control Panels
- **SpectralControlPanel** — spectral morph engine parameters
- **GranularControlPanel** — grain size, density, pitch randomization
- **HybridBlendPanel** — blend weights between parameter/spectral/granular domains
- **DriftControlPanel** — drift mode parameters (speed, distance, chaos, orbit)
- **EngineTabPage** — tabbed interface for engine configuration

### Theme System
- Custom `MorePhiLookAndFeel` with theme support
- Theme directory for customization

---

## Quick Reference

| What | How |
|------|-----|
| Host a plugin | Scan or load via PluginBrowserPanel or MCP `load_hosted_plugin` |
| Capture a snapshot | Click capture button or MCP `capture_snapshot` (slots 0–11) |
| Morph between snapshots | Drag on MorphPad, use SnapFader, or play MIDI notes C3–B3 |
| Animate morph | Select Elastic/Drift physics or program Waypoint sequencer |
| Crossfade audio | Enable Spectral/Granular morph in HybridBlend panel |
| Modulate parameters | Assign LFOs/envelopes/macros in ModulationMatrix panel |
| AI mastering | Connect LLM provider → ConductorAgent decomposes goals → agents execute |
| Neural mastering | SonicMaster analyzes audio → `sonicmaster_decision` → `apply_mastering_plan` |
| Remote control | Connect MCP client to `localhost:30001` (JSON-RPC 2.0) |
| Save preset | Meta-preset library with full snapshot bank |
| Undo a change | Ctrl+Z (32 levels) or MCP rollback |

---

*This document reflects More-Phi v3.4.1 capabilities as of June 2026.*
