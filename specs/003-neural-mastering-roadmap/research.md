# Phase 0 Research: Neural Mastering Implementation Roadmap

**Feature**: Neural Mastering Implementation Roadmap  
**Date**: 2026-05-18  
**Inputs**: `spec.md`, `implementation-strategy.md`, `specs/002-neural-mastering-framework/audit-report.md`, More-Phi constitution

## Decision 1: Implement deterministic safety projection before any model backend

**Decision**: Phase 1 starts with `NeuralMasteringTypes` and `NeuralMasteringSafetyPolicy` so every plan candidate can be rejected, projected, or routed to fallback without requiring a neural model artifact.

**Rationale**: The audit requires bounded, auditable control publication and rejects direct neural audio-callback inference. Building validation first prevents optional model work from defining unsafe behavior and makes invalid-output testing possible immediately.

**Alternatives considered**:

- Start with compact TCN inference: rejected because model output would lack an enforced admissible set and would risk overfitting implementation to unvalidated artifacts.
- Extend existing heuristic planner only: insufficient because the feature needs explicit schemas, evidence metadata, gate results, and no-go/fallback behavior independent of planner implementation.

## Decision 2: Use fixed-size control-plan data structures

**Decision**: Represent feature frames, target vectors, masks, gate results, and validated plans with bounded/fixed-size storage in Core-level types.

**Rationale**: Fixed-size structures align with More-Phi's real-time safety constitution, make validation deterministic, and avoid hidden allocation pressure when validated control state is read by audio-rate processors.

**Alternatives considered**:

- Dynamic maps keyed by control names: rejected for runtime paths because they create allocation and lookup variability.
- Free-form JSON as runtime representation: useful for contracts and fixtures, but rejected for audio/control handoff because parsing and dynamic allocation belong outside callback-adjacent paths.

## Decision 3: Keep learned planning outside `processBlock()`

**Decision**: Optional learned planning, including any compact causal TCN runner, is restricted to offline, preview, background, message-thread, or queued low-rate control contexts. The audio callback consumes only deterministic processor state.

**Rationale**: The audit and constitution prohibit model inference in the audio callback unless future measured proof satisfies fixed topology, fixed memory, no allocation, no locks, no I/O, no exceptions, no remote dependency, and buffer deadline headroom.

**Alternatives considered**:

- Audio-callback inference with a small model: rejected because no measured CPU/latency proof exists and platform variance can create dropouts.
- Remote inference for live mastering: rejected for real-time audio because network dependency and latency are incompatible with deterministic DAW operation.

## Decision 4: Reuse existing analyzers and mastering processors as the integration foundation

**Decision**: Feature frames and validated control application should integrate with `AutoMasteringEngine`, `RealtimeSpectrumAnalyzer`, `StereoFieldAnalyzer`, `LUFSMeter`, `TruePeakEstimator`, `BrickwallLimiter`, `AdaptiveEQ`, `MultibandDynamicsProcessor`, `StereoImager`, `HarmonicExciter`, and `LoudnessNormalizer` before introducing new DSP.

**Rationale**: These components already define More-Phi's mastering signal path, metering, and safety-relevant state. Reuse reduces duplicate DSP behavior and supports deterministic fallback/ablation against the current mastering chain.

**Alternatives considered**:

- Build a parallel neural mastering signal path: rejected because it duplicates deterministic DSP, increases bypass/latency risk, and conflicts with the audit's Data Plane recommendation.
- Implement feature extraction only in offline Python: useful for training, but insufficient for runtime review/preview parity with plugin analyzers.

## Decision 5: Treat ONNX Runtime or equivalent model backends as optional proof-of-concept dependencies

**Decision**: Do not make any external inference backend a required dependency for the first implementation phases. If introduced later, hide it behind an explicit opt-in build/config path and artifact validation.

**Rationale**: More-Phi must remain functional without neural artifacts. Optionalizing model backends avoids platform, binary size, licensing, initialization, and CPU variance risks before data governance and safety validation are complete.

**Alternatives considered**:

- Add ONNX Runtime as a default dependency immediately: rejected because it couples runtime delivery to unproven model behavior.
- Use a bespoke inference engine first: rejected because it increases maintenance burden and still requires the same safety gates.

## Decision 6: Version model artifacts and contracts separately from plugin state

**Decision**: Model cards, plan fixtures, safety policy defaults, and proof-of-concept metadata are separate schema-versioned artifacts; initial phases do not change APVTS, snapshot, hosted-plugin state, or preset compatibility.

**Rationale**: The audit does not authorize production neural persistence changes. Separate artifacts allow validation and governance without forcing DAW project compatibility changes.

**Alternatives considered**:

- Store model state in plugin presets immediately: rejected because model availability and compatibility are unproven.
- Use unversioned local files: rejected because reproducible validation and fallback decisions require schema and provenance metadata.

## Decision 7: Gate data work before model quality claims

**Decision**: Dataset schema, provenance, license status, split isolation, reference quality, and leakage checks are required before Train-1/Validate-1 proof-of-concept work can support any quality claim.

**Rationale**: The audit identifies sparse, restricted, or leakage-prone reference data as a high-impact risk. Professional mastering claims require legally usable and leakage-resistant evaluation material.

**Alternatives considered**:

- Train first and audit data later: rejected because leakage or licensing failure would invalidate results.
- Use synthetic-only data for professional claims: rejected because synthetic renders are useful baselines but cannot prove professional mastering quality alone.

## Decision 8: Define contracts for plan fixtures, model cards, and validation gates

**Decision**: Create JSON schemas for plan candidates and model cards, plus a human-readable validation-gates contract aligned with audit gates G01-G10.

**Rationale**: Contracts let tests, scripts, review artifacts, and future tooling validate the same data shapes without requiring runtime implementation first.

**Alternatives considered**:

- Document contracts only in prose: insufficient for fixtures and script validation.
- Generate contracts from future C++ types: useful later, but current planning needs stable reviewable artifacts now.

## Decision 9: UI and MCP surfaces must be status-first

**Decision**: Any future UI or MCP addition should initially report plan status, evidence level, confidence, gate failures, fallback state, and review-only recommendations rather than directly mutating unsafe audio state.

**Rationale**: The audit identifies user trust as a risk. Status-first surfaces make abstention and fallback visible and avoid presenting unmeasured AI output as authoritative.

**Alternatives considered**:

- Add one-click neural apply from MCP/UI immediately: rejected until bounded application, validation gates, and user-facing evidence wording are complete.
- Hide low-confidence/fallback states: rejected because it undermines trust and makes debugging unsafe behavior harder.
