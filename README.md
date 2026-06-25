# More-Phi v3.3.0

**Advanced Parameter Morphing Engine** — A VST3/AU plugin that hosts other plugins and morphs between parameter snapshots using physics-based interpolation, genetic breeding, multi-agent AI orchestration, and realtime neural mastering.

> **Technical Audit Score: 7.9/10** — "The most disciplined audio-plugin codebase I have audited." See [VST3_TECHNICAL_AUDIT_AND_MARKET_ANALYSIS.md](VST3_TECHNICAL_AUDIT_AND_MARKET_ANALYSIS.md) for the complete 39 KB audit report with 26 verifiable claims, competitor analysis, and market positioning.

---

## Features

### Core Morphing
- **Plugin Hosting** — Load any VST3/AU plugin inside More-Phi with production-grade exception firewall (SEH+C++ dual-guard, auto-suspension, deferred teardown, fade seams)
- **12-Slot Snapshot System** — Capture and recall parameter states in clock-layout with seqlock-protected lock-free audio-thread reads
- **2D Morph Pad** — XY pad with inverse-distance weighted (IDW) interpolation, gravity-well mass weights, 64-point cursor trail ring buffer
- **1D Snap Fader** — Linear interpolation across occupied snapshots with slot markers
- **Physics Modes** — Direct (instant), Elastic (spring-damper with adaptive sub-stepping and fully-implicit damping), Drift (Perlin noise with 8 gradient directions)
- **Voronoi/NNI Morphing** — Natural Neighbor Interpolation via Bowyer-Watson Delaunay triangulation, with IDW fallback outside convex hull
- **Genetic Breeding** — Crossover + mutation with sanity-mode protection for dangerous parameters (Volume, Pitch, Bypass)
- **Waypoint Engine** — BPM-synced 16-waypoint morph path sequencer with configurable hold/transition beats

### Audio-Domain Processing
- **Spectral Morphing** — STFT phase-vocoder with periodic Hann COLA, log-magnitude geometric mean, instantaneous-frequency interpolation, transient preservation
- **Granular Morphing** — Real-time grain cloud with Hann² normalization, xorshift32 PRNG, pitch randomization LUT, staggered grain scheduling
- **Formant Preservation** — Cepstral liftering for vocal/instrument character preservation during morphing
- **Hybrid Blend** — Equal-power SIMD mixing of parameter, spectral, and granular outputs

### Built-in Mastering Chain (11 stages)
- M/S encode → 4-band Linkwitz-Riley split → per-band VCA dynamics → 32-band adaptive EQ → stereo imager → harmonic exciter (4× oversampled tanh) → loudness normalizer → M/S decode → brickwall limiter (ISP-aware with true-peak cache) → LUFS meter (BS.1770-4 verified) + true-peak estimator (96-tap Kaiser FIR)

### AI & Automation
- **Embedded MCP Server** — JSON-RPC 2.0 on localhost:30001+ with per-instance bearer token auth, 30s idle timeout, rate limiting, batch request support, 30+ tools
- **7-Agent AI Orchestration** — Conductor (goal decomposition) + Analysis/Optimization/Creative/RealtimeControl/QualitySafety/Memory specialists with O(1) priority scheduler, 2-tier starvation prevention, 3-layer fault isolation
- **Realtime Neural Mastering (Preview)** — ONNX in-process inference with HTTP fallback, 3-thread model (audio capture → analysis → message thread handoff), safety policy clamping
- **Learn Mode** — AI parameter classification, importance scoring, token optimization with cost models for Claude/GPT
- **iZotope Ozone Integration** — Parameter mapping, plan application, Track Assistant IPC bridge

### Modulation
- **Modulation Matrix** — 128 routes with seqlock double-buffer publishing
- **4 LFOs** — 6 shapes (Sine, Triangle, Saw, Square, S&H, Smoothed Random), tempo-sync, rate-independent random
- **2 Envelope Followers** — RMS-based with pre-computed per-block coefficients
- **2 Step Sequencers** — 32 steps, 4 directions, seqlock config, per-step smoothing
- **16 Macro Knobs** — 10Hz sync with command queue overflow warning

### MIDI Integration
- **Note Triggers** — C3-B3 (configurable octave) trigger snapshots 1-12
- **CC Routing** — Mod Wheel (CC1) controls fader position
- **Sidechain Trigger** — Audio-driven snapshot recall with exponential envelope coefficients
- **Channel Filter** — Configurable MIDI channel (0=omni)

### Licensing & Security
- **Ed25519 Signature Verification** — Vendored orlp/ed25519 (no external crypto dependency), production public key, fail-closed
- **Crockford Base32 License Keys** — CRC32 checksum, O/I/L disambiguation
- **Machine Fingerprinting** — OS+CPU+memory hash binding
- **Activation/Deactivation** — Server-side seat management with friendly error codes, force-local-clear fallback
- **Constant-Time Token Auth** — XOR accumulator comparison for MCP bearer tokens
- **JSON-RPC Validation** — Depth check, field whitelist, method whitelist

### Performance Optimizations
- **Interleaved Touch Sampling** — 75% reduction in hosted-plugin `getValue()` calls
- **Morph Stable-Skip** — Skips entire interpolation chain when output has stabilized
- **5× Pre-Computed DSP Coefficients** — `std::exp`/`std::pow` never called on audio thread
- **SIMD Acceleration** — AVX2 (8 floats), SSE2 (4 floats) with runtime CPU detection
- **CPU Saver Mode** — Halves audio-domain FFT, caps oversampling at ×2
- **Lock-Free Audio Thread** — Zero heap allocation after `prepareToPlay()`, all audio paths `noexcept`

### Dataset Generation
- **V2 Pipeline** — Latin Hypercube Sampling, plugin chains, enhanced rendering, feature extraction, metadata, validation
- **V3 Pipeline** — Async with worker pool, adaptive throttling, checkpoint recovery, watchdog timer

---

## Quick Start

### 1. Load a Plugin
1. Add More-Phi to a track in your DAW
2. Click **"Load Plugin"** in the plugin browser panel
3. Select a VST3 or AU plugin to host
4. The hosted plugin's UI opens automatically

### 2. Capture Snapshots
1. Tweak the hosted plugin's parameters to a sound you like
2. **Double-click** on the MorphPad to capture to the next empty slot
3. **Right-click** a snapshot dot to capture to a specific slot
4. Repeat for up to 12 snapshots

### 3. Morph Between Sounds
- **XY Pad:** Drag anywhere on the circular pad — cursor blends nearby snapshots
- **Snap Fader:** Drag the vertical fader to interpolate linearly
- **Physics:** Enable Elastic (spring) or Drift (Perlin noise) in the Mode Bar
- **Breeding:** Click Breed/Mutate to evolve new parameter combinations

### 4. Connect AI (Optional)
1. Check the AI Status panel for the MCP port and bearer token
2. Connect any MCP-compatible client to `localhost:<port>`
3. Call `initialize` with `bearer_token` first, then use any of 30+ tools

---

## MCP Integration

More-Phi includes a built-in MCP (Model Context Protocol) server with 30+ tools across these categories:

| Category | Tools | Description |
|----------|:-----:|-------------|
| **Hosted Plugin** | `get_plugin_info`, `list_parameters`, `get_parameter`, `set_parameter`, `set_parameters_batch`, `sweep_parameter`, `load_hosted_plugin`, `scan_hosted_plugin` | Full parameter read/write, plugin lifecycle |
| **Snapshots & Morphing** | `capture_snapshot`, `recall_snapshot`, `set_morph_position`, `get_morph_state`, `get_morph_compatibility`, `suggest_intermediate_snapshots` | Snapshot capture/recall, morph control, compatibility analysis |
| **Analysis & Metering** | `analysis.get_summary`, `analysis.get_spectrum`, `analysis.get_stereo_field`, `analysis.capture_window`, `analysis.compare_render`, `diagnose_parameter_pipeline` | LUFS, true-peak, spectrum, stereo field, comparison |
| **Mastering** | `apply_mastering_plan`, `get_mastering_state`, `mastering.plan_preview`, `mastering.render_batch`, `sonicmaster_decision`, `ozone.track.*` | Heuristic + neural mastering, Ozone integration |
| **More-Phi Control** | `more_phi.list_parameters`, `more_phi.get_parameter`, `more_phi.set_parameter`, `more_phi.set_parameters` | Direct More-Phi parameter control |
| **AI & Learning** | `analyze_parameters`, `expose_parameters`, `get_token_estimate`, `learn_from_adjustment`, `get_learn_mode_status`, `find_related_parameters` | Learn Mode, token management, semantic mapping |
| **Agents** | `agents.list`, `agents.run_goal`, `agents.run_task`, `agents.run_status`, `agents.run_cancel`, `agents.blackboard.recent`, `agents.set_autonomy` | 7-agent orchestration control |
| **Dataset** | `generate_dataset`, `generate_dataset_v2`, `generate_dataset_v3` | Synthetic audio dataset generation |
| **System** | `get_instance_info`, `list_instances`, `run_self_test`, `heartbeat` | Multi-instance, diagnostics, liveness |

### Connection Details
- **Protocol:** JSON-RPC 2.0
- **Port:** Per-instance dynamic (30001+), shown in AI Status panel
- **Host:** localhost only (non-local connections rejected)
- **Auth:** Bearer token via `initialize` params
- **Limits:** 4 concurrent connections, 30s idle timeout, 256 KB max request

### Analysis & AI Claim Boundaries

More-Phi's analysis tools expose deterministic DSP measurements. They are useful for comparison and metering but are not certified laboratory instruments:

- Loudness: Lightweight BS.1770-style rolling estimates — not formal reference-vector certification
- True peak: 96-tap Kaiser β=9 polyphase FIR estimate (≈±0.2 dBTP)
- Spectrum: Hann-window FFT mono sum — anti-phase stereo may cancel
- Stereo field: Mid/side energy analysis with banded correlation
- Mastering plans: Deterministic heuristic rule engine with rule IDs — not learned model predictions
- Genre classifier: Default fallback unless real model backend loaded
- Neural mastering: Preview quality, clamped by safety policy

---

## Physics Modes

| Mode | Algorithm | Use Case |
|------|-----------|----------|
| **Direct** | Instant cursor → interpolation | Precise control |
| **Elastic** | Spring-damper, adaptive sub-stepping, fully-implicit damping (ζ=1.5 Heavy preset), velocity kill with near-target check | Smooth, organic transitions |
| **Drift** | Perlin noise, 8 gradient directions, octave summation (max 4), Free/Locked/Orbit variants | Evolving textures, ambient soundscapes |

---

## MIDI Mapping

### Snapshot Triggers
- **Notes C3-B3** (configurable octave) trigger snapshots 1-12
- Only triggers if snapshot slot is occupied
- Note-off also consumed for trigger-range notes (prevents leaking to hosted plugin)

### Morph Control
- **Mod Wheel (CC1)** controls fader position
- Automatically switches to fader mode

### Sidechain
- External audio input triggers snapshot advance on transient detection
- Configurable threshold with exponential envelope follower

---

## Installation

### Windows
1. Download `MorePhi.vst3` from [Releases](https://github.com/your-repo/releases)
2. Copy to `C:\Program Files\Common Files\VST3\`
3. Rescan plugins in your DAW

### macOS
1. Download `More-Phi.component` (AU) or `MorePhi.vst3`
2. Copy to `/Library/Audio/Plug-Ins/Components/` or `/Library/Audio/Plug-Ins/VST3/`
3. Rescan plugins in your DAW

### Build from Source

**Requirements:** CMake 3.24+, C++20 compiler, Git

```bash
git clone https://github.com/your-repo/morephi.git
cd morephi

# Configure and build Release
cmake -B build -S .
cmake --build build --config Release --parallel 2

# Build with tests
cmake -B build -S . -DMORE_PHI_BUILD_TESTS=ON
cmake --build build --config Release --parallel 2
ctest --test-dir build --output-on-failure

# Debug build
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug

# With sanitizers (Clang/GCC)
cmake -B build -S . -DMORE_PHI_BUILD_TESTS=ON -DMORE_PHI_ENABLE_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug

# With ONNX Runtime (neural mastering)
cmake -B build -S . -DMORE_PHI_ENABLE_ONNX=ON

# With profiling
cmake -B build -S . -DMORE_PHI_ENABLE_PROFILING=ON

# Safe local build (Windows, conservative settings)
cmake --preset windows-msvc-safe
cmake --build --preset windows-safe --parallel 2
```

---

## Architecture

```
src/
├── Plugin/          MorePhiProcessor (878-line header, 45 APVTS params), MorePhiEditor (V2 tabbed, 920×760)
├── Core/            InterpolationEngine (SIMD IDW+Voronoi), PhysicsEngine (3 modes, sub-stepping),
│                    SnapshotBank (12-slot seqlock), MorphProcessor, GeneticEngine,
│                    SpectralMorphEngine (STFT, periodic Hann COLA), GranularMorphEngine (Hann² normalized),
│                    FormantMorphEngine (cepstral liftering), AutoMasteringEngine (11-stage M/S chain),
│                    AdaptiveEQ (32-band), BrickwallLimiter (ISP-aware), LUFSMeter (BS.1770-4),
│                    TruePeakEstimator (96-tap FIR), ModulationEngine, ModulationMatrix (128 routes),
│                    LFO (6 shapes), StepSequencer (4 dirs), DiscreteParameterHandler (5 strategies),
│                    WaypointEngine, HybridBlend, PerformanceProfiler, ThreadPool
├── Host/            PluginHostManager (SEH+C++ guard, ref-counted leasing, deferred doom),
│                    ParameterBridge (withPlugin template, batch ops, exception counting)
├── AI/              MCPServer (JSON-RPC 2.0, 30+ tools), MCPToolHandler, MCPToolsExtended (18 tools)
│   ├── Agents/      AgentRuntime, ConductorAgent, 6 specialist agents, PriorityScheduler (O(1) 4-level),
│   │                BlackboardBridge (sequence-cursor), DefaultToolInvoker (fail-closed)
│   ├── Dataset/     DatasetGeneratorV2, DatasetGeneratorV3, ParameterSampler, ValidationEngine
│   ├── StandaloneMcp/ StandaloneMcpServer (stdio JSON-RPC), OzonePluginBackend
│   └── Orchestrator/ AgentOrchestrator, SecurityValidator
├── MIDI/            MIDIRouter (256-event pre-allocated, channel filter, sidechain)
├── Preset/          PresetSerializer (V1+V2), PresetLibrary (CRUD, search, tags, ratings)
├── Licensing/       LicenseManager (Ed25519), LicenseVerifier, SecureLicenseStore, ActivationClient
└── UI/              MorphPad, SnapFader, SnapshotRing, BreedingPanel, AIChatPanel,
                     PluginBrowserPanel, ModeBar, MacroKnobStrip, BottomControlStrip,
                     EngineTabPage, ModulationMatrixPanel, V2PresetBrowserPanel, +10 more
```

---

## Documentation

- [Technical Audit & Market Analysis](VST3_TECHNICAL_AUDIT_AND_MARKET_ANALYSIS.md) — 39 KB comprehensive audit with 26 verifiable claims, competitor analysis, and market positioning
- [User Guide](docs/USER_GUIDE.md) — Complete usage tutorial
- [API Reference](docs/API_REFERENCE.md) — MCP tools and protocols
- [Developer Guide](docs/DEVELOPER_GUIDE.md) — Building and contributing
- [Architecture](docs/ARCHITECTURE.md) — Technical design details
- [Build Stability Guide](docs/BUILD_STABILITY_GUIDE.md) — Avoiding build freezes on Windows
- [VST3 License Key Management](docs/VST3_LICENSE_KEY_MANAGEMENT.md) — Licensing architecture, activation flows
- [Dataset Generation](docs/DATASET_GENERATION.md) — V2 and V3 pipeline documentation
- [Learn Mode Guide](docs/LEARN_MODE_GUIDE.md) — AI parameter optimization
- [Audio Engine Specification](docs/AudioEngineSpec_v2.md) — Audio processing details
- [Ozone IPC Assistant Capabilities](docs/OZONE_IPC_ASSISTANT_CAPABILITIES.md) — Verified assistant workflows
- [iZotope IPC Research Methodology](docs/OZONE_IPC_RESEARCH_METHODOLOGY.md) — IPC transport and schema research

### Agent Documentation
- [Multi-Agent Orchestration Layer Design](docs/superpowers/specs/2026-06-21-multi-agent-orchestration-layer-design.md)
- [SonicMaster Realtime Integration Design](docs/superpowers/specs/2026-06-21-sonicmaster-vst3-realtime-integration-design.md)
- [Neural Mastering Roadmap](specs/003-neural-mastering-roadmap/plan.md)

---

## Troubleshooting

### Plugin Won't Load
- Verify plugin is in correct folder (see Installation)
- Rescan plugins in DAW
- Check DAW is 64-bit

### No Sound
- Ensure a hosted plugin is loaded
- Check hosted plugin is receiving MIDI (if instrument)
- Verify audio routing in DAW

### Plugin Crashes on Load (FL Studio)
- More-Phi is configured with 4 MB stack (`/STACK:4194304`) for plugin-in-plugin hosting
- If crashes persist, try increasing FL Studio's plugin stack size

### MCP Server Won't Start
- Check if port is already in use (starts at 30001, increments on bind failure)
- Look for firewall blocking localhost connections
- Check DAW console logs

### Hosted Plugin Parameters Not Capturing
- Only automatable parameters are captured
- Use Full Recall mode for plugins that store internal state (Kontakt, wavetable synths)

### Plugin Disconnects During Export or Close/Reopen
- Fixed in v3.3.0+ with robust state restoration and Timer-deferred plugin reloading
- See [TROUBLESHOOTING_PLUGIN_DISCONNECTION.md](docs/TROUBLESHOOTING_PLUGIN_DISCONNECTION.md)

### Poor Performance
- Enable **CPU Saver** mode (halves audio-domain FFT, caps oversampling)
- Switch to **Direct** physics mode
- Disable unused audio-domain engines (Spectral, Granular, Formant)
- Increase audio buffer size in DAW
- Disable touch detection in PERF-OPT flags (coarse writes + disable touch)

### PC Freezes During CMake Build
- Use safe local presets: `cmake --preset windows-msvc-safe`
- Single job: `cmake --build --preset windows-single --parallel 1`
- See [docs/BUILD_STABILITY_GUIDE.md](docs/BUILD_STABILITY_GUIDE.md)

---

## Technical Highlights

| Claim | Mechanism |
|-------|-----------|
| Zero heap allocation after `prepareToPlay()` | Pre-sized vectors, fixed arrays throughout all DSP engines |
| Audio thread never blocks | Lock-free queue, seqlock reads with retry, try-lock gates on all audio-thread boundaries |
| SEH+C++ dual-guard for hosted plugins | `__try/__except` + `try/catch` wrapping every `plugin->processBlock()` call |
| Streaming-safe limiter ceiling | -1.0 dBTP enforced on every neural and heuristic apply |
| Sample-rate-independent smoothing | Time constant τ from reference config, α = exp(-dt/τ) |
| Inter-sample peak protection | True-peak cached per write position in brickwall limiter lookahead |
| Verified LUFS K-weighting | Continuous-time coefficients solved to match BS.1770-4 exactly at 48 kHz via bilinear transform |
| Agent fault isolation (3 layers) | Worker → execute+publish+followUps+store → per-subscriber |
| O(1) agent scheduling with anti-starvation | 4 per-priority `std::queue`, splice promotion at 1000ms, tier2 at 5000ms |

---

## License

This project's source code is released under the MIT License — see [LICENSE](LICENSE).

### Third-Party Licenses

**JUCE Framework** — This software uses [JUCE](https://juce.com/) 8.0.4, which is dual-licensed:
- **AGPLv3**: For open-source projects
- **Commercial License**: For closed-source/commercial distribution

If you distribute binary builds, you must either release under AGPLv3 or purchase a [JUCE commercial license](https://juce.com/pricing).

**Dependencies** (all fetched automatically via CMake FetchContent):
- JUCE 8.0.4
- nlohmann/json 3.11.3
- Catch2 v3.4.0 (tests)
- ONNX Runtime 1.22.1 (optional, gated by `MORE_PHI_ENABLE_ONNX`)

---

## Credits

Built with [JUCE](https://juce.com/) 8.0.4  
Version 3.3.0 — Synthesizer Edition  
Technical audit score: **7.9/10** — [full report](VST3_TECHNICAL_AUDIT_AND_MARKET_ANALYSIS.md)
