# Technical Implementation Strategy: Neural Mastering Roadmap

**Feature**: Neural Mastering Implementation Roadmap  
**Branch**: `003-neural-mastering-roadmap`  
**Created**: 2026-05-18  
**Source audit**: `specs/002-neural-mastering-framework/audit-report.md`  
**Evidence level**: planning / research-estimate only; no model quality, latency, CPU, or listening outcome is measured by this document.

## 1. Technical Implementation Architecture

### 1.1 Contract Boundary

The implementation must preserve the audit's core recommendation:

```text
Feature / context analysis outside audio callback
→ compact causal TCN or deterministic planner proposes bounded intent
→ deterministic Safety Layer validates and projects intent
→ deterministic mastering DSP applies only validated controls
→ fallback holds last safe controls, deterministic baseline, review-only suggestion, or transparent bypass
```

Direct neural audio transformation inside `MorePhiProcessor::processBlock()` remains rejected until future measured evidence proves fixed topology, fixed memory, no allocation, no locks, no I/O, no exceptions, no remote inference, and deterministic deadline headroom for the smallest supported DAW buffer.

### 1.2 Layered System Design

#### Layer A — Feature Acquisition

**Role**: Convert current audio, analyzer state, and user intent into low-rate feature frames for offline, preview, background, or message-thread planning.

**Integration points**:

- `src/Core/AutoMasteringEngine.h` / `.cpp`: already owns LUFS, true peak, spectrum, stereo-field, mono, dynamics, EQ, imaging, exciter, limiter, and normalizer components.
- `src/Core/RealtimeSpectrumAnalyzer.*`: spectral centroid and band summaries.
- `src/Core/StereoFieldAnalyzer.*`: correlation, mid/side, width, and polarity-sensitive stereo features.
- `src/Core/MeterWindowAccumulator.*`: short analysis windows for loudness and peak history.
- `src/AI/Dataset/FeatureExtractor.*`: offline dataset feature extraction path for training and proof-of-concept validation.

**Proposed core data structure**:

```cpp
struct NeuralMasteringFeatureFrame
{
    uint32_t schemaVersion;
    double sampleRate;
    int channelCount;
    int blockSize;
    uint64_t frameIndex;
    float integratedLUFS;
    float shortTermLUFS;
    float momentaryLUFS;
    float loudnessRange;
    float truePeakDbTp;
    float crestFactorDb;
    float spectralTilt;
    std::array<float, kSpectralBands> spectralBands;
    std::array<float, kStereoBands> stereoCorrelation;
    std::array<float, kStereoBands> midSideRatio;
    float monoFoldDownDeltaDb;
    float transientDensity;
    float harmonicRisk;
    float sourceQualityScore;
};
```

**Rules**:

- Audio-thread work may update existing analyzers only if those analyzers are already real-time safe.
- Any neural feature aggregation requiring heap allocation, large windows, file access, model input preparation, or metadata joins runs outside the audio callback.
- The first implementation should support mono and stereo explicitly and carry an unsupported-layout flag for wider formats.

#### Layer B — Control Plane Planner

**Role**: Produce proposed mastering intent, not audio samples. The first production-safe planner can be deterministic/rule-based; compact TCN inference remains optional and proof-gated.

**Integration points**:

- `src/AI/ChainPlanExecutor.*`: existing heuristic multi-effect planner; good baseline for deterministic fallback and plan shape alignment.
- `src/Core/NeuralCompressor.*`: existing model-loaded status and message-thread intelligence precedent.
- `src/AI/AutomationControlPlane.*`: existing bounded automation/control-plane concepts.
- Future optional `src/AI/NeuralMasteringModelRunner.*`: isolated model adapter for proof-of-concept inference outside the audio callback.

**Proposed core data structure**:

```cpp
struct NeuralMasteringPlanCandidate
{
    uint32_t schemaVersion;
    uint64_t planId;
    uint64_t producedAtFrame;
    uint64_t expiresAfterFrame;
    float confidence;
    bool abstain;
    bool reviewOnly;
    MasteringTargetVector targets;
    MasteringTargetVector deltas;
    MasteringControlMask editableMask;
    NeuralMasteringEvidence evidence;
    NeuralMasteringModelMetadata model;
};
```

**Planner output categories**:

- EQ targets and deltas.
- Dynamics targets and deltas.
- Stereo width and mono-safety targets.
- Harmonic enhancement enable/amount targets.
- Limiter and loudness-normalization goals.
- Masks for high-risk controls.
- Confidence and abstain state.
- Evidence level and model/provenance metadata.

#### Layer C — Safety Layer Projection

**Role**: Deterministically transform a candidate plan into an admissible control state or reject it.

**Integration points**:

- New `src/Core/NeuralMasteringSafetyPolicy.*`: central policy and projection logic.
- Existing mastering processors under `src/Core/`: target ranges and semantic constraints.
- `tests/Unit/TestAutomationControlPlane.cpp`: precedent for bounded automation tests.

**Proposed core data structures**:

```cpp
struct NeuralMasteringSafetyPolicy
{
    uint32_t schemaVersion;
    float minConfidence;
    uint32_t maxPlanAgeFrames;
    MasteringTargetVector minTargets;
    MasteringTargetVector maxTargets;
    MasteringTargetVector maxDeltaPerPlan;
    MasteringControlMask highRiskControls;
    SignalIntegrityThresholds signalThresholds;
};

struct ValidatedNeuralMasteringPlan
{
    uint64_t sourcePlanId;
    MasteringTargetVector projectedTargets;
    MasteringControlMask appliedMask;
    NeuralMasteringFallbackMode fallbackMode;
    std::array<ValidationGateResult, kValidationGateCount> gateResults;
    bool valid;
};
```

**Projection logic**:

1. Reject schema/version mismatch.
2. Reject NaN, Inf, denormal-risk, or out-of-range values.
3. Reject stale, low-confidence, unknown, or abstain plans unless marked review-only.
4. Apply high-risk control masks.
5. Clamp to policy ranges.
6. Enforce max-delta limits against last applied safe plan.
7. Preserve binary, discrete, enum, and continuous parameter semantics.
8. Require signal-integrity thresholds for true peak, mono compatibility, stereo correlation, low-frequency side energy, alias/IMD risk, pumping risk, and bypass continuity.
9. Publish a validated plan only if every required gate passes.
10. Otherwise choose deterministic baseline, last-safe hold, review-only result, or transparent bypass.

#### Layer D — Deterministic Data Plane Application

**Role**: Apply only validated plans through the existing deterministic mastering processors.

**Integration points**:

- `src/Core/AutoMasteringEngine.*`: add a non-audio-thread `applyValidatedPlan()` entry point and keep `processBlock()` deterministic.
- `src/Core/AdaptiveEQ.*`: bounded EQ targets.
- `src/Core/MultibandDynamicsProcessor.*`: dynamics targets.
- `src/Core/StereoImager.*`: stereo width targets with mono constraints.
- `src/Core/HarmonicExciter.*`: harmonic enhancement targets with alias/IMD safeguards.
- `src/Core/BrickwallLimiter.*`, `TruePeakEstimator.*`, `LUFSMeter.*`, `LoudnessNormalizer.*`: limiter/loudness constraints and validation.
- `src/Plugin/PluginProcessor.*`: maintain `AutoMasteringEngine` ownership and expose no direct neural callback path.

**Rules**:

- Plan application occurs on message thread, background-to-message handoff, offline render setup, or low-rate queue publication.
- `processBlock()` reads only prepared deterministic processor state and does not wait for planner work.
- Bypass, wet/dry, and target transitions must use deterministic smoothing or crossfades.
- Missing model artifacts must not change plugin state persistence semantics in the first phase.

#### Layer E — Validation and Governance

**Role**: Produce falsifiable evidence before enabling runtime neural behavior.

**Integration points**:

- `tests/Unit/TestAudioEngine.cpp`: current mastering processor tests.
- `tests/Unit/TestMCPServerUnit.cpp`: current mastering analyzer and candidate scoring coverage.
- `tests/Integration/TestVST3AudioSignalAccuracy.cpp`: signal-path validation precedent.
- `tests/Performance/BenchmarkSuite.cpp`: opt-in benchmark target.
- Future scripts under `scripts/neural-mastering/`: dataset audit, objective metric generation, model-card validation, and benchmark summary.

**Gate families**:

- G01 identity transparency.
- G02 composite objective quality.
- G03 blind/expert listening readiness and results.
- G04 latency boundary.
- G05 CPU headroom.
- G06 invalid output rejection.
- G07 bypass and transition safety.
- G08 artifact rate.
- G09 fallback behavior.
- G10 data governance.

### 1.3 Runtime Logic Flows

#### Offline / Preview Analysis Flow

```text
User requests neural mastering preview
→ collect full-track or section-level feature windows
→ run deterministic baseline and optional model planner outside audio callback
→ emit NeuralMasteringPlanCandidate with evidence metadata
→ Safety Layer validates candidate
→ preview renderer applies ValidatedNeuralMasteringPlan
→ objective metrics and review artifacts are generated
→ user may accept, reject, or compare against deterministic baseline
```

#### Background Recommendation Flow

```text
AutoMasteringEngine publishes analyzer snapshots
→ NeuralMasteringController samples low-rate features
→ deterministic planner or optional model runner proposes a plan
→ Safety Layer projects or rejects plan
→ accepted plan is published to message-thread applicator
→ AutoMasteringEngine applies smoothed targets
→ audio thread continues processing current deterministic state without blocking
```

#### Invalid Output Flow

```text
candidate contains invalid schema / NaN / stale timestamp / illegal mask / unsupported layout / low confidence
→ Safety Layer marks gate failure
→ no target is applied
→ fallback mode chooses review-only, last-safe hold, deterministic baseline, or bypass
→ diagnostic event is available to tests/UI/MCP without touching audio callback
```

#### Future Low-Rate Control Flow

```text
validated plan is converted to bounded parameter deltas
→ deltas are smoothed and published through existing safe handoff mechanisms
→ audio-rate processors consume only deterministic state
→ late planner output is ignored rather than forcing synchronization
```

## 2. Targeted File Manifest

This manifest lists the implementation target, not files created by this strategy document.

### `/src/Core`

| File | New / Modified | Role |
|------|----------------|------|
| `src/Core/NeuralMasteringTypes.h` | New | Shared fixed-size structs for feature frames, target vectors, masks, evidence metadata, gate results, fallback modes, and validated plans. |
| `src/Core/NeuralMasteringSafetyPolicy.h` | New | Public policy and projection API for validating plan candidates before application. |
| `src/Core/NeuralMasteringSafetyPolicy.cpp` | New | Deterministic validation logic for schema, finite values, ranges, masks, max deltas, confidence, stale plans, and signal thresholds. |
| `src/Core/NeuralMasteringPlanSmoother.h` | New | Bounded smoothing helper for control targets; precomputes state outside callback and exposes real-time-safe reads. |
| `src/Core/NeuralMasteringPlanSmoother.cpp` | New | Smoothing implementation and transition safety checks. |
| `src/Core/AutoMasteringEngine.h` | Modified | Add validated-plan application API, last-safe-plan snapshot access for tests, and feature-frame publication hook. |
| `src/Core/AutoMasteringEngine.cpp` | Modified | Apply validated controls on safe thread domain and preserve deterministic `processBlock()` behavior. |
| `src/Core/AdaptiveEQ.h/.cpp` | Modified | Expose bounded target application for spectral balance controls. |
| `src/Core/MultibandDynamicsProcessor.h/.cpp` | Modified | Expose bounded dynamics target application and gain-reduction safety metadata. |
| `src/Core/StereoImager.h/.cpp` | Modified | Expose mono-safe width target application with low-frequency side-energy constraints. |
| `src/Core/HarmonicExciter.h/.cpp` | Modified | Expose conservative harmonic target application with alias/IMD policy checks. |
| `src/Core/BrickwallLimiter.h/.cpp` | Modified | Expose limiter target validation and true-peak ceiling policy. |
| `src/Core/LoudnessNormalizer.h/.cpp` | Modified | Align loudness target changes with plan validation and smoothing. |
| `src/Core/RealtimeSpectrumAnalyzer.h/.cpp` | Modified | Provide stable feature snapshot fields needed by neural mastering feature frames. |
| `src/Core/StereoFieldAnalyzer.h/.cpp` | Modified | Provide stereo gate inputs for correlation, M/S ratio, and mono compatibility. |

### `/src/AI`

| File | New / Modified | Role |
|------|----------------|------|
| `src/AI/NeuralMasteringController.h` | New | Orchestrates low-rate feature sampling, planner invocation, safety projection, and message-thread application. |
| `src/AI/NeuralMasteringController.cpp` | New | Controller implementation with no audio-callback inference path. |
| `src/AI/NeuralMasteringModelRunner.h` | New | Optional model-adapter interface for proof-of-concept inference backends. |
| `src/AI/NeuralMasteringModelRunner.cpp` | New | Null/deterministic baseline runner first; optional backend adapter later. |
| `src/AI/NeuralMasteringModelMetadata.h` | New | Model provenance, schema version, supported sample rates, feature schema, and evidence metadata. |
| `src/AI/ChainPlanExecutor.h/.cpp` | Modified | Align deterministic fallback plan output with `NeuralMasteringPlanCandidate`. |
| `src/AI/MasteringCandidateScoring.h` | Modified | Extend scoring inputs for new gates without claiming model quality. |
| `src/AI/AutomationControlPlane.h/.cpp` | Modified | Reuse or bridge bounded control semantics for neural mastering plan application. |
| `src/AI/MCPToolsExtended.h/.cpp` | Modified | Future review-only endpoints for reporting plan status, evidence level, and gate failures; no direct unsafe parameter mutation. |

### `/src/AI/Dataset`

| File | New / Modified | Role |
|------|----------------|------|
| `src/AI/Dataset/NeuralMasteringDatasetSchema.h` | New | Dataset item, split, provenance, license, and feature/target schema for proof-of-concept training. |
| `src/AI/Dataset/NeuralMasteringDatasetSchema.cpp` | New | Schema validation helpers. |
| `src/AI/Dataset/NeuralMasteringFeatureExtractor.h` | New | Offline feature extraction wrapper aligned with runtime feature frame schema. |
| `src/AI/Dataset/NeuralMasteringFeatureExtractor.cpp` | New | Feature extraction implementation using existing analyzers where possible. |
| `src/AI/Dataset/FeatureExtractor.h/.cpp` | Modified | Bridge existing feature extraction to mastering-specific feature frames. |
| `src/AI/Dataset/ValidationEngine.h/.cpp` | Modified | Add provenance, license, split-leakage, and target-schema checks. |
| `src/AI/Dataset/ParameterSafetyConfig.h/.cpp` | Modified | Add mastering-specific high-risk control masks and range metadata. |

### `/src/Plugin`

| File | New / Modified | Role |
|------|----------------|------|
| `src/Plugin/PluginProcessor.h` | Modified | Own or expose the neural mastering controller only outside the audio callback; retain `AutoMasteringEngine` as DSP owner. |
| `src/Plugin/PluginProcessor.cpp` | Modified | Initialize controller in prepare/message-thread maintenance and ensure no planner work runs in `processBlock()`. |
| `src/Plugin/PluginEditor.h/.cpp` | Modified | Future UI status surface for plan confidence, review-only state, fallback state, and gate failures. |

### `/tests`

| File | New / Modified | Role |
|------|----------------|------|
| `tests/Unit/TestNeuralMasteringSafetyPolicy.cpp` | New | Unit tests for finite checks, range checks, masks, max deltas, stale plans, confidence gating, semantic validation, and fallback selection. |
| `tests/Unit/TestNeuralMasteringController.cpp` | New | Tests that controller never requires audio-callback inference and correctly applies/rejects plans through safe handoff. |
| `tests/Unit/TestNeuralMasteringDatasetSchema.cpp` | New | Dataset provenance, license, split isolation, schema, and unsupported-material tests. |
| `tests/Unit/TestAudioEngine.cpp` | Modified | Add plan-application and fallback regression cases for existing mastering chain. |
| `tests/Unit/TestMCPServerUnit.cpp` | Modified | Add review-only plan-status and gate-failure reporting checks if MCP surface is added. |
| `tests/Integration/TestVST3AudioSignalAccuracy.cpp` | Modified | Add bypass, true-peak, mono compatibility, and no-discontinuity checks for validated plan transitions. |
| `tests/Performance/BenchmarkSuite.cpp` | Modified | Add future opt-in benchmarks for planner/controller overhead outside the callback. |
| `tests/CMakeLists.txt` | Modified | Register new unit/integration test translation units. |

### `/scripts`

| File | New / Modified | Role |
|------|----------------|------|
| `scripts/neural-mastering/audit_dataset.py` | New | Validate provenance, license metadata, split leakage, and reference-quality tags before training. |
| `scripts/neural-mastering/extract_features.py` | New | Produce offline feature-frame fixtures aligned with runtime schema. |
| `scripts/neural-mastering/generate_mastering_dataset.py` | New | Generate aligned unmastered/mastered WAV pairs with synthesis, augmentation, LUFS metadata, and manifest output. |
| `scripts/neural-mastering/train_neural_mastering.py` | New | Train the offline hybrid CNN-Transformer mastering model with AMP, warmup-cosine scheduling, multi-resolution STFT/perceptual losses, and validation metrics. |
| `scripts/neural-mastering/run_objective_metrics.py` | New | Generate spectral, loudness, dynamics, stereo, true-peak, artifact, and identity metrics. |
| `scripts/neural-mastering/benchmark_planner.py` | New | Measure optional planner/model runner latency and CPU outside audio callback. |
| `scripts/neural-mastering/generate_model_card.py` | New | Create model card and evidence summary from metadata, gates, and metric outputs. |
| `scripts/neural-mastering/validate_model_artifact.py` | New | Check model artifact schema, supported sample rates, feature schema, and provenance before loading. |

### `/contracts`

| File | New / Modified | Role |
|------|----------------|------|
| `specs/003-neural-mastering-roadmap/contracts/neural-mastering-plan.schema.json` | New | JSON schema for serialized plan candidate fixtures, gate results, and review artifacts. |
| `specs/003-neural-mastering-roadmap/contracts/model-card.schema.json` | New | Model-card schema for provenance, evidence level, sample-rate support, backend assumptions, and no-go states. |
| `specs/003-neural-mastering-roadmap/contracts/validation-gates.md` | New | Human-readable gate contract aligned with G01-G10 from the audit report. |

### `/config`

| File | New / Modified | Role |
|------|----------------|------|
| `config/neural-mastering/safety-policy.default.json` | New | Default ranges, masks, max deltas, confidence thresholds, stale-plan limits, and signal thresholds. |
| `config/neural-mastering/dataset-governance.default.json` | New | Required provenance, license, split-isolation, and quality labels. |
| `config/neural-mastering/poc-model.default.json` | New | Optional proof-of-concept model metadata and supported runtime modes; disabled by default. |

### `/cmake` and root build files

| File | New / Modified | Role |
|------|----------------|------|
| `CMakeLists.txt` | Modified | Add new core, AI, and dataset source files to existing source groups. |
| `CMakePresets.json` | Modified only if needed | Add optional proof-of-concept toggles only after core safety tests exist. |
| `cmake/MorePhiNeuralMastering.cmake` | New optional | Isolate optional model-backend configuration if external inference support is introduced. |

### `/docs` and `/specs`

| File | New / Modified | Role |
|------|----------------|------|
| `specs/003-neural-mastering-roadmap/spec.md` | New | Stakeholder-facing requirements for the implementation strategy deliverable. |
| `specs/003-neural-mastering-roadmap/implementation-strategy.md` | New | This technical architecture, manifest, phased roadmap, and dependency map. |
| `specs/003-neural-mastering-roadmap/neural-training-implementation-plan.md` | New | Detailed offline model architecture, dataset generation pipeline, and PyTorch training framework design. |
| `specs/003-neural-mastering-roadmap/checklists/requirements.md` | New | Speckit quality checklist for the generated specification. |
| `docs/AI_ASSISTANT_TEST_PROMPTS.md` | Modified only if MCP/UI review surface ships | Add test prompts for plan status and fallback explanation. |

## 3. Phased Implementation Plan

### Phase 1 — Core Types and Safety Projection

**Goal**: Establish deterministic plan schemas, fixed-size data structures, safety policy, and rejection behavior before any model inference exists.

**Tasks**:

1. Add `NeuralMasteringTypes.h` with fixed-size target vectors, masks, metadata, gate IDs, and fallback modes.
2. Add `NeuralMasteringSafetyPolicy.*` with validation/project logic.
3. Add default safety-policy configuration and schema fixtures.
4. Add unit tests for invalid outputs, stale outputs, low confidence, masks, max deltas, unsupported layouts, and semantic control handling.
5. Add test fixtures for last-safe hold, deterministic baseline, review-only result, and transparent bypass.

**Success criteria**:

- 100% of malformed candidate fixtures are rejected without changing applied plan state.
- Every gate result includes a stable ID and pass/fail reason.
- No test requires a neural model artifact.
- Core safety tests run in `MorePhiTests`.

**Technical risks**:

- Overly broad target vectors can introduce future ABI/state pressure.
- Policy thresholds may be too permissive without prototype evidence.
- Fixed arrays must be sized conservatively to avoid hidden allocation pressure.

**Stop/go criteria**:

- Stop if invalid-output rejection is incomplete or if the policy requires audio-thread allocation/blocking.
- Proceed only when deterministic safety projection is fully testable without model inference.

### Phase 2 — Feature Frames and Deterministic Baseline Planner

**Goal**: Produce low-rate feature frames and baseline control candidates using existing analyzers and deterministic planning.

**Tasks**:

1. Add feature-frame extraction from `AutoMasteringEngine`, spectrum analyzer, stereo-field analyzer, LUFS meter, true-peak estimator, and meter windows.
2. Align `ChainPlanExecutor` output with `NeuralMasteringPlanCandidate`.
3. Add deterministic baseline candidate generation for spectral, dynamics, imaging, harmonic, limiter, and loudness targets.
4. Add tests for mono, stereo, silence, clipped, high-crest-factor, bass-heavy, and already-mastered material.
5. Add no-model fallback tests.

**Success criteria**:

- Feature frames are finite and schema-valid for supported mono/stereo buffers.
- Deterministic planner produces bounded candidates or abstains.
- Missing optional model state resolves to deterministic baseline or review-only output.

**Technical risks**:

- Analyzer snapshots may not contain all features needed by later model training.
- Stereo assumptions can leak into mono material.
- Baseline planner may appear authoritative if confidence/evidence labels are unclear.

**Stop/go criteria**:

- Stop if feature publication requires unsafe thread access.
- Proceed only when deterministic baseline and no-model fallback are observable and tested.

### Phase 3 — Safe Application to Mastering Chain

**Goal**: Apply validated plans to deterministic mastering processors through safe thread domains while preserving audio continuity.

**Tasks**:

1. Add `AutoMasteringEngine::applyValidatedPlan()` or equivalent non-audio-thread application API.
2. Add bounded target application to EQ, dynamics, stereo imager, harmonic exciter, limiter, and loudness normalizer.
3. Add smoothing/crossfade handling for plan transitions.
4. Add transition tests for bypass, wet/dry, max-delta, true-peak ceiling, mono compatibility, and no discontinuities.
5. Add integration tests ensuring `processBlock()` does not invoke planner or model code.

**Success criteria**:

- Validated plans update deterministic processor targets without audio-thread waiting.
- Invalid or late plans leave last safe state unchanged.
- Bypass and transition tests meet the release-defined discontinuity threshold.

**Technical risks**:

- Existing processors may lack public setters with sufficient semantic constraints.
- Transition smoothing can conflict with current timer/intelligence updates.
- Plan application might accidentally run from an unsafe thread.

**Stop/go criteria**:

- Stop if any application path can block, allocate, or synchronize with background planner work on the audio thread.
- Proceed only when plan application is deterministic and test-covered.

### Phase 4 — Dataset Governance and Offline Proof-of-Concept Tooling

**Goal**: Prepare data and objective-validation tooling before model training or quality claims.

**Tasks**:

1. Add dataset schema and governance checks for provenance, license, split isolation, reference quality, and unsupported material.
2. Add offline feature extraction scripts and fixtures aligned with runtime feature frames.
3. Add objective metric script covering spectral, phase/stereo, dynamics, loudness, true peak, identity, and artifact checks.
4. Add model-card and validation-gate artifact schemas.
5. Generate proof-of-concept reports without enabling runtime neural behavior.

**Success criteria**:

- 100% of training/evaluation items have provenance and license status.
- Split-leakage checks reject related renders, stems, alternate masters, plugin-chain variants, and artist/session overlap.
- Objective metrics are reproducible from scripted inputs.

**Technical risks**:

- Usable licensed reference data may be too small for professional claims.
- Objective metrics may reward loudness or spectral matching while missing perceptual defects.
- Python tooling can drift from runtime C++ feature schema if contracts are not enforced.

**Stop/go criteria**:

- Stop if provenance or leakage is unresolved.
- Proceed only when data governance artifacts can support Train-1/Validate-1 evidence.

### Phase 5 — Optional Model Runner and Compact TCN PoC

**Goal**: Add optional, disabled-by-default proof-of-concept model inference outside the audio callback.

**Tasks**:

1. Add `NeuralMasteringModelRunner` interface with null runner and deterministic baseline runner.
2. Add optional artifact validation for schema version, feature schema, supported sample rates, model metadata, and platform support.
3. Add compact TCN student runner only behind explicit proof-of-concept configuration.
4. Add benchmarks for planner/model-runner latency and CPU outside audio callback.
5. Compare TCN candidate output against deterministic baseline using objective metrics and review artifacts.

**Success criteria**:

- No model artifact is required for normal plugin operation.
- Optional model output cannot bypass Safety Layer validation.
- Benchmarks record worst-case planner/model time outside the callback.
- Student output reaches future-defined quality gates before runtime consideration.

**Technical risks**:

- External inference backend can introduce binary size, licensing, platform, or CPU variance risk.
- Model output calibration may be poor even when objective metrics improve.
- Backend initialization could accidentally affect plugin startup or state restore.

**Stop/go criteria**:

- Stop if model runner requires audio-thread inference, remote inference for playback, hidden allocation in callback, or unsupported platform behavior.
- Proceed only when optional runner remains disabled by default and fully fallback-safe.

### Phase 6 — Runtime Review Surface and MCP/UI Reporting

**Goal**: Present safe, auditable status to users and assistants without overstating neural capability.

**Tasks**:

1. Add review-only status for plan confidence, evidence level, gate failures, fallback state, and abstention reason.
2. Add UI status in `PluginEditor` only after core safety and application tests pass.
3. Add MCP reporting endpoints only for status and review artifacts unless separate authorization approves bounded parameter application.
4. Add assistant prompt tests for fallback explanations and no-go messaging.

**Success criteria**:

- Users can distinguish deterministic baseline, review-only neural suggestion, validated applied plan, and fallback state.
- Low-confidence or abstain states are visible and not framed as professional recommendations.
- MCP/status outputs do not mutate unsafe audio state.

**Technical risks**:

- UX could imply measured neural quality before evidence exists.
- MCP tools could become an unsafe write path if not scoped tightly.
- Status noise may overwhelm users during normal playback.

**Stop/go criteria**:

- Stop if UI/MCP wording presents unmeasured behavior as proven capability.
- Proceed only after product/DSP/QA review of evidence labels and no-go messaging.

### Phase 7 — Release Readiness Decision

**Goal**: Decide whether the neural mastering feature can advance beyond proof-of-concept.

**Tasks**:

1. Run G01-G10 gate review with automated tests, benchmarks, objective metrics, listening artifacts, and governance reports.
2. Confirm deterministic fallback on all target platforms and DAW buffer scenarios.
3. Confirm no audio-callback inference path is enabled unless future stricter proof passes.
4. Produce release/no-release recommendation and residual-risk list.

**Success criteria**:

- Every gate has pass/fail evidence and owner signoff.
- No high-severity artifact class remains unresolved.
- Fallback behavior is validated for missing, late, invalid, overloaded, and low-confidence model scenarios.

**Technical risks**:

- Platform variance can invalidate CPU/headroom assumptions.
- Listening tests may reveal defects not captured by objective metrics.
- Legal/data governance may block model release despite technical progress.

**Stop/go criteria**:

- No-go if any G04-G09 runtime or signal-safety gate fails.
- No-go for professional quality claims if G01-G03 or G10 are incomplete.

## 4. Dependency Mapping

### Internal Dependencies

| Dependency | Required initialization/update | Supports | Failure impact |
|------------|--------------------------------|----------|----------------|
| `AutoMasteringEngine` | Add validated-plan application and feature-frame publication outside callback. | Data-plane DSP, analyzer access, fallback baseline. | No safe integration point for validated plans. |
| `RealtimeSpectrumAnalyzer` | Ensure stable snapshots expose bands/tilt required by feature schema. | Spectral balance features and gates. | Model/baseline lacks spectral context. |
| `StereoFieldAnalyzer` | Ensure correlation, width, M/S, mono-safety data are accessible. | Stereo imaging and mono compatibility gates. | Width plans may be unsafe. |
| `LUFSMeter` / `MeterWindowAccumulator` | Provide loudness windows and integrated/short-term metrics. | Loudness, identity, dynamics, and objective metrics. | Loudness-only mistakes or missing gate evidence. |
| `TruePeakEstimator` / `BrickwallLimiter` | Expose true-peak ceiling validation and limiter state. | True-peak and clipping safeguards. | Plans can cause over/ISP violations. |
| `AdaptiveEQ`, `MultibandDynamicsProcessor`, `StereoImager`, `HarmonicExciter`, `LoudnessNormalizer` | Add bounded setters or target-application methods with semantic constraints. | Deterministic application of validated plans. | Plan cannot be applied safely or audibly smoothly. |
| `ChainPlanExecutor` | Align deterministic plans with candidate schema. | Baseline/fallback and ablation comparison. | No safe no-model baseline for product operation. |
| `AutomationControlPlane` | Reuse bounded automation semantics where compatible. | Safe parameter/control publication. | Duplicate unsafe control logic may emerge. |
| `DatasetGeneratorV3` / dataset modules | Add governance and feature/target schema validation. | Training and proof-of-concept data pipeline. | Data leakage/licensing risks remain unresolved. |
| `PluginProcessor` | Own controller lifecycle and enforce no planner work in `processBlock()`. | Runtime integration and thread-domain enforcement. | Neural work could enter unsafe callback path. |
| `MCPToolsExtended` / assistant surfaces | Add status-only reporting after safety foundation exists. | Review, explainability, and assistant workflow. | User trust risk or unsafe remote-triggered mutation. |
| `tests/CMakeLists.txt` / Catch2 test target | Register new tests. | Regression and gate validation. | Core safety logic remains untested in CI/local builds. |

### Optional External Dependencies

| Dependency | Initialization/update | Scope | Release posture |
|------------|-----------------------|-------|-----------------|
| ONNX Runtime or equivalent CPU inference backend | Add only behind explicit opt-in CMake/config after Phase 1-4 pass. | Optional PoC model runner outside audio callback. | Disabled by default until platform, license, binary-size, and CPU evidence pass. |
| Python scientific stack | Use for scripts that audit datasets, extract offline features, compute metrics, and generate model cards. | Offline proof-of-concept and validation. | Not required for plugin runtime. |
| Model artifact format | Define schema, feature layout, supported sample rates, model-card metadata, and checksum/provenance requirements. | Optional proof-of-concept runner. | Invalid artifacts rejected and never required for fallback operation. |
| Listening-test tooling | Produce level-matched review packages and collect blind/expert ratings. | G03 evidence. | Required before professional quality claims. |

### Build and Configuration Dependencies

- Add core and AI source files to existing `CMakeLists.txt` groups only after Phase 1 source files exist.
- Keep optional model backend behind an explicit build option such as `MORE_PHI_ENABLE_NEURAL_MASTERING_POC`, default `OFF`.
- Keep default runtime configuration model-free and deterministic.
- Add JSON config loading only outside the audio callback, with validated defaults compiled or embedded for fallback.
- Do not modify plugin state persistence for model artifacts until release-readiness decisions approve compatibility behavior.

## 5. Traceability to Audit Contract

| Strategy area | Audit traceability |
|---------------|--------------------|
| Control Plane / Data Plane split | Executive Summary, Section 5, Safety Layer Section 6 |
| Direct audio-callback inference rejection | Sections 1, 5, 8, 9; Gates G04-G09 |
| Deterministic fallback baseline | Sections 1, 4, 5, 8, 10 |
| Safety policy projection | Section 6 and Signal Integrity Policy |
| Dataset governance | Sections 3, 7, 9, 10; Gate G10 |
| Objective and listening validation | Sections 2, 7, 9, 10; Gates G01-G03/G08 |
| Runtime transition and fallback safety | Sections 8 and 9; Gates G06-G09 |
| Risks and no-go decisions | Sections 9 and 10; Risk Register R01-R08 |

## 6. Immediate Next Actions

1. Generate `/speckit-plan` for this feature if implementation planning should proceed.
2. Treat Phase 1 as the only safe coding starting point: core types, safety projection, and rejection tests.
3. Do not add a model backend before deterministic safety and dataset governance are complete.
4. Keep all neural inference outside `processBlock()` unless a later measured proof explicitly overturns the current no-go condition.
5. Use `neural-training-implementation-plan.md`, `generate_mastering_dataset.py`, and `train_neural_mastering.py` for offline model research only until validation gates approve any product-facing runtime path.
