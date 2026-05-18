# Data Model: Neural Mastering Implementation Roadmap

**Feature**: Neural Mastering Implementation Roadmap  
**Date**: 2026-05-18

## Entity: Implementation Architecture

**Purpose**: Captures the selected engineering shape for converting neural mastering audit guidance into executable work.

**Fields**:

- `architectureId`: Stable identifier for the architecture record.
- `evidenceLevel`: Planning, research-estimate, prototype-measured, or production-measured.
- `controlPlaneModes`: Allowed planner execution modes: offline, preview, background, message-thread, queued low-rate control.
- `dataPlaneOwner`: Deterministic mastering processor owner that generates audio-rate samples.
- `directAudioCallbackInferenceAllowed`: Boolean, default false.
- `fallbackBaseline`: Deterministic DSP/rule-based baseline description.
- `noGoConditions`: References to gate IDs and hard rejection conditions.

**Relationships**:

- Owns many `RoadmapPhase` records.
- References many `DependencyMapEntry` records.
- Produces `ValidationGate` requirements.

**Validation rules**:

- `directAudioCallbackInferenceAllowed` must remain false unless all runtime and signal-safety gates have measured-pass evidence.
- `fallbackBaseline` is required before optional model work can begin.
- `evidenceLevel` must not be higher than available validation artifacts.

## Entity: NeuralMasteringFeatureFrame

**Purpose**: A bounded low-rate analysis snapshot used by deterministic or optional learned planners outside the audio callback.

**Fields**:

- `schemaVersion`: Feature-frame schema version.
- `sampleRate`: Source sample rate.
- `channelCount`: Number of channels represented.
- `blockSize`: Associated processing block size or analysis hop.
- `frameIndex`: Monotonic frame counter or analysis position.
- `layoutClass`: Mono, stereo, wider-supported, or unsupported.
- `loudness`: Integrated, short-term, momentary, and loudness-range measurements.
- `truePeakDbTp`: True-peak estimate.
- `crestFactorDb`: Crest-factor estimate.
- `spectralSummary`: Fixed band values and tilt/centroid summaries.
- `stereoSummary`: Correlation, mid/side ratio, width, and mono fold-down indicators.
- `transientSummary`: Transient density and dynamics-envelope indicators.
- `harmonicRisk`: Conservative alias/IMD or high-frequency residual risk indicator.
- `sourceQualityFlags`: Silence, clipping, already-mastered, low-confidence, unsupported-layout, or invalid flags.

**Relationships**:

- Feeds `ControlPlanCandidate` generation.
- Supplies signal checks for `SafetyPolicy` and `ValidationGate` evaluation.

**Validation rules**:

- All numeric values must be finite.
- Unsupported layouts must be flagged rather than coerced into stereo assumptions.
- Feature extraction that requires allocation, I/O, large windows, or metadata joins must occur outside the audio callback.

## Entity: ControlPlanCandidate

**Purpose**: A proposed mastering intent payload produced by deterministic or optional learned planners.

**Fields**:

- `schemaVersion`: Candidate schema version.
- `planId`: Unique plan identifier.
- `producedAtFrame`: Publication position.
- `expiresAfterFrame`: Staleness deadline.
- `runtimeMode`: Offline, preview, background, message-thread, queued-control, or audio-callback-prohibited.
- `confidence`: Planner confidence from 0.0 to 1.0.
- `abstain`: Whether the planner declines to recommend changes.
- `reviewOnly`: Whether output may be shown but not applied.
- `evidenceLevel`: Evidence level for the plan source.
- `targetVector`: Bounded desired mastering controls.
- `deltaVector`: Bounded change request relative to previous safe state.
- `editableMask`: Controls the planner is allowed to modify.
- `modelMetadata`: Optional reference to model/provenance metadata.
- `diagnostics`: Optional status and rejection hints for review artifacts.

**Relationships**:

- Consumes one or more `NeuralMasteringFeatureFrame` records.
- Is validated by a `SafetyPolicy`.
- Produces zero or one `ValidatedControlPlan` records.

**Validation rules**:

- Must be rejected on schema mismatch, NaN, Inf, out-of-range target, illegal mask, stale timestamp, unsupported semantic type, or low confidence when application is requested.
- Must not contain audio buffers or direct audio transformation payloads.
- Review-only and abstain candidates must not be applied to the Data Plane.

## Entity: ValidatedControlPlan

**Purpose**: The only plan shape eligible for deterministic processor application.

**Fields**:

- `sourcePlanId`: Candidate source identifier.
- `validatedAtFrame`: Validation position.
- `projectedTargetVector`: Target vector after range, mask, max-delta, and signal projection.
- `appliedMask`: Controls permitted after projection.
- `fallbackMode`: None, last-safe-hold, deterministic-baseline, review-only, transparent-bypass, or reject.
- `gateResults`: Validation results for relevant gates.
- `valid`: Whether the plan can be applied.
- `reason`: Human-readable pass/fail or fallback reason.

**Relationships**:

- Produced from one `ControlPlanCandidate` and one `SafetyPolicy`.
- May update one `FallbackState` after application or rejection.
- Is consumed by deterministic mastering processors only through safe thread domains.

**Validation rules**:

- `valid` can be true only when every required gate passes.
- `projectedTargetVector` must preserve semantic control types and max deltas.
- Invalid plans must leave last-safe state unchanged.

## Entity: SafetyPolicy

**Purpose**: Defines the admissible control set and fallback projection behavior.

**Fields**:

- `schemaVersion`: Policy schema version.
- `minConfidence`: Minimum confidence for application.
- `maxPlanAgeFrames`: Staleness threshold.
- `targetRanges`: Minimum and maximum values for each control.
- `maxDeltaPerPlan`: Maximum per-plan movement for each control.
- `highRiskControlMask`: Controls that cannot change unless explicitly enabled.
- `semanticTypes`: Continuous, binary, discrete, enum, decibel, frequency, width, gain, or bypass semantics.
- `signalThresholds`: True peak, mono compatibility, stereo correlation, low-frequency side energy, alias/IMD, pumping, transition, and latency thresholds.
- `fallbackPriority`: Ordered fallback behavior when projection fails.

**Relationships**:

- Validates `ControlPlanCandidate` records.
- Generates `ValidationGate` results.
- Updates `FallbackState` behavior.

**Validation rules**:

- Policy loading and parsing must not occur in the audio callback.
- Defaults must be available when external config is missing or invalid.
- Signal thresholds must be conservative until measured evidence supports loosening.

## Entity: FallbackState

**Purpose**: Tracks the safe response when plans are missing, late, invalid, unsupported, low-confidence, or overloaded.

**Fields**:

- `lastSafePlan`: Last validated and applied control plan.
- `lastFallbackMode`: Most recent fallback mode.
- `lastFailureGate`: Gate responsible for the latest rejection.
- `modelAvailable`: Whether optional model artifact is available and valid.
- `plannerOverloaded`: Whether planner/backend is too slow or unavailable.
- `transparentBypassRequested`: Whether bypass fallback is required.
- `reviewOnlyMessage`: User/assistant-facing status text.

**Relationships**:

- Updated by `SafetyPolicy` validation.
- Reported by future UI/MCP status surfaces.
- Referenced by `ValidatedControlPlan` application.

**Validation rules**:

- Fallback must never require blocking audio-thread work.
- Missing model must resolve to deterministic DSP baseline or disabled neural behavior.
- Fallback state must not imply measured quality if only planning/research evidence exists.

## Entity: FileManifestEntry

**Purpose**: Records the implementation role and validation ownership for each targeted artifact.

**Fields**:

- `path`: Project-relative file path.
- `category`: Source, test, script, contract, config, documentation, or build.
- `changeType`: New, modified, optional, or future-gated.
- `role`: Core logic, integration, validation, proof-of-concept research, release readiness, or review surface.
- `ownerDomain`: Core, AI, Dataset, Plugin, UI, Tests, Scripts, Build, or Specs.
- `validationRequirement`: Required test, script, benchmark, review, or gate.
- `phaseIntroduced`: Roadmap phase where the artifact becomes relevant.

**Relationships**:

- Belongs to a `RoadmapPhase`.
- May satisfy one or more `DependencyMapEntry` requirements.

**Validation rules**:

- Every new runtime source file must have at least one test or explicit validation owner.
- Optional model-backend files must be marked future-gated until safety and governance phases pass.

## Entity: RoadmapPhase

**Purpose**: A milestone with tasks, success criteria, risks, and stop/go rules.

**Fields**:

- `phaseId`: Ordered phase identifier.
- `name`: Phase title.
- `goal`: Outcome delivered by the phase.
- `tasks`: Concrete implementation or validation tasks.
- `successCriteria`: Measurable completion criteria.
- `technicalRisks`: Known implementation risks.
- `dependencies`: Internal/external dependencies.
- `stopGoCriteria`: Conditions that block advancement.
- `gateCoverage`: Gate IDs addressed by the phase.

**Relationships**:

- Contains many `FileManifestEntry` records.
- Consumes many `DependencyMapEntry` records.
- Produces evidence for `ValidationGate` records.

**Validation rules**:

- Each phase must be independently reviewable.
- Model-runner phases must remain blocked until safety and governance phases pass.
- Release-readiness phase must include all G01-G10 gates.

## Entity: DependencyMapEntry

**Purpose**: Captures internal subsystem or optional external dependency requirements.

**Fields**:

- `name`: Dependency name.
- `type`: Internal module, external library, script stack, contract, config, or review process.
- `requiredUpdate`: Initialization, adapter, schema, test, config, or documentation change.
- `supports`: Feature capability enabled by the dependency.
- `failureImpact`: What breaks or remains blocked if unavailable.
- `phaseNeeded`: Earliest phase requiring the dependency.
- `releasePosture`: Required, optional, disabled-by-default, future-gated, or review-only.

**Relationships**:

- Referenced by `RoadmapPhase` records.
- May constrain `ImplementationArchitecture` decisions.

**Validation rules**:

- Optional external inference dependencies must not be required for normal plugin operation.
- Dependencies touching `processBlock()`-reachable code must include real-time safety validation.

## Entity: ValidationGate

**Purpose**: A falsifiable decision point that controls phase advancement or release readiness.

**Fields**:

- `gateId`: Stable gate identifier, aligned with G01-G10 where applicable.
- `area`: Identity, quality, listening, latency, CPU, stability, bypass, artifact, fallback, or governance.
- `measurementMethod`: Automated test, script metric, benchmark, listening review, or stakeholder review.
- `threshold`: Pass/fail threshold.
- `evidenceLevel`: Current evidence level.
- `decisionRule`: Proceed, review-only, fallback-only, no-go, or release-approved.
- `ownerRole`: DSP, ML, QA, product, legal/release, or build owner.
- `residualRisk`: Known risk after passing.

**Relationships**:

- Evaluates `ValidatedControlPlan`, `RoadmapPhase`, `ModelArtifact`, and dataset governance outputs.
- Informs `FallbackState` and release readiness.

**Validation rules**:

- Gates must be falsifiable and must not claim measured evidence before measurement exists.
- Runtime gates G04-G09 must pass before any neural runtime enablement beyond deterministic/review-only behavior.
- G10 must pass before professional model quality claims.

## Entity: ModelArtifactMetadata

**Purpose**: Describes optional proof-of-concept model artifacts and their evidence boundaries.

**Fields**:

- `modelId`: Stable model identifier.
- `schemaVersion`: Artifact metadata schema version.
- `featureSchemaVersion`: Required feature-frame schema.
- `outputSchemaVersion`: Required plan-candidate schema.
- `supportedSampleRates`: Declared sample-rate support.
- `supportedLayouts`: Mono, stereo, or wider layout support.
- `evidenceLevel`: Current proof level.
- `trainingDataManifest`: Reference to dataset provenance summary.
- `licenseStatus`: Approved, restricted, unresolved, or rejected.
- `checksum`: Integrity check.
- `backend`: Optional inference backend requirement.
- `runtimeModes`: Allowed modes, excluding audio callback unless future gates pass.

**Relationships**:

- Referenced by `ControlPlanCandidate` and model-card contracts.
- Validated by dataset governance and artifact validation scripts.

**Validation rules**:

- Missing or invalid artifacts must not block deterministic plugin operation.
- Artifact evidence level must not exceed available gates.
- Unsupported sample rates/layouts require abstain or fallback.

## State Transitions

### Control plan lifecycle

```text
Draft candidate
→ schema validated
→ finite/range/mask/semantic checks
→ confidence/staleness/runtime checks
→ signal-integrity checks
→ projected valid plan OR rejected fallback
→ deterministic application OR review-only/fallback report
→ last-safe state update only after successful application
```

### Optional model artifact lifecycle

```text
Unavailable
→ discovered outside audio callback
→ schema/provenance/checksum validated
→ supported runtime modes checked
→ enabled for proof-of-concept planner only
→ benchmarked and gate-reviewed
→ remains disabled/review-only unless release gates pass
```

### Roadmap phase lifecycle

```text
Pending
→ dependencies available
→ tasks completed
→ validation evidence attached
→ gates pass
→ next phase unlocked
OR gate fails → fallback/review-only/no-go decision
```
