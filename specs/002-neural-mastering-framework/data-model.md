# Data Model: Neural Mastering Framework Audit

## Mastering Objective Profile

**Purpose**: Defines the sonic objectives the audit must evaluate and the quality risks for each objective.

**Fields**:

- `objective_name`: One of spectral balance, dynamic range control, stereo imaging, harmonic enhancement.
- `user_value`: Why the objective matters to a mastering or mixing user.
- `acceptable_change`: The kinds of changes considered musically valid.
- `failure_modes`: Artifacts or regressions that make the objective unsafe.
- `objective_metrics`: Quantitative checks proposed for the objective.
- `listening_checks`: Subjective review prompts for expert validation.
- `runtime_boundary`: Real-time, preview, offline, or unsuitable.

**Validation rules**:

- Each named objective from the feature spec must appear exactly once.
- Each objective must include at least one objective metric and one listening check.
- Failure modes must include both sonic artifacts and operational risks when applicable.

**Relationships**:

- Referenced by `Architecture Candidate`, `Recommended Framework`, and `Validation Gate`.

## Audio Modality Profile

**Purpose**: Captures the audio inputs, references, temporal context, and channel assumptions needed for the audit.

**Fields**:

- `channel_layouts`: Mono, stereo, and wider layouts considered by the audit.
- `sample_rates`: Sample-rate assumptions and validation rates.
- `block_sizes`: Low-latency buffer sizes and offline chunk sizes considered.
- `temporal_contexts`: Transient, phrase, section, and whole-track windows.
- `reference_material`: Required mix/master pairs, reference tracks, stems, or rendered examples.
- `metadata`: Optional genre, intent, plugin-chain, loudness target, and source-quality descriptors.
- `data_limitations`: Known gaps, licensing issues, sparse labels, or domain-shift risks.

**Validation rules**:

- Must include multi-channel PCM audio as the primary modality.
- Must distinguish real-time block sizes from offline or training chunks.
- Must document leakage risks for related renders, stems, and versions.

**Relationships**:

- Feeds `Training Roadmap`, `Architecture Candidate`, and `Risk Register`.

## Architecture Candidate

**Purpose**: Represents each model family or baseline compared by the audit.

**Fields**:

- `candidate_name`: Candidate model or baseline name.
- `candidate_class`: Causal convolution, TCN, attention, spectral encoder-decoder, hybrid baseline, or other.
- `intended_role`: Direct audio transformation, control recommendation, offline analysis, preview enhancement, or baseline comparison.
- `fidelity_potential`: Expected ability to meet professional audio quality.
- `latency_profile`: Expected real-time, preview, or offline latency behavior.
- `cpu_memory_profile`: Planning-level CPU and memory risk.
- `temporal_awareness`: Supported temporal context and receptive-field behavior.
- `training_complexity`: Dataset and optimization difficulty.
- `deployment_risk`: Integration, fallback, safety, and explainability concerns.
- `recommendation_status`: Recommended, fallback, constrained, rejected, or research-only.

**Validation rules**:

- At least five candidates must be represented.
- Each candidate must include a recommendation status and rationale.
- Rejected candidates must state the evidence or constraint that caused rejection.

**Relationships**:

- Consumes `Audio Modality Profile`; supports `Recommended Framework` and `Risk Register`.

## Recommended Framework

**Purpose**: Defines the selected architecture direction and fallback baseline for planning.

**Fields**:

- `primary_architecture`: Hybrid deterministic DSP plus compact causal TCN control-plane model.
- `fallback_baseline`: Deterministic DSP and rule-based/offline analysis baseline.
- `model_inputs`: Feature windows, context metadata, optional user intent, and reference descriptors.
- `model_outputs`: Bounded parameter targets, deltas, masks, plans, embeddings, and confidence scores.
- `application_boundary`: Offline, preview, background, message-thread, or queued low-rate control use.
- `prohibited_paths`: Direct audio-callback inference unless future proof gates pass.
- `safety_layer`: Masks, ranges, smoothing, max-delta limits, confidence gating, and fallback behavior.
- `acceptance_gates`: Linked validation gates required before implementation approval.

**Validation rules**:

- Must include one primary and one fallback direction.
- Must classify all runtime paths.
- Must include an explicit condition for rejecting or delaying direct neural audio processing.

**Relationships**:

- Built from `Architecture Candidate`; constrained by `Signal Integrity Policy`; evaluated by `Validation Gate`.

## Training Roadmap

**Purpose**: Captures how future experiments should train and validate the selected framework.

**Fields**:

- `dataset_sources`: Curated mix/master pairs, rendered plugin examples, generated datasets, references, and synthetic augmentations.
- `loss_stack`: Multi-resolution spectral, complex/phase, waveform, loudness, mid/side, dynamics, identity, true-peak, perceptual, and artifact penalties.
- `hyperparameter_ranges`: Context length, receptive field, depth, channel width, learning rate, weight decay, batch strategy, and stopping criteria.
- `curriculum_stages`: Conservative pairs, expanded transformations, genre/source diversity, difficult edge cases, and distillation.
- `augmentation_policy`: Gain, EQ tilt, dynamics, stereo width, pitch/time limits, resampling, and artifact augmentation rules.
- `split_policy`: Leakage-resistant separation by track, session, artist, plugin chain, render scenario, and genre where possible.
- `teacher_student_plan`: High-capacity offline teacher and compact student with measurable quality retention.

**Validation rules**:

- Must include at least three training stages and three validation stages.
- Must include both objective and subjective validation.
- Must define leakage-resistant split criteria.

**Relationships**:

- Uses `Audio Modality Profile`; produces evidence for `Validation Gate`; informs `Risk Register`.

## Signal Integrity Policy

**Purpose**: Defines safeguards required to prevent neural recommendations or audio transformations from damaging signal quality.

**Fields**:

- `gain_bounds`: Headroom, true-peak, loudness, and output ceiling requirements.
- `phase_bounds`: Stereo correlation, mono compatibility, inter-channel delay, and phase-rotation checks.
- `nonlinear_bounds`: Saturation, harmonic enhancement, aliasing, THD, IMD, and oversampling expectations.
- `transition_rules`: Bypass, wet/dry, parameter smoothing, latency compensation, and crossfade behavior.
- `invalid_output_rules`: NaN/Inf handling, out-of-range controls, late model output, and overload fallback.
- `fallback_modes`: Hold last safe controls, deterministic DSP, transparent bypass, or preview-only rejection.

**Validation rules**:

- Must cover clipping, phase, aliasing, bypass, CPU overload, and invalid output.
- Must define a safe behavior for every model failure mode.
- Must not require audio-thread blocking or allocation.

**Relationships**:

- Constrains `Recommended Framework`; audited through `Validation Gate`.

## Validation Gate

**Purpose**: Provides falsifiable acceptance criteria for future proof-of-concept and implementation decisions.

**Fields**:

- `gate_name`: Objective metric, listening validation, runtime performance, signal safety, fallback, or governance gate.
- `measurement_method`: How evidence is collected.
- `threshold`: Quantitative or pass/fail acceptance threshold.
- `evidence_level`: Research estimate, prototype measurement, automated test, benchmark, or blind listening panel.
- `decision`: Pass, fail, constrained, or requires more data.
- `residual_risk`: Remaining uncertainty after the gate.

**Validation rules**:

- Must include at least eight go/no-go targets.
- Must include objective audio metrics, subjective listening, latency/CPU, stability, bypass, artifact, and fallback gates.
- Must distinguish measured evidence from assumptions.

**Relationships**:

- Evaluates `Recommended Framework`, `Training Roadmap`, and `Signal Integrity Policy`.

## Risk Register

**Purpose**: Records blockers and uncertainties that could invalidate the recommendation.

**Fields**:

- `risk_id`: Stable identifier for traceability.
- `risk_area`: Data, model, runtime, signal quality, licensing, user trust, platform, or validation.
- `description`: Specific uncertainty or failure mode.
- `impact`: Consequence if the risk occurs.
- `likelihood`: Low, medium, high, or unknown.
- `mitigation`: Experiment, fallback, scope reduction, or governance action.
- `owner_role`: Product, DSP, ML, QA, legal, or release owner.
- `decision_deadline`: When the risk must be resolved before proceeding.

**Validation rules**:

- Must include data gaps, licensing constraints, real-time risks, sonic artifact risks, and proof-of-concept decisions.
- High-impact risks must have a mitigation or explicit no-go condition.

**Relationships**:

- Informed by all other entities and referenced by the final audit report.

## State Transitions

### Architecture Candidate Status

```text
proposed -> evaluated -> recommended
proposed -> evaluated -> fallback
proposed -> evaluated -> constrained
proposed -> evaluated -> rejected
proposed -> evaluated -> research-only
```

### Validation Gate Decision

```text
not-measured -> measured-pass
not-measured -> measured-fail
not-measured -> constrained
constrained -> measured-pass
constrained -> measured-fail
```

### Runtime Path Classification

```text
candidate -> offline-only
candidate -> preview-only
candidate -> background-control
candidate -> real-time-permitted
candidate -> unsuitable
```

Direct neural audio-callback inference can move to `real-time-permitted` only after every relevant validation gate is `measured-pass`.
