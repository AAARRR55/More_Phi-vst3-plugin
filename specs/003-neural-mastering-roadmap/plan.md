# Implementation Plan: Neural Mastering Implementation Roadmap

**Branch**: `003-neural-mastering-roadmap` | **Date**: 2026-05-18 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/003-neural-mastering-roadmap/spec.md`

## Summary

Convert the neural mastering audit into an executable implementation plan that starts with deterministic safety infrastructure before any optional model backend. The plan establishes fixed-size control-plan schemas, a deterministic Safety Layer, feature-frame extraction, validated-plan application to existing mastering processors, data governance, optional proof-of-concept model runner boundaries, and review/reporting contracts. Deterministic More-Phi mastering DSP remains the audio-rate Data Plane; learned or heuristic planners operate only outside the audio callback and may publish bounded control intent that must pass safety projection before use.

## Technical Context

**Language/Version**: C++20 for plugin/runtime code, JUCE 8 and CMake for build integration; optional offline validation scripts may use Python but are not required for plugin runtime.

**Primary Dependencies**: Existing More-Phi `Core` mastering processors and analyzers, `AutoMasteringEngine`, `ChainPlanExecutor`, `AutomationControlPlane`, dataset modules, Catch2 test target, CMake source groups, nlohmann/json for JSON fixtures/contracts, and optional future CPU inference backend such as ONNX Runtime only behind an opt-in proof-of-concept flag.

**Storage**: Planning artifacts in `specs/003-neural-mastering-roadmap/`; future runtime defaults in `config/neural-mastering/`; no required plugin state persistence change in the initial safety phases. Future model artifacts require schema/version, checksum, feature-layout, supported-sample-rate, provenance, and evidence metadata.

**Testing**: Catch2 unit/integration tests through `MorePhiTests`, optional `MorePhiBenchmarks`, script-level validation for dataset governance/objective metrics/model-card checks, and future level-matched listening review artifacts before professional quality claims.

**Target Platform**: More-Phi VST3 on Windows/Linux/macOS and AU on macOS in professional DAW hosts, with conservative CPU-only fallback assumptions and support for small DAW buffers through deterministic audio-thread behavior.

**Project Type**: JUCE audio plugin with Core DSP, AI/MCP control surfaces, dataset/offline tooling, and specification/contract artifacts.

**Performance Goals**: No neural inference, model loading, file I/O, locks, allocation, exceptions, or remote calls in `processBlock()`; 100% invalid plan rejection in tests; deterministic fallback for missing/late/invalid/low-confidence model output; future optional planner/model benchmarks must report worst-case latency and CPU outside the callback before runtime enablement.

**Constraints**: Direct neural audio-callback inference is rejected unless future measured proof satisfies fixed topology, fixed memory, no allocation, no locks, no I/O, no exceptions, no remote dependency, and buffer-deadline headroom. Model artifacts are optional and disabled by default until G01-G10 evidence supports advancement.

**Scale/Scope**: Seven implementation phases covering core schemas/safety projection, feature frames, deterministic application, dataset governance/offline tooling, optional compact TCN PoC runner, UI/MCP review reporting, and release-readiness gates. Initial supported analysis/application scope is mono and stereo mastering; wider layouts remain flagged until semantics and validation are defined.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- Real-time safety: PASS. The baseline plan adds no neural inference to `processBlock()` and requires fixed-size, deterministic control structures plus non-blocking fallback behavior before any audio-rate state changes.
- Thread domains: PASS. Planner work is limited to offline, preview, background, message-thread, or queued low-rate control paths; audio callback consumes only deterministic processor state.
- Parameter safety: PASS. Plan candidates must preserve normalized/control semantics through masks, ranges, max deltas, type-aware validation, and Safety Layer projection before application.
- AI/MCP boundaries: PASS. Learned behavior is optional, evidence-labeled, disabled by default, and cannot bypass local validation; future MCP/UI surfaces are status/review-first.
- Persistence and compatibility: PASS. Initial phases do not require plugin state schema changes; future model artifacts use separate metadata/config contracts and deterministic fallback when missing.
- Tests and validation: PASS. The plan requires unit tests for safety projection, integration tests for transition/fallback behavior, dataset-governance checks, objective metrics, benchmarks, and listening evidence before release claims.

## Project Structure

### Documentation (this feature)

```text
specs/003-neural-mastering-roadmap/
├── spec.md
├── implementation-strategy.md
├── neural-training-implementation-plan.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   ├── neural-mastering-plan.schema.json
│   ├── model-card.schema.json
│   └── validation-gates.md
└── checklists/
    └── requirements.md
```

### Source Code (repository root)

```text
src/
├── Core/
│   ├── NeuralMasteringTypes.h
│   ├── NeuralMasteringSafetyPolicy.h
│   ├── NeuralMasteringSafetyPolicy.cpp
│   ├── NeuralMasteringPlanSmoother.h
│   ├── NeuralMasteringPlanSmoother.cpp
│   ├── AutoMasteringEngine.h
│   ├── AutoMasteringEngine.cpp
│   └── existing mastering DSP/analyzer modules
├── AI/
│   ├── NeuralMasteringController.h
│   ├── NeuralMasteringController.cpp
│   ├── NeuralMasteringModelRunner.h
│   ├── NeuralMasteringModelRunner.cpp
│   ├── NeuralMasteringModelMetadata.h
│   └── Dataset/
│       ├── NeuralMasteringDatasetSchema.h
│       ├── NeuralMasteringDatasetSchema.cpp
│       ├── NeuralMasteringFeatureExtractor.h
│       └── NeuralMasteringFeatureExtractor.cpp
└── Plugin/
    ├── PluginProcessor.h
    ├── PluginProcessor.cpp
    ├── PluginEditor.h
    └── PluginEditor.cpp

tests/
├── Unit/
│   ├── TestNeuralMasteringSafetyPolicy.cpp
│   ├── TestNeuralMasteringController.cpp
│   └── TestNeuralMasteringDatasetSchema.cpp
├── Integration/
│   └── TestVST3AudioSignalAccuracy.cpp
├── Performance/
│   └── BenchmarkSuite.cpp
└── CMakeLists.txt

scripts/
└── neural-mastering/
    ├── audit_dataset.py
    ├── extract_features.py
    ├── generate_mastering_dataset.py
    ├── train_neural_mastering.py
    ├── run_objective_metrics.py
    ├── benchmark_planner.py
    ├── generate_model_card.py
    └── validate_model_artifact.py

config/
└── neural-mastering/
    ├── safety-policy.default.json
    ├── dataset-governance.default.json
    └── poc-model.default.json
```

**Structure Decision**: Use the existing More-Phi layered layout. Core owns real-time-safe types, validation, smoothing, and deterministic DSP application. AI owns planner/controller/model-runner orchestration and dataset schema/tooling. Plugin owns lifecycle and UI/editor surfaces. Tests follow existing Catch2 unit/integration/performance organization. Scripts and config remain outside plugin runtime and are introduced only for proof-of-concept validation/governance.

## Phase 0: Research Decisions

Research is consolidated in [research.md](./research.md). Key resolved decisions:

1. Start implementation with deterministic schemas and Safety Layer projection rather than a model backend.
2. Represent neural mastering output as fixed-size bounded control plans, masks, confidence, and evidence metadata.
3. Keep direct neural audio processing and model inference outside `processBlock()` unless future measured gates pass.
4. Reuse existing analyzers and deterministic mastering processors as the Data Plane and fallback baseline.
5. Treat ONNX Runtime or any equivalent backend as optional proof-of-concept infrastructure only, disabled by default.
6. Require dataset governance and objective/listening validation artifacts before quality claims or runtime enablement.

## Phase 1: Design Outputs

- Data model: [data-model.md](./data-model.md)
- Plan contract schema: [contracts/neural-mastering-plan.schema.json](./contracts/neural-mastering-plan.schema.json)
- Model-card contract schema: [contracts/model-card.schema.json](./contracts/model-card.schema.json)
- Validation-gate contract: [contracts/validation-gates.md](./contracts/validation-gates.md)
- Quickstart: [quickstart.md](./quickstart.md)
- Neural training implementation plan: [neural-training-implementation-plan.md](./neural-training-implementation-plan.md)
- Agent context: `CLAUDE.md` Speckit pointer updated to `specs/003-neural-mastering-roadmap/plan.md`

## Post-Design Constitution Check

- Real-time safety: PASS. Design contracts require no model inference in the audio callback, reject unsafe plans before application, and keep fallback deterministic.
- Thread domains: PASS. Entities identify producer/consumer domains, and contracts distinguish offline/preview/background/message-thread/queued-control paths from audio-thread processing.
- Parameter safety: PASS. Data model and contracts include masks, ranges, max deltas, semantic type handling, validation gates, and fallback states.
- AI/MCP boundaries: PASS. Model metadata and evidence levels prevent unmeasured claims; UI/MCP review surfaces are explicitly status-first and cannot bypass Safety Layer validation.
- Persistence and compatibility: PASS. Initial design avoids APVTS/snapshot/hosted-state migration; optional model artifacts are separate, schema-versioned, and fallback-safe.
- Tests and validation: PASS. Quickstart and contracts define unit, integration, script, benchmark, dataset-governance, and listening-review gates before implementation approval.

## Complexity Tracking

No constitution violations are introduced by this plan.
