# More-Phi Technical Documentation

> Updated 2026-06-25. **Technical Audit Score: 7.9/10** — See [VST3_TECHNICAL_AUDIT_AND_MARKET_ANALYSIS.md](../VST3_TECHNICAL_AUDIT_AND_MARKET_ANALYSIS.md) for the complete audit.

More-Phi is an advanced parameter morphing engine for VST3/AU workflows. It hosts third-party plugins, captures parameter snapshots, morphs between them in real time, and exposes a local MCP interface for AI-assisted control. The v3.3.0 architecture includes a **Multi-Agent Orchestration Layer** (7 specialist agents: Conductor, Analysis, Optimization, Creative, RealtimeControl, QualitySafety, Memory), an `AgentOrchestrator` facade, embedded MCP server with 30+ tools, ONNX-based neural mastering (preview — model embedded in binary via JUCE `BinaryData`, rate-proportional capture ring, polyphase resampling, pending-plan application pattern, mono capture support), and comprehensive licensing with Ed25519 signatures.

> Screenshot placeholder: `[Screenshot: More-Phi main interface with MorphPad, plugin browser, tab bar, and AI status panel]`
>
> Diagram placeholder: `[Diagram: High-level module architecture from DAW host to hosted plugin and MCP clients]`

## Quick Navigation

- [System Overview](#system-overview)
- [Architecture](#architecture)
- [Installation and Build](#installation-and-build)
- [Dependencies](#dependencies)
- [Codebase Structure](#codebase-structure)
- [MCP API Reference](#mcp-api-reference)
- [Developer Workflows](#developer-workflows)
- [Contribution Guidelines](#contribution-guidelines)

## System Overview

More-Phi is a JUCE 8 C++20 desktop audio plugin. Its core job is to sit inside a DAW, host another plugin, capture that hosted plugin's parameter states, and generate expressive transitions between those states.

Primary capabilities:

| Capability | Description |
|---|---|
| Plugin hosting | Loads VST3 plugins on supported platforms and AU plugins on macOS. |
| Snapshot morphing | Stores up to 12 parameter snapshots and interpolates between them. |
| Physics modes | Adds direct, elastic, and drift behaviors to morph movement. |
| Genetic tools | Breeds, mutates, and randomizes snapshot states for sound design. |
| MIDI control | Supports snapshot note triggers and CC-driven morph control. |
| MCP automation | Exposes localhost JSON-RPC tools for AI clients and external controllers. |
| Semantic plugin profile | Describes hosted-plugin controls using safer semantic categories for LLM automation. |
| Mastering and analysis | Provides local metering, spectrum, stereo, and mastering-plan tooling. |

## Architecture

More-Phi uses a layered architecture with strict thread-domain separation.

```text
DAW Host
  |
  v
MorePhiProcessor
  |-- Core: morphing, physics, modulation, snapshots, audio-domain engines
  |-- Host: hosted plugin lifecycle and parameter bridge
  |-- AI: embedded MCP server, tool handler, semantic profiles, LLM settings
  |-- MIDI: note/CC routing and sidechain trigger handling
  |-- Preset: internal banks, presets, and state serialization
  `-- UI: MorphPad, SnapFader, panels, tabs, and status controls
```

> Diagram placeholder: `[Diagram: MorePhiProcessor owning Core, Host, AI, MIDI, Preset, and UI-facing subsystems]`

### Thread Domains

| Thread domain | Main entry points | Rules |
|---|---|---|
| Audio thread | `MorePhiProcessor::processBlock()` | No allocation, no blocking, no locks, no I/O. Drains command queues and processes audio. |
| Message thread | JUCE UI, timers, plugin loading, editor windows | Owns UI interactions and safe deferred hosted-plugin operations. Also owns `AgentOrchestrator` initialization and `EcosystemConfig` loading. |
| MCP threads | `MCPServer`, `MCPToolHandler` | Handles JSON-RPC I/O and queues parameter edits back to the processor. MCP threads are now instance-isolated. `AutomationRuntime` is per-instance, cache keys are prefixed with `instanceId + ':'`, and `InstanceRegistry` evicts zombies after TTL expiry. |
| Orchestrator thread | `AgentOrchestrator`, `SecurityValidator` | Mediates processor-to-agent runtime wiring, MCP message sanitization, auth validation, and rate limiting. Operates on the message thread during initialization; security validation runs on MCP threads. |
| Background workers | Dataset, rendering, scan, and validation jobs | Run long-lived work outside the audio callback. |
| Standalone MCP process | `MorePhiMcpServer` | Separate stdio MCP server for standalone workflows. |

### Audio Processing Pipeline

```text
sync APVTS atomics
→ drain parameter command queue
→ process MIDI snapshot/CC events
→ process snapshot recalls and captures
→ compute morph position and interpolation
→ apply modulation and audio-domain engines
→ write normalized parameter values through ParameterBridge
→ process hosted plugin audio
→ update realtime analyzers and meters
```

### Realtime Safety Model

Developers must keep the audio path deterministic and allocation-free. UI, MCP, and assistant features should communicate through existing queues, atomics, or deferred message-thread mechanisms.

Key primitives:

- `LockFreeQueue<ParamCommand, 8192>` for UI/MCP/assistant parameter edits.
- `SnapshotBank` seqlock-style reads for audio-safe snapshot access.
- APVTS atomics for DAW-automatable parameters.
- Double-buffered modulation route publishing.
- Hosted plugin exclusive access via `PluginHostManager`.
- `PerformanceProfiler` uses a pre-allocated circular buffer with atomic index — no allocations on audio thread.

### Recent DSP Correctness and Safety Fixes (v3.3.0)

A set of structural audio thread safety fixes were implemented in version 3.3.0 to enforce strict real-time boundaries and mathematical precision:

1. **Real-time Safe Envelope Tracking (`EnvelopeFollower`):**
   - **Pre-computed Coefficients:** Attack and release coefficients are calculated and converted to log bases on the message thread during parameter updates (`setAttack()` and `setRelease()`).
   - **Audio Thread Execution:** The processing loop avoids non-real-time-safe functions like `std::pow()`. When processing standard blocks, it reads pre-calculated block coefficients (`attackCoeffPerBlock_`/`releaseCoeffPerBlock_`). If block sizes vary dynamically, it relies on a faster `std::exp(numSamples * logBase)` calculation.

2. **Discrete Parameter Step Snapping (`DiscreteParameterHandler`):**
   - **Rounding Behavior:** Snaps continuous morph values to nearest discrete states using `std::round(value * maxStep)`. This resolves a truncating cast bug that made the maximum discrete state unreachable.
   - **Listen Mode Integration:** Snapped outputs are fed into the audio pipeline after the modulation matrix, preventing glitches or intermediate garbage values from being sent to discrete/binary inputs of the hosted plugin.

3. **Phase-Vocoder Stereo Coherence (`SpectralMorphEngine`):**
   - **Shared Transient Modification:** Runs transient detection on the left channel (Channel 0) and records the resulting morph alpha adjustments into a pre-allocated `blockAlphas_` shared array. Channel 1 (or subsequent channels) reuses these values for identical hop indices, avoiding channel phase drift.

## Agent Orchestration Layer

Introduced in v3.3.0, the **Agent Orchestration Layer** lives in `src/AI/Orchestrator/` and provides a single-initialization facade that wires `MorePhiProcessor` → `AgentRuntime` → `MCPServer`. All files compile into the `MorePhi` shared-code target.

### Components

| Component | Source Files | Purpose |
|---|---|---|
| `AgentOrchestrator` | `AgentOrchestrator.h` / `AgentOrchestrator.cpp` | Single-initialization facade that coordinates the processor, agent runtime, and MCP server lifecycle. |
| `EcosystemConfig` | `EcosystemConfig.h` / `EcosystemConfig.cpp` | Unified JSON configuration for plugin settings, agent behavior, MCP server options, and security policies. |
| `SecurityValidator` | `SecurityValidator.h` / `SecurityValidator.cpp` | MCP message sanitization, authentication validation, and rate limiting for incoming JSON-RPC connections. |
| `McpProtocol` | `McpProtocol.h` / `McpProtocol.cpp` | Explicit JSON-RPC 2.0 message schemas defining `McpRequest`, `McpResponse`, `McpNotification`, and `McpError`. |

### Initialization Flow

```text
MorePhiProcessor::prepareToPlay()
  |
  v
AgentOrchestrator::initialize()
  |---> EcosystemConfig::load(configPath)
  |---> SecurityValidator::setup(rules)
  |---> McpProtocol::registerSchemas()
  |---> AgentRuntime::start()
  |---> MCPServer::run()
```

All orchestrator components are instantiated on the **message thread** and operate outside the real-time audio path. They communicate with the audio thread exclusively through the existing `LockFreeQueue<ParamCommand, 8192>` and `SnapshotBank` seqlock mechanisms.


## Installation and Build

### Install a Release Build

#### Windows VST3

1. Copy `MorePhi.vst3` to one of the standard VST3 folders:
   - `C:\Program Files\Common Files\VST3\`
   - `C:\Users\<username>\Documents\VST3\`
2. Rescan plugins in your DAW.
3. Insert More-Phi on an audio or instrument track.

#### macOS VST3/AU

1. Copy the plugin bundle to one of these folders:
   - VST3: `/Library/Audio/Plug-Ins/VST3/`
   - AU: `/Library/Audio/Plug-Ins/Components/`
   - Current-user equivalents under `~/Library/Audio/Plug-Ins/`
2. Rescan plugins in your DAW.
3. Insert More-Phi on a compatible track.

### Build from Source

Requirements:

- CMake 3.24+
- C++20 compiler
- Git
- Platform SDK:
  - Windows: Visual Studio or Build Tools with Windows SDK
  - macOS: Xcode and command line tools
  - Linux: Clang/GCC and audio/UI development libraries

```bash
cmake -B build -S . -DMORE_PHI_BUILD_TESTS=ON
cmake --build build --config Release --parallel 2
ctest --test-dir build --build-config Release --output-on-failure --parallel 4
```

Windows safe preset:

```bash
cmake --preset windows-msvc-safe
cmake --build --preset windows-safe --parallel 2
```

RelWithDebInfo VST3 build example:

```bash
cmake --build build/codex-ninja-vs18d-relwithdebinfo --target MorePhi_VST3 --parallel 2
```

### Build Outputs

| Target | Output |
|---|---|
| Plugin | `build/MorePhi_artefacts/<config>/VST3/MorePhi.vst3` |
| macOS AU | `build/MorePhi_artefacts/<config>/AU/More-Phi.component` |
| CLI | `MorePhiCLI` |
| Standalone MCP server | `MorePhiMcpServer` |
| Tests | `MorePhiTests`, `MorePhiMcpServerTests` |

> **Note:** Orchestrator source files (`AgentOrchestrator`, `EcosystemConfig`, `SecurityValidator`, `McpProtocol`) in `src/AI/Orchestrator/` are compiled into the `MorePhi` shared-code target and do not produce a separate artifact.

## Dependencies

| Dependency | Version | Purpose |
|---|---:|---|
| JUCE | 8.0.4 | Plugin framework, UI, audio, VST3/AU hosting. |
| nlohmann/json | 3.11.3 | JSON serialization for MCP and profiles. |
| Catch2 | 3.4.0 | Unit and integration tests. |
| CMake | 3.24+ | Build configuration and dependency fetching. |

Common CMake options:

| Option | Default | Description |
|---|---:|---|
| `MORE_PHI_BUILD_TESTS` | `ON` | Adds test targets. |
| `MORE_PHI_BUILD_BENCHMARKS` | `OFF` | Builds benchmark target. |
| `MORE_PHI_COPY_PLUGIN_AFTER_BUILD` | `OFF` | Copies plugin to the system plugin folder after build. |
| `MORE_PHI_ENABLE_SANITIZERS` | `OFF` | Enables ASAN/UBSAN where supported. |
| `MORE_PHI_SAFE_BUILD_MODE` | `ON` | Uses conservative Windows linker/build settings. |
| `MORE_PHI_ENABLE_LTO` | `OFF` | Enables release LTO for CI/release use. |
| `MORE_PHI_ENABLE_DATASET_V3` | `OFF` (deprecated/no-op) | Dataset V3 sources are always compiled. |
| `MORE_PHI_MSVC_MP` | Host core count (VS generator) | MSVC `/MP` process count; `0` disables. Ignored under Ninja. |
| `MORE_PHI_ENABLE_ONNX` | `ON` | Enables ONNX runtime for neural mastering inference. |

## Codebase Structure

The project is structured into distinct layers with clear architectural responsibilities:

### 1. Embedded MCP Integration & AI (`src/AI/`, `src/CLI/`)
- `MCPServer`: Embeds a JSON-RPC 2.0 TCP server, managing API integration across system lifetimes. Uses a bounded `LockFreeQueue` (8192 capacity) to relay parameterized actions without blocking the audio thread.
- `MCPToolHandler`: Dispatches calls to DSP targets and offloads heavy tasks via `AsyncToolExecutor` to prevent audio dropouts. Features `ToolResultCache` with smart dirtying algorithms.
- `DatasetGenerator`: Advanced multi-pipeline architecture (V2 and asynchronous V3). Handles Latin Hypercube sampling, multithreaded offline routing, CPU chunking, crash checkpoints, and audio feature extraction to build supervised ML training materials.
- `StandaloneMcpServer`: Operates via `stdio` using `OzonePluginBackend` independently of the main plugin framework, bridging isolated configuration over process IPC.
- `MorePhiCLI`: A headless command-line tool designed for bulk non-GUI batch processing workflows and AI smoke testing without JUCE editor boundaries.

### 2. Core Audio & Data processing (`src/Core/`, `src/MIDI/`)
- `MorphProcessor`: Audio thread-safe orchestrator utilizing `noexcept` constraints and raw primitive buffers to transform user input via physics and interpolation configurations smoothly. Employs mathematically stable single-pole filters.
- `PhysicsEngine`: Handles 2D cursor algorithms for mathematical tracking: `ElasticState` (spring-damper physics) and `Drift` modes (Procedural Perlin Noise gradient generators). Stateless limits assure scaling stability.
- `GeneticEngine`: Mutates and breeds interpolation nodes using pseudo-evolutionary crossover operations and strict algorithmic boundary protections natively defined in a fast-hash map `SanityConfig`.
- `MIDIRouter`: Wait-free callback translation bridging notes-to-snapshots and control changes to direct UI interactions utilizing an array buffer limit implementation to protect allocations. Employs pre-computed RMS arrays for audio transient thresholds.

### 3. VST/AU Host Abstraction & Presets (`src/Host/`, `src/Licensing/`, `src/Preset/`)
- `PluginHostManager`: Encapsulates 3rd-party plugin lifecycle, discovery logic utilizing fallback methodologies, resolving specific plugin architecture bundles. Maintains exception guard recovery systems bypassing real-time crashes via isolated audio suspension zones.
- `ParameterBridge`: Casts diverse plugin traits directly onto normalized float structures, maintaining synchronization without dynamic allocations via memory throttling loop systems avoiding overloads and cache-miss stall scenarios over concurrent threads.
- `PresetLibrary`: A fast-indexing metadata indexer mapping recursive directory parsing of preset schemas and cross-referencing capabilities optimized exclusively to standard message UI threads without real-time boundaries.
- `PresetSerializer` / `LicenseManager`: Static-typed variable mapping structures parsing JSON object graphs into the exact binary arrays necessary to load DSP layouts, coupled with cryptographical `Ed25519Verifier` bounds protecting plugin deployment environments without online interruptions.

### 4. User Interface & Integration Layer (`src/Plugin/`, `src/UI/`, `src/Tools/`)
- `MorePhiProcessor`: Main APVTS controller and root plugin instance connecting GUI scopes with Real-time processing engines synchronously. Ensures cross-subsystem containment without global singleton dependencies.
- `MorePhiEditor`: Principal modern JUCE 8 Tab system connecting user interactions through atomic components avoiding event blocking. 
- `MorphPad`: Circular UX interaction element built on rendering loops retaining local circular tracking vectors directly optimized without heap fragmentation constraints over continuous drawing cycles.
- `OzoneHeadlessHost` (`src/Tools/`): Creates a fabricated dummy framework to satisfy iZotope integrations which require virtual Message Thread cycles simulating a real DAW without requiring explicit standard process hooks.

```text
src/
  Plugin/              JUCE plugin entry point and editor
  Core/                Morphing, DSP, snapshots, modulation, mastering engines
  Host/                Hosted plugin lifecycle, scanning, parameter bridge
  AI/                  MCP server, tool handler, LLM settings, semantic profiles
  AI/Orchestrator/     AgentOrchestrator, EcosystemConfig, SecurityValidator, McpProtocol
  AI/Agents/           Agent runtime, registry, scheduler, 7 specialist agents, blackboard, tooling, logging, LLM clients
  AI/StandaloneMcp/    Stdio MCP server for standalone workflows
  AI/Dataset/          Dataset generation, render pipeline, validation
  MIDI/                Snapshot note and CC routing
  Preset/              Meta presets, preset library, serialization
  UI/                  MorphPad, tabs, panels, controls, theme
  CLI/                 Standalone offline CLI executable tools
  Licensing/           Offline crypto licensing protocols
  Tools/               Headless host bridging capabilities

tests/
  Unit/                Catch2 unit tests
  Integration/         Plugin lifecycle and MCP integration tests
  Performance/         Benchmarks when enabled

research/              Experimental artifacts (Ozone/iZotope research, prototypes)
docs/
  architecture/        Architecture and methodology notes
  audits/              Manual DAW QA matrices
  design/              UI design documents
  superpowers/         Project planning artifacts
```

## MCP API Reference

More-Phi exposes an embedded local JSON-RPC MCP server. The active port and bearer token are shown in the AI status panel. Message schemas are explicitly defined by `McpProtocol` (`McpRequest`, `McpResponse`, `McpNotification`, `McpError`) in `src/AI/Orchestrator/McpProtocol.h`.

> Screenshot placeholder: `[Screenshot: AI status panel showing MCP ON, port, clients, Start/Stop MCP, and Copy Token controls]`

### Connection Model

| Field | Value |
|---|---|
| Transport | TCP localhost |
| Protocol | JSON-RPC 2.0 / MCP tool calls |
| Host | `127.0.0.1` |
| Port | Per instance, displayed in the UI |
| Auth | Bearer token from the AI status panel |
| Security | Auth tokens are compared in constant time to prevent timing attacks. |
| Idle timeout | Idle connections are closed after 30 seconds. |

### Tool Call Shape

```json
{
  "jsonrpc": "2.0",
  "method": "tools/call",
  "params": {
    "name": "tool_name",
    "arguments": {}
  },
  "id": 1
}
```

### Core Tool Groups

#### Plugin and Parameter Tools

| Tool | Purpose | Key arguments |
|---|---|---|
| `get_plugin_info` | Returns More-Phi and hosted plugin identity. | None |
| `list_parameters` | Lists hosted plugin parameters with normalized values. | None |
| `get_parameter` | Reads one parameter by `stableId`, `index`, or exact `name`. | `stableId`, `index`, `name` |
| `set_parameter` | Queues one hosted-plugin parameter edit. | `value`, plus identifier |
| `set_parameters_batch` | Queues multiple hosted-plugin parameter edits. | `parameters[]` or `params[]` |

Example request:

```json
{
  "jsonrpc": "2.0",
  "method": "tools/call",
  "params": {
    "name": "set_parameter",
    "arguments": {
      "index": 12,
      "value": 0.48
    }
  },
  "id": 2
}
```

Example response:

```json
{
  "success": true,
  "queued": true,
  "parameter": {
    "index": 12,
    "name": "Band 1 Gain",
    "value": 0.48
  }
}
```

#### Snapshot and Morph Tools

| Tool | Purpose | Key arguments |
|---|---|---|
| `capture_snapshot` | Captures current hosted-plugin state to slot `0`-`11`. | `slot`, `includeState` |
| `recall_snapshot` | Recalls a snapshot using `fast` or `full` mode. | `slot`, `mode` |
| `set_morph_position` | Sets XY or fader morph position. | `x`, `y`, `fader`, `source` |
| `get_morph_state` | Returns current morph position and source. | None |

Example:

```json
{
  "jsonrpc": "2.0",
  "method": "tools/call",
  "params": {
    "name": "set_morph_position",
    "arguments": {
      "x": 0.35,
      "y": 0.72,
      "source": "xy"
    }
  },
  "id": 3
}
```

#### Hosted Plugin Tools

| Tool | Purpose |
|---|---|
| `hosted_plugin.scan` | Inspects a VST3 plugin path and returns a plugin description. |
| `hosted_plugin.load` | Loads a hosted VST3 plugin from an explicit path. |
| `hosted_plugin.info` | Alias for `get_plugin_info`. |
| `hosted_plugin.parameters` | Alias for `list_parameters`. |
| `hosted_plugin.set_parameters` | Alias for `set_parameters_batch`. |
| `hosted_plugin.capture_state` | Captures hosted state and optionally a snapshot slot. |

#### Analysis and Mastering Tools

| Tool | Purpose |
|---|---|
| `analysis.get_summary` | Returns compact metering and analysis data. |
| `analysis.get_spectrum` | Returns a rebinned realtime spectrum snapshot. |
| `analysis.get_stereo_field` | Returns stereo field and mid-side metrics. |
| `analysis.capture_window` | Returns rolling deterministic DSP meter-window statistics. |
| `analysis.compare_render` | Compares two analysis summaries. |
| `mastering.plan_preview` | Generates a heuristic rule-based mastering plan without applying it. |
| `mastering.apply_plan` | Applies a heuristic rule-based mastering plan. |
| `mastering.render_status` | Polls an offline mastering render job. |
| `mastering.select_candidate` | Selects a candidate from a render batch. |

#### Semantic Plugin Profile Tools

Semantic tools are the preferred automation surface for AI clients because they expose musical intent and safety categories instead of raw parameter IDs.

| Tool | Purpose |
|---|---|
| `plugin_profile.audit_parameters` | Builds a versioned hosted-plugin parameter audit. |
| `plugin_profile.describe_semantics` | Returns semantic controls with safety categories and action limits. |
| `plugin_profile.apply_safe_action` | Applies a guarded semantic action and returns a rollback snapshot ID. |
| `plugin_profile.restore_safe_snapshot` | Restores a rollback snapshot from a previous safe action. |
| `plugin_profile.get` | Loads a saved profile or audits the current plugin. |
| `plugin_profile.save` | Saves the current hosted-plugin profile. |
| `plugin_profile.describe_semantic_map` | Returns grouped LLM-safe semantic controls. |

Example safe action:

```json
{
  "jsonrpc": "2.0",
  "method": "tools/call",
  "params": {
    "name": "plugin_profile.apply_safe_action",
    "arguments": {
      "action": {
        "type": "set_semantic_normalized",
        "semantic_id": "eq.band_1.gain",
        "normalized_value": 0.48
      },
      "dry_run": false
    }
  },
  "id": 4
}
```

Example response:

```json
{
  "success": true,
  "snapshot_id": "safe_action_1",
  "queued": 1,
  "before_parameter_count": 128
}
```

Possible guardrail errors:

| Error | Meaning |
|---|---|
| `control_locked` | The requested control is not safe for automation. |
| `caution_requires_confirmation` | The control requires explicit `allow_caution: true`. |
| `value_out_of_safe_range` | The normalized value is outside the semantic safe range. |
| `delta_out_of_safe_range` | The requested EQ gain delta exceeds the allowed range. |
| `pending_parameter_edits` | A previous parameter queue has not drained yet. |
| `snapshot_context_mismatch` | The hosted plugin layout changed before restore. |
| `snapshot_not_found` | The rollback snapshot ID is unknown. |

#### Agent Orchestration Tools

The multi-agent layer exposes 7 tools for goal-driven agent orchestration, dispatched via `MCPToolHandler::handle` and classified in `PermissionKernel::classifyTool`:

| Tool | Purpose |
|---|---|
| `agents.list` | Lists all registered agents with their status and capabilities. |
| `agents.run_goal` | Submits a high-level goal to the Conductor agent for decomposition and multi-step execution. |
| `agents.run_task` | Submits a single task to a specific agent for execution. |
| `agents.run_status` | Queries the status of a running goal or task. |
| `agents.run_cancel` | Cancels a running goal or task. |
| `agents.blackboard.recent` | Returns recent blackboard events for an agent or domain. |
| `agents.set_autonomy` | Adjusts agent autonomy level (manual/assisted/autonomous). |

#### Neural Mastering Pipeline

The neural mastering subsystem (`SonicMasterAnalysisEngine`) provides an alternative edit entry point alongside the MCP `set_parameter` path. Both funnel through `LockFreeQueue` → audio-thread drain → `ParameterBridge` with aligned safety properties:

- **ONNX model** embedded in binary via `BinaryData::getNamedResource("masteringbrain_v2_decision_onnx")` — no runtime file I/O
- **Capture ring** is rate-proportional (`8.0 * sampleRate`, clamped `[2×44100, 32×192000]`), lazily allocated
- **Resampling** uses `resamplePolyphase` (not linear interpolation)
- **Mono capture** supported (`channelCount == 1` downmix path)
- **Plan application** uses pending-plan atomic-flag pattern — stores in `pendingPlan_`, applies in `runCycle` on the analysis thread (no `callAsync`)
- **Plan boundary markers** — `enqueuePlanBoundary()` + `lastDrainedPlanId_` atomic for observability
- **Transition guard** — `notifyHostedParameterChanged()` + ring flush prevents analyzing hybrid states during parameter changes
- **Action ledger** cap raised to 4096 (`kLedgerMaxTransactions`)

## Developer Workflows

### Add a New MCP Tool

1. Define the tool in `MCPToolHandler.cpp` with a clear description and JSON schema.
2. Add dispatch in `MCPToolHandler::handle()`.
3. Route parameter writes through `MorePhiProcessor` queue APIs.
4. Add Catch2 coverage in `tests/Unit/TestAIRegressions.cpp` or an integration test.
5. Run focused tests before broad tests.

### Add a New Audio Feature

1. Keep allocations out of `processBlock()`.
2. Pre-size buffers in `prepareToPlay()` or subsystem `prepare()` methods.
3. Use atomics or queues for UI-to-audio handoff.
4. Add unit tests for pure computation.
5. Add DAW/manual QA notes for UI or host behavior.

### AI Assistant Access

`getAIAssistant()` returns `AIAssistant*` (nullable), not `AIAssistant&`. Callers must null-check before dereferencing.

### Run Tests

```bash
ctest --test-dir build --build-config Release -R "TestAIRegressions|TestMCPServerUnit" --output-on-failure
ctest --test-dir build --build-config Release --output-on-failure --parallel 4
```

## Contribution Guidelines

### Code Quality

- Preserve realtime safety on the audio path.
- Prefer focused changes over broad refactors.
- Keep user-facing behavior explicit and testable.
- Avoid singletons unless extending existing registry patterns.
- Keep hosted-plugin access mediated by `PluginHostManager` and `ParameterBridge`.

### Documentation Quality

- Use concise headings and task-oriented examples.
- Include screenshots or diagram placeholders when visuals are expected.
- Keep MCP examples valid JSON.
- Document safety limits and thread-domain constraints for developer-facing features.

### Pull Request Checklist

- [ ] Build succeeds for the affected target.
- [ ] Focused tests pass.
- [ ] Full active suite passes when feasible.
- [ ] UI changes are manually exercised in a host or standalone test environment.
- [ ] MCP changes include request/response examples and error cases.
- [ ] Realtime/audio-thread constraints are respected.
