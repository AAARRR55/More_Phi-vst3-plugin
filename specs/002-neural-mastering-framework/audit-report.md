# Technical Specification and Feasibility Audit: Neural Mastering Framework

## Report Metadata

- `feature`: `Neural Mastering Framework`
- `branch`: `002-neural-mastering-framework`
- `created_or_updated_date`: `2026-05-18`
- `report_owner_role`: Senior Principal ML Architect / DSP Systems Auditor
- `evidence_level`: `research-estimate`
- `scope_statement`: This document is a gatekeeper audit, feasibility specification, and implementation roadmap for neural mastering in More-Phi. It is not a production implementation, does not authorize inference inside the audio callback, does not claim measured model performance, and does not expand product capability beyond the referenced planning artifacts.

## Source Material, Evidence Rules, and Audit Trail

Primary source artifacts:

- Feature requirements: `specs/002-neural-mastering-framework/spec.md`
- Research decisions: `specs/002-neural-mastering-framework/research.md`
- Data model: `specs/002-neural-mastering-framework/data-model.md`
- Report contract: `specs/002-neural-mastering-framework/contracts/audit-report-contract.md`
- Planning review: `specs/002-neural-mastering-framework/quickstart.md`

Evidence levels used in this report:

- `planning`: Requirement, architecture, scope boundary, runtime classification, or review structure derived from Speckit planning artifacts.
- `research-estimate`: Technically justified recommendation based on audio-DSP, ML, DAW-host, and More-Phi architectural constraints, but not measured in this feature.
- `prototype-measured`: Reserved for future proof-of-concept benchmarks, controlled listening evidence, or model-validation experiments.
- `production-measured`: Reserved for production telemetry, release validation, and repeatable release benchmarks.

Unless explicitly labeled as a future measurement target, every conclusion in this report remains `planning` or `research-estimate`. No neural behavior, sonic improvement, latency margin, or CPU budget is represented as measured evidence by this document.

Traceability shorthand:

- Requirements: `FR-001` through `FR-016` from `spec.md`.
- Research: `Decision 1` through `Decision 6` from `research.md`.
- Data model entities: Mastering Objective Profile, Audio Modality Profile, Architecture Candidate, Recommended Framework, Training Roadmap, Signal Integrity Policy, Validation Gate, Risk Register.

Continuous audit trail model:

| Audit object | Governing source | Evidence constraint | Downstream decision artifact |
|--------------|------------------|---------------------|------------------------------|
| Product objective | `FR-001`, `FR-002`, Mastering Objective Profile | `planning` | Objective-specific metrics, listening prompts, and runtime boundary classification |
| Architecture selection | `FR-006`, `FR-007`, `Decision 1`, `Decision 2` | `research-estimate` | Candidate matrix, Recommended Framework, fallback baseline |
| Training strategy | `FR-009`, `FR-010`, `FR-011`, `Decision 3`, `Decision 4` | `research-estimate` | Composite loss stack, curriculum, distillation roadmap |
| Runtime boundary | `FR-005`, `FR-012`, `Decision 5`, `Decision 6` | `planning` plus future measurements | Safety Layer, Signal Integrity Policy, Go/No-Go Gates |
| Product authorization | `FR-013`, `FR-014`, `FR-015`, `FR-016` | future `prototype-measured` or release evidence required | Roadmap, Engineering Impact Matrix, no-go decisions |

## 1. Executive Gatekeeper Summary

**Primary recommendation**: Adopt a hybrid deterministic-DSP plus compact causal Temporal Convolutional Network (TCN) control-plane framework. Deterministic More-Phi mastering DSP remains the audio-rate Data Plane and the only source authorized to generate output samples. The TCN consumes feature windows and context metadata outside the audio callback, then emits bounded targets, parameter deltas, masks, multi-step plans, and confidence scores for deterministic validation and application. Evidence level: `research-estimate`. Traceability: `FR-001`, `FR-007`, `Decision 1`, `Decision 2`, Recommended Framework.

**Fallback baseline**: Use deterministic DSP, rule-based analysis, and offline/reference-assisted planning as the mandatory fallback and ablation baseline. This path is required when neural artifacts are missing, late, low-confidence, unvalidated, platform-incompatible, or prohibited by release gates. Evidence level: `planning`. Traceability: `FR-008`, `Decision 1`, `Decision 5`, Recommended Framework.

**Direct neural audio processing decision**: Production direct neural audio transformation inside `processBlock()` is rejected in this planning phase. A future research path may reconsider it only after fixed topology, fixed memory, no allocation, no locks, no I/O, no exceptions, deterministic worst-case completion below the smallest supported buffer deadline, and full signal-integrity gate passage are demonstrated with future measured evidence. Evidence level: `planning`. Traceability: `FR-005`, `FR-015`, `Decision 5`, Validation Gates G03-G09.

**Architectural boundary**: The neural component is a Control Plane that proposes intent. The deterministic DSP graph is the Data Plane that enforces physical signal constraints and produces audio. No neural output is executable as audio-rate behavior until it has passed the Safety Layer projection defined in Section 6. Evidence level: `planning`. Traceability: `FR-001`, `FR-005`, `FR-012`, `Decision 5`, `Decision 6`.

**Top risks requiring executive visibility**:

1. Sparse, legally restricted, or leakage-prone reference material can invalidate professional mastering claims and compromise model governance.
2. Neural control outputs can induce loudness inflation, inter-sample peak violations, phase incoherence, aliasing, pumping, transient smear, unsafe bypass states, or invalid parameter semantics.
3. Runtime integration can fail catastrophically through priority inversion, buffer underruns, hidden allocation, synchronization stalls, denormal slowdowns, or platform-specific CPU variance.

**Next decision milestone**: Execute a constrained proof-of-concept that trains an offline teacher and compact causal TCN student on leakage-resistant material, then run objective metrics, blind expert listening, worst-case CPU/latency benchmarks, bypass/fallback tests, and artifact gates before any runtime integration decision. Current status: not measured.

## 2. Mastering Objective Analysis

### 2.1 Spectral Balance

- **User value**: Spectral balance helps users reach translation-ready tonal distribution across playback systems without overcorrecting source material. Traceability: `FR-002`, Mastering Objective Profile.
- **Acceptable transformations**: Broad EQ tilt correction, low-frequency headroom management, presence-band moderation, harshness attenuation, air-band shaping, and reference-informed tonal targets. All changes must be bounded, reversible through deterministic controls, and smoothed to avoid zipper noise or coefficient discontinuities.
- **Failure modes**: Hollow midrange, excessive low-frequency resonance, brittle high-frequency lift, over-aggressive de-essing, genre-inappropriate spectral flattening, pre-ringing from aggressive linear-phase processing, and loudness-biased tonal decisions.
- **Objective metrics**: Long-term spectral deviation from reference bands; Bark or mel-band error after loudness matching; spectral tilt delta; low-frequency crest/headroom checks; true-peak change after EQ-related gain movement; inter-sample peak risk after downstream limiting.
- **Listening-test prompts**: Does the master translate across small speakers, headphones, and full-range monitors? Is the tonal movement musically intentional rather than sterile correction? Did the model improve balance, or merely increase level and brightness?
- **Runtime suitability**: Deterministic real-time EQ is suitable for applying bounded controls. Neural analysis must run offline, in preview, in background, on the message thread, or through queued low-rate control. Direct audio-rate neural EQ is constrained until future measured proof satisfies runtime and artifact gates.

### 2.2 Dynamic Range Control

- **User value**: Dynamic control improves perceived density, macro-consistency, and loudness competitiveness while preserving punch, groove, transient identity, and mix intent. Traceability: `FR-002`, Mastering Objective Profile.
- **Acceptable transformations**: Broadband or multiband compression targets, limiter goals, crest-factor shaping, transient-preserving level control, gain-staging recommendations, and source-aware loudness planning.
- **Failure modes**: Pumping, breathing, transient smear, dull attacks, low-frequency modulation, loudness inflation, true-peak overs, clip-like distortion, unstable gain under host automation, and block-size-dependent envelope behavior.
- **Objective metrics**: Integrated LUFS, short-term LUFS, momentary loudness deltas, loudness range, crest factor, transient-to-sustain ratio, gain-reduction envelope smoothness, true-peak ceiling compliance, and over-count analysis.
- **Listening-test prompts**: Did drums retain impact? Does bass content trigger audible pumping? Does the track sound better at matched loudness, or only louder? Are vocal and lead elements stable through dense sections?
- **Runtime suitability**: Deterministic dynamics processors are suitable for audio-rate application. Neural outputs should be low-rate control plans with temporal coefficient smoothing, max-delta limits, and stale-plan rejection. Direct neural dynamics processing is rejected for production without measured real-time and artifact proof.

### 2.3 Stereo Imaging

- **User value**: Stereo imaging improves perceived width, depth, and spatial separation while preserving mono compatibility, center-image integrity, and low-frequency stability. Traceability: `FR-002`, Mastering Objective Profile.
- **Acceptable transformations**: Mid/side EQ and dynamics targets, bounded stereo-width adjustment, bass mono management, side-channel cleanup, and phase-aware enhancement.
- **Failure modes**: Mono incompatibility, phase cancellation, phantom-center instability, image collapse, low-frequency widening, inter-channel delay artifacts, excessive side energy, and correlation discontinuities during bypass or automation.
- **Objective metrics**: Stereo correlation distribution; mid/side energy ratio; mono fold-down spectral and loudness delta; inter-channel phase coherence; low-frequency side-energy threshold; center-channel stability proxies.
- **Listening-test prompts**: Do vocal, kick, snare, bass, and other anchor elements remain centered? Does mono playback remove critical content? Does the width feel integrated rather than exaggerated? Are phase artifacts audible on headphones?
- **Runtime suitability**: Deterministic stereo DSP with bounded controls is suitable. Neural analysis may recommend width plans, masks, or abstention states outside the audio callback. Direct neural stereo transformation is constrained because phase incoherence and fold-down failure are high-impact release blockers.

### 2.4 Harmonic Enhancement

- **User value**: Harmonic enhancement can add warmth, clarity, density, and excitation when applied conservatively, source-dependently, and with explicit alias/IMD containment. Traceability: `FR-002`, Mastering Objective Profile.
- **Acceptable transformations**: Bounded saturation drive, exciter amount, band-limited harmonic emphasis, wet/dry controls, and oversampled deterministic non-linear processors.
- **Failure modes**: Aliasing, intermodulation distortion, high-frequency harshness accumulation, transient blurring, masked detail, stereo-dependent non-linearity, true-peak elevation, and genre-inappropriate coloration.
- **Objective metrics**: THD and IMD estimates; alias-energy checks above expected harmonic bands; high-frequency residual energy; transient sharpness; true-peak increase; wet/dry delta; oversampling requirement compliance.
- **Listening-test prompts**: Does enhancement improve perceived energy without grit or fizz? Are cymbals and sibilants harsher? Does dense bass material create intermodulation? Does bypass reveal level manipulation rather than quality improvement?
- **Runtime suitability**: Deterministic oversampled saturation/exciter DSP can be audio-rate only when already proven safe. Neural outputs may recommend bounded drive, band, and wet/dry controls. Direct neural harmonic generation is rejected for production until aliasing, IMD, transition, and CPU gates pass.

## 3. Audio Modality and Temporal Dependency Analysis

**Primary modality**: Multi-channel PCM audio is the primary modality. The audit covers mono, stereo, and wider channel layouts, with stereo as the first validation target because mastering workflows and More-Phi's current DSP feature set emphasize stereo material. Traceability: `FR-003`, Audio Modality Profile.

**Channel layouts considered**:

- Mono: requires no stereo widening assumptions and must avoid artificial-width defaults.
- Stereo: primary target for spectral balance, dynamics, imaging, and harmonic enhancement.
- Wider layouts: research and roadmap consideration only until channel semantics, fold-down expectations, and bus-specific validation are defined.

**Sample-rate assumptions**:

- Planning validation should cover 44.1 kHz, 48 kHz, 88.2 kHz, and 96 kHz.
- Harmonic enhancement and limiter/true-peak checks must account for oversampling, reconstruction filters, and inter-sample peak behavior.
- Any future model artifact must declare supported sample rates or a deterministic resampling policy with measured quality and latency impact.

**Block-size and offline chunk assumptions**:

- Real-time block processing must treat 32, 64, 128, 256, 512, and 1024 sample buffers as compatibility targets, with the smallest supported buffer defining the hard worst-case deadline.
- Offline and training chunks can use 1-8 second windows for student control planning and longer section or whole-track summaries for offline analysis.
- Real-time block processing and offline analysis windows are separate operating regimes. This report does not approve neural inference inside the audio callback.

**Input categories**:

- Audio features: loudness, crest factor, spectral tilt, Bark/mel-band summaries, dynamics envelopes, transient density, stereo correlation, phase-width summaries, and harmonic/noise indicators.
- Context metadata: genre, user intent, target loudness, source quality, plugin-chain or render provenance, and reference descriptors.
- Optional references: mix/master pairs, reference tracks, stems, rendered examples, and controlled deterministic DSP render pairs.

**Output categories**:

- Bounded parameter targets and deltas.
- Masks indicating which controls may be changed.
- Multi-step mastering plans.
- Confidence scores and unknown/abstain flags.
- Embeddings or analysis reports for offline review.
- Direct audio buffers are excluded from the recommended production path.

**Temporal contexts**:

- Transient: sub-100 ms attack behavior, click risk, limiter interaction, and transient smear.
- Short phrase: 0.5-8 s dynamics, tonal balance, density, and local gain-envelope decisions.
- Section: 8-60 s chorus/verse contrast, loudness contour, arrangement-aware decisions, and long-release behavior.
- Whole track: global loudness, tonal arc, intro/outro treatment, reference matching, and sequence-level consistency.

**Reference data requirements and leakage risks**:

- Required material includes curated mix/master pairs, deterministic processor render pairs, reference masters, and difficult edge cases such as clipped, silent, high-crest-factor, bass-heavy, and already-mastered sources.
- Leakage risks include related renders, stems from the same session, alternate masters, plugin-chain variants, artist/session overlap, and genre clustering across train/validation/test splits.
- Sparse or legally restricted reference data is a high-impact risk because professional mastering quality cannot be proven from synthetic or unlicensed examples alone. Traceability: `FR-015`, Risk Register R01-R02.

## 4. Candidate Architecture Matrix

| Candidate | Intended role | Fidelity potential | Latency and CPU profile | Parameter/model scale implications | Temporal awareness | Training complexity | Deployment risk | Recommendation status |
|-----------|---------------|-------------------|-------------------------|------------------------------------|--------------------|--------------------|-----------------|-----------------------|
| WaveNet-inspired causal convolution | Research baseline for causal direct audio or feature-to-control modeling | High for local texture if trained well, but full-band mastering transparency is unproven | Full-band direct audio is expensive at 44.1-96 kHz; feature-rate variants are cheaper but still require strict scheduling analysis | Raw audio models can require many residual channels and dilation stacks; feature-rate control versions can be smaller | Strong causal receptive fields, bounded by dilation schedule and receptive-field design | High for stable full-band audio; moderate for feature-rate control adaptation | Hidden CPU spikes, alias/artifact risk, explainability limits, and direct-audio safety risk | Constrained/research-only; useful as TCN design inspiration, not primary production path |
| Compact causal TCN | Primary learned Control Plane producing bounded plans and deltas | Feasible for mastering recommendations when paired with deterministic DSP and expert validation | Fixed compute at feature rate; suitable for offline, preview, background, message-thread, or queued low-rate control publication | Planning target must remain compact enough for CPU-friendly student deployment; channel width/depth must be bounded by benchmark gates | Causal receptive field can cover 1-8 s student context and accept whole-track summaries from offline analysis | Moderate; requires curated data, composite losses, teacher-student distillation, and leakage control | Model quality, data leakage, confidence calibration, and bounded-control correctness remain unproven until PoC | Recommended primary architecture |
| Transformer or attention-based model | Offline analysis, reference matching, global structure, metadata/user-intent conditioning | Strong for long-range structure and context if data is sufficient | Variable and relatively costly; unsuitable for small-buffer audio-callback use | Attention memory grows with sequence length unless constrained or summarized | Strong global context and section-level awareness | High; needs large data, careful regularization, and leakage control | Latency variability, memory use, explainability, and overfit risk | Constrained to offline/research or teacher role |
| U-Net, spectral masking, or encoder-decoder | Offline enhancement, restoration-like experiments, spectral-mask research | Potentially strong for spectral shaping but risky for transparent mastering | STFT framing, overlap-add, phase handling, and large tensors add latency/CPU load | Encoder-decoder width/depth can grow quickly for stereo high-resolution audio | Good local-to-mid temporal context depending on windowing and architecture | High; phase, target ambiguity, and perceptual quality are difficult | Phase reconstruction damage, pre-ringing, latency, and CPU risk | Constrained/research-only; not primary live path |
| Deterministic DSP, rule-based, or hybrid baseline | Safe fallback, ablation baseline, and production audio-rate processing | High for predictable bounded mastering controls; limited learned adaptation | Known CPU profile and real-time safety when implemented according to More-Phi constraints | No neural model scale; rules and processor parameters remain auditable | Depends on analyzer windows and existing DSP state | Low to moderate; mostly validation and tuning | Less adaptive or reference-aware; still must handle bypass, host automation, and artifacts | Mandatory fallback baseline and audio-rate source of truth |

Rejected or constrained options are not production-ready because the current feature provides no prototype-measured proof for direct neural audio transformation, no CPU/latency benchmark under smallest DAW buffers, and no blind listening evidence. Traceability: `FR-006`, `FR-007`, `FR-008`, `Decision 1`, `Decision 5`, Architecture Candidate.

## 5. Recommended Framework: Control Plane and Data Plane

**Primary architecture shape**: Five layers: feature extraction, compact causal TCN, optional context conditioning, constrained multi-head outputs, and deterministic safety/application layer. Traceability: `FR-007`, `Decision 2`, Recommended Framework.

**Control Plane responsibilities**:

- Extract and consume feature-rate observations, context metadata, target descriptors, and safe-state summaries outside the audio callback.
- Estimate bounded mastering intent as target vectors, parameter deltas, masks, plan steps, confidence scores, and abstain states.
- Publish only versioned, finite, schema-valid, time-stamped plan candidates for deterministic validation.
- Never produce output audio buffers for the recommended production path.

**Data Plane responsibilities**:

- Execute deterministic audio-rate DSP in the plugin processing path.
- Apply only validated, bounded, smoothed controls received through safe handoff mechanisms.
- Preserve hard real-time constraints: no unbounded allocation, no waiting on locks, no I/O, no exceptions, no remote dependency, and no audio-thread model inference.
- Maintain transparent bypass, declared latency behavior, inter-sample peak mitigation, parameter smoothing, and fallback continuity.

**Inputs**:

- Feature windows: loudness, crest factor, spectral tilt, Bark/mel bands, dynamics envelopes, transient density, stereo correlation, phase width, harmonic/noise indicators.
- Context: sample rate, channel layout, target loudness, genre/source descriptors, user intent, reference descriptors, and optional whole-track summaries.
- Safety state: current deterministic DSP parameters, high-risk-control masks, confidence thresholds, last safe plan, and runtime eligibility flags.

**Outputs**:

- Bounded EQ, dynamics, stereo width, saturation/exciter, limiter, and wet/dry targets.
- Parameter deltas with max-delta limits.
- Type-aware normalized control payloads preserving continuous, binary, discrete, and enumerated parameter semantics.
- Masks identifying controls that may be changed.
- Confidence scores and abstain/unknown states.
- Plan steps for offline, preview, background, message-thread, or queued low-rate control application.

**Runtime boundary classification**:

- Offline analysis: permitted for report generation, reference matching, dataset preparation, teacher evaluation, and full-track analysis.
- Preview render: permitted for controlled A/B evaluation if late model output cannot interrupt playback.
- Background analysis: permitted only when it never blocks the audio path and publishes safe plan candidates.
- Message-thread recommendation: permitted for user-visible suggestions and review.
- Queued low-rate control: constrained; permitted only through bounded, smoothed, deterministic handoff to existing safe control paths.
- Audio callback: prohibited for neural inference in this phase; direct audio processing requires future measured proof and gate passage.

**Fallback behavior**:

- Missing model: deterministic DSP/rule-based baseline or transparent bypass.
- Late output: reject stale plan and hold last safe controls.
- Invalid output: reject NaN, Inf, schema mismatch, out-of-range value, illegal mask, or unsupported semantic type and use fallback.
- Low confidence: present suggestion for review or abstain.
- CPU overload: stop consuming neural plans and continue deterministic processing without blocking the audio path.

**Integration assumptions**:

- Production source code is out of scope for this audit.
- This planning deliverable introduces no preset, plugin state, project persistence, or hosted-plugin state compatibility change.
- Thread boundaries remain explicit: neural work is limited to offline, preview, background, message-thread, or queued low-rate control paths, and audio-thread work remains deterministic DSP only.
- Future runtime work must keep neural inference out of `processBlock()` unless every relevant gate is measured-pass.
- Future model artifacts must include provenance, schema/version metadata, supported sample rates, parameter mappings, and deterministic fallback behavior.
- More-Phi's existing deterministic mastering processors remain responsible for audio-rate signal generation.

## 6. Safety Layer as a Boundary Condition

The Safety Layer is a deterministic projection from neural intent to admissible control state. It is not an advisory UI layer. It is the formal boundary condition that prevents the Control Plane from violating Data Plane signal constraints. Traceability: `FR-005`, `FR-012`, `Decision 5`, `Decision 6`, Signal Integrity Policy.

Let `z_t` be a neural plan candidate at publication time `t`, `p_{t-1}` be the last applied deterministic control vector, `p_safe` be the last known safe control vector, and `S` be the admissible control set defined by parameter ranges, semantic masks, max-delta limits, confidence thresholds, finite-value checks, version/schema validity, and signal-integrity constraints. The only publishable control vector is:

```text
p_t = Project_S(z_t, p_{t-1}, p_safe, runtime_state)
```

If the projection cannot produce a finite, bounded, semantically legal, smoothed, and signal-safe `p_t`, then the system must reject `z_t` and hold `p_safe`, use deterministic DSP fallback, or enter transparent bypass according to the fallback contract. No neural output may bypass this projection.

The admissible set `S` is constrained by:

- **Range constraints**: normalized targets must remain inside declared per-parameter bounds.
- **Mask constraints**: high-risk controls are immutable unless explicitly enabled by a valid plan mask.
- **Semantic constraints**: binary, discrete, enumerated, and continuous controls must preserve their parameter-class semantics.
- **Delta constraints**: `abs(p_t[i] - p_{t-1}[i]) <= max_delta[i]` before smoothing and handoff.
- **Temporal constraints**: coefficient changes must use deterministic smoothing or crossfade rules compatible with block-size changes and automation.
- **Confidence constraints**: low-confidence or unknown model states must abstain or degrade to review-only recommendations.
- **Signal constraints**: true-peak ceiling, mono compatibility, stereo correlation, low-frequency side energy, alias/IMD risk, pumping risk, bypass continuity, and latency declaration constraints must remain within gate-defined policy.
- **Runtime constraints**: model output may never force audio-thread waiting, allocation, I/O, remote inference, exception handling, or synchronization with lower-priority work.

The Safety Layer therefore converts the neural problem from unconstrained audio transformation into constrained multi-objective control publication. It protects the physical signal path even when the model is stale, invalid, overconfident, undertrained, or unavailable.

## 7. Loss Functions and Training Strategy

**Warning against loudness-only claims**: A model that improves loudness metrics while damaging phase coherence, transient morphology, stereo image, alias profile, or bypass behavior is a failure. LUFS improvements are meaningful only when evaluated with level-matched listening, true-peak analysis, and artifact gates. Traceability: `FR-010`, `Decision 3`, Training Roadmap.

**Composite optimization objective**:

Training should be treated as a multi-objective optimization problem rather than a single scalar loudness target. The planning-level composite objective can be expressed as:

```text
L_total = w_stft L_mr_stft
        + w_phase L_phase
        + w_time L_time
        + w_loud L_loudness
        + w_dyn L_dynamics
        + w_ms L_mid_side
        + w_perc L_perceptual
        + w_id L_identity
        + w_art L_artifact
        + w_reg L_regularization
```

The weights are future experiment variables, not measured results. A valid training run must demonstrate that improvement in one objective does not mask regression in another.

**Composite loss stack**:

- Multi-resolution spectral objective: MR-STFT magnitude loss across short, medium, and long windows to balance transient localization against tonal-envelope accuracy.
- Phase or complex-spectrum objective: complex STFT, phase derivative, or group-delay-aware term to reduce phase damage, transient smear, and incoherent stereo movement.
- Time-domain objective: waveform L1 or Huber loss for conservative paired examples, weighted to avoid over-penalizing musically valid mastering differences.
- Loudness and dynamics objective: integrated LUFS, short-term LUFS, loudness range, crest factor, dynamics-envelope consistency, and gain-reduction behavior.
- Stereo or mid/side objective: mid/side energy, correlation, mono fold-down, and low-frequency side-energy penalties.
- Perceptual or expert-reference objective: perceptual embedding or expert-reference score used only as one term, never as sole proof.
- Identity/transparency regularizer: penalizes unnecessary movement on already acceptable, already-mastered, or identity examples.
- Artifact and true-peak penalties: true-peak ceiling, clipping, alias energy, IMD, pumping, pre-ringing, high-frequency harshness, and unsafe transition penalties.

**Training strategy**:

1. Start with conservative curated mix/master pairs and deterministic DSP render pairs where the target transformation is known and bounded.
2. Expand to curriculum tasks for EQ, dynamics, imaging, and harmonic enhancement separately before combined mastering plans.
3. Add genre/source diversity, difficult edge cases, and source-quality variation.
4. Train a high-capacity offline teacher to explore quality ceilings.
5. Distill to a compact causal TCN student for CPU-friendly preview/control-plane use.
6. Validate with objective metrics and blind or expert listening review at each stage.

**Hyperparameter planning ranges**:

- Student audio context: 1-8 seconds for control planning.
- Offline/teacher context: longer section or whole-track summaries where latency is not real-time critical.
- Receptive field: enough to cover local phrase dynamics, validated against CPU and quality gates.
- Depth/channel width: bounded by student CPU benchmark; compare small, medium, and teacher-sized bands during PoC.
- Learning rate: 1e-5 to 3e-4 with warmup and cosine decay.
- Weight decay: 1e-5 to 1e-3.
- Gradient clipping: 0.5 to 5.0.
- Effective batch: 8-64 audio chunks, adjusted for sample rate and memory.
- Early stopping: validation composite score plus blind/expert listening review, not loudness improvement alone.

**Data augmentation strategy**:

- Safe augmentations: gain staging, EQ tilt, mild dynamics variation, bounded stereo width, resampling validation, and controlled noise/artifact examples.
- Constrained augmentations: pitch/time changes only within musically plausible limits and never as a substitute for real mastering references.
- Harmonic augmentations: include oversampled deterministic saturation examples and alias/IMD negatives.
- Metadata augmentation: genre, loudness target, source quality, and user intent labels when reliable.

**Leakage-resistant split strategy**:

- Split by source track, artist/session, plugin chain, render scenario, and genre where possible.
- Keep stems, alternate masters, related renders, and deterministic variants from crossing train/validation/test boundaries.
- Preserve dataset provenance, plugin/version context, parameter schemas, validation flags, and split metadata.

## 8. Signal Integrity and Runtime Safeguards

| Failure mode | Required safeguard | Safe fallback |
|--------------|-------------------|---------------|
| Clipping and true peak | Output ceiling, true-peak estimation, inter-sample peak mitigation, gain bounds, limiter target validation | Reject plan, hold last safe controls, or use deterministic limiter baseline |
| NaN/Inf or invalid outputs | Finite checks, range checks, parameter mask validation, schema/version checks, semantic parameter validation | Drop output and continue deterministic DSP or transparent bypass |
| Phase and mono compatibility | Stereo correlation bounds, mid/side limits, mono fold-down checks, low-frequency side-energy constraints | Disable width changes and use mono-safe deterministic plan |
| Aliasing and high-frequency artifacts | Oversampling requirements for non-linear DSP, alias-energy checks, HF residual gates | Disable harmonic enhancement or reduce to conservative deterministic settings |
| Pumping, transient smear, pre-ringing | Dynamics-envelope checks, transient metrics, release/attack bounds, linear-phase caution | Reduce dynamics changes or revert to baseline compressor/limiter settings |
| Bypass and wet/dry transitions | Equal-latency or compensated paths, smoothing, crossfade rules, wet/dry bounds | Transparent bypass or last safe deterministic controls |
| Latency declaration or compensation | Fixed-latency declaration for any future runtime path; no dynamic latency mutation in callback | Preview/offline-only mode until fixed latency is proven |
| CPU overload and late model outputs | Deadline checks, stale-plan rejection, background publish only, no audio-thread waiting | Ignore late output and continue current deterministic processing |
| Missing model fallback | Optional model availability checks and versioned artifact validation | Deterministic DSP/rule-based baseline or disabled neural feature |

No fallback may block the audio callback, allocate on the audio path, wait on locks, perform I/O, throw exceptions, require remote inference, or create priority inversion between background inference and real-time audio. Runtime safeguards must preserve audio continuity under buffer-size changes, automation, bypass transitions, and host scheduling pressure. Traceability: `FR-012`, `Decision 5`, `Decision 6`, Signal Integrity Policy.

## 9. Go/No-Go Gates

All gates are current `planning` or `research-estimate` acceptance targets for future proof-of-concept work. None are measured by this documentation feature. A no-go decision is mandatory when a gate fails in a way that can affect audio continuity, signal integrity, legal release readiness, or user trust.

| Gate | Area | Measurement method | Threshold | Current evidence level | Decision rule | Residual risk |
|------|------|--------------------|-----------|------------------------|---------------|---------------|
| G01 Identity transparency | Objective audio quality | Identity and already-mastered set, loudness-matched objective analysis | Median loudness drift under 0.25 LU and true-peak increase under 0.3 dB | Future `prototype-measured`; current `research-estimate` | No-go for runtime if exceeded | May still miss subtle tonal or spatial damage |
| G02 Composite quality | Objective audio quality | Composite validation score using spectral, phase, dynamics, stereo, identity, and artifact terms | Student reaches at least 95% of teacher composite validation score | Future `prototype-measured`; current `research-estimate` | No-go for student deployment below threshold | Teacher may be imperfect or overfit |
| G03 Blind listening | Subjective listening quality | Blind expert A/B/X or panel review with level matching | At least 80% of reviewed excerpts receive no blocker rating; no artifact class recurs in more than 10% of excerpts | Future blind listening evidence; current `planning` | No-go if reviewers identify unaddressed objective blockers | Panel size and genre coverage may be limited |
| G04 Latency | Runtime performance | Worst-case measured inference/control publication under smallest supported buffer and sample-rate targets | No neural work in audio callback; any future permitted runtime path completes with explicit headroom below buffer deadline | Future benchmark; current `planning` | No-go for audio-callback inference without measured pass | Platform variance remains |
| G05 CPU headroom | Runtime performance | Stress benchmark under DAW-like load and background contention | Background/control-plane inference stays below 5% of one performance core on target hosts and causes zero audio dropouts in stress runs | Future benchmark; current `research-estimate` | No-go if overload affects audio continuity | Host/plugin interactions may vary |
| G06 Stability and invalid output | Stability | Fuzz invalid outputs, missing artifacts, model version mismatch, NaN/Inf, out-of-range controls | 100% invalid cases rejected without audio interruption | Future automated test; current `planning` | No-go if invalid outputs propagate | Coverage must include real model formats |
| G07 Bypass and transition safety | Bypass safety | Automated and listening checks for active/bypass, wet/dry, plan changes, and latency compensation | Zero sample discontinuities above -90 dBFS at transition points; no unannounced latency change; no parameter jump beyond declared max-delta limits | Future automated/listening test; current `research-estimate` | No-go if transitions are audible or unsafe | Edge DAW automation patterns remain |
| G08 Artifact rate | Artifact control | Alias, IMD, pumping, transient smear, pre-ringing, mono fold-down, overs, and HF residual tests | Zero true-peak overs; no high-severity recurring artifact; each measured artifact class stays below release-defined tolerance before runtime approval | Future `prototype-measured`; current `research-estimate` | No-go if high-severity artifacts remain | Metrics may not capture all perception |
| G09 Fallback behavior | Fallback safety | Missing, late, invalid, overloaded, and low-confidence model scenarios | Hold last safe controls, fade to deterministic DSP, or transparent bypass without blocking | Future automated/integration test; current `planning` | No-go if fallback blocks, underruns, clicks, or interrupts audio | Requires host-specific validation |
| G10 Data governance | Validation governance | Dataset audit for provenance, license, split isolation, and reference quality | 100% training/eval material has acceptable provenance and no known split leakage | Future review evidence; current `planning` | No-go if provenance or leakage is unresolved | Licensing constraints may shrink usable data |

Uncompromising no-go conditions:

- Any neural path that can execute inside `processBlock()` without future measured proof of fixed topology, fixed memory, no allocation, no locks, no I/O, no exceptions, and deadline headroom is rejected.
- Any integration that can cause priority inversion, audio-thread waiting, buffer underrun, host-visible latency mutation, or remote-inference dependency is rejected.
- Any model output path that can bypass the Safety Layer, publish invalid controls, violate parameter semantics, or produce discontinuous transitions is rejected.
- Any artifact profile with recurring high-severity phase incoherence, true-peak overs, aliasing, pumping, transient smear, pre-ringing, mono fold-down failure, or bypass clicks is rejected.
- Any dataset with unresolved licensing, provenance, or split leakage is rejected for quality claims and release training.

## 10. Roadmap and High-Stakes Engineering Impact Matrix

### Training Roadmap

| Stage | Purpose | Pass/fail evidence |
|-------|---------|--------------------|
| Train-1 Conservative paired baseline | Learn bounded mastering deltas from curated mix/master and deterministic render pairs | Objective metrics improve without identity drift, true-peak violations, or obvious artifacts |
| Train-2 Objective-specific curriculum | Isolate spectral, dynamics, imaging, and harmonic tasks before combined control planning | Per-objective validation passes and no objective regresses another beyond threshold |
| Train-3 Offline teacher | Establish quality ceiling with larger offline model and whole-track context | Teacher passes composite objective and blind/expert listening targets for PoC material |
| Train-4 Student distillation | Distill to compact causal TCN for preview/control-plane feasibility | Student reaches at least 95% teacher composite score and passes CPU-size targets |

### Validation Roadmap

| Stage | Purpose | Pass/fail evidence |
|-------|---------|--------------------|
| Validate-1 Objective metrics | Check spectral, phase, dynamics, stereo, loudness, true-peak, and artifact metrics | Gates G01, G02, G06, G08 have measured pass/fail outputs |
| Validate-2 Blind/expert listening | Confirm musical quality and detect metric blind spots | Gate G03 has documented panel method, material list, and pass/fail results |
| Validate-3 Runtime/fallback benchmark | Verify CPU, latency, bypass, invalid output, and fallback behavior | Gates G04, G05, G07, G09 have measured pass/fail outputs |
| Validate-4 Governance review | Confirm data provenance, licensing, split isolation, and model-card completeness | Gate G10 passes with legal/release signoff or produces no-go decision |

### Prioritized Next Actions

1. Build a scoped proof-of-concept dataset plan with provenance and split-isolation rules before training.
2. Define deterministic DSP render baselines and identity sets for transparent mastering validation.
3. Prototype teacher/student experiments offline, with no plugin runtime dependency.
4. Run objective metrics and blind listening before considering runtime integration.
5. Benchmark compact student control-plane inference outside the audio callback under conservative CPU assumptions.
6. Draft a model card and fallback contract before productizing any neural feature.

### Engineering Impact Matrix

| Risk ID | Area | Catastrophic failure mode | Engineering impact | Likelihood | Detection evidence | Mitigation or no-go condition | Owner role | Decision deadline | Linked gates |
|---------|------|---------------------------|--------------------|------------|--------------------|-------------------------------|------------|-------------------|--------------|
| R01 | Data | Sparse, biased, or non-representative professional mix/master pairs | High: model may learn non-general mastering behavior and invalidate quality claims | High | Dataset coverage review, split audit, reference-quality review | Start with documented deterministic render pairs and curated licensed material; no-go for professional claims without representative references | ML owner | Before Train-1 | G01, G02, G03, G10 |
| R02 | Licensing | Reference masters, stems, or commercial material lack training rights | High: legal exposure, release blockage, and unusable datasets | Medium | Provenance ledger, license review, legal signoff | Require provenance and license review for every dataset item; no-go for unlicensed or ambiguous material | Legal/release owner | Before dataset ingestion | G10 |
| R03 | Runtime | Neural inference or model backend causes priority inversion, buffer underruns, hidden allocation, locks, or synchronization stalls | High: audible dropouts, clicks, host instability, and product failure | High | Worst-case DAW stress benchmark, callback instrumentation, stale-plan tests | Keep inference out of audio callback; no-go for callback inference without G04-G05 measured pass and real-time safety proof | DSP owner | Before runtime PoC | G04, G05, G09 |
| R04 | Signal quality | Outputs cause phase incoherence, aliasing, pumping, transient smear, pre-ringing, or true-peak overs | High: professional mastering trust failure and release blocker | Medium | Signal integrity tests, artifact metrics, mono fold-down checks, blind listening | Enforce Safety Layer and gates G01, G06-G08; no-go for recurring high-severity artifacts | DSP/QA owner | Before listening validation signoff | G01, G06, G07, G08 |
| R05 | User trust | AI recommendations appear authoritative despite low confidence, weak evidence, or abstention-worthy inputs | Medium: misleading workflow, support risk, and brand damage | Medium | UX review, confidence calibration review, abstention audits | Expose confidence, evidence level, and abstain states; require auditable bounded outputs | Product owner | Before UX design | G03, G09 |
| R06 | Platform | CPU behavior or optional model backends vary across Windows, macOS, Linux, and DAW hosts | Medium: inconsistent availability, degraded performance, or unsafe enablement | Medium | Cross-platform CPU-only benchmarks, host matrix, model-card platform review | Conservative CPU benchmarks, optional model availability, deterministic fallback, and platform matrix; no-go for unsupported hosts | Release owner | Before runtime candidate | G04, G05, G09 |
| R07 | Validation | Objective metrics reward loudness or spectral matching while missing perceptual defects | High: false confidence in poor masters | Medium | Level-matched blind/expert listening, identity-set validation, artifact review | Require level-matched blind/expert listening and identity regularization; no-go for loudness-only improvement | QA/ML owner | Before production readiness decision | G01, G02, G03, G08 |
| R08 | Integration | Model outputs map poorly to hosted-plugin parameter semantics or high-risk controls | High: unsafe parameter jumps, invalid states, automation discontinuity, or bypass artifacts | Medium | Parameter semantic fuzzing, mask validation, max-delta tests, transition tests | Use masks, ranges, normalized parameter semantics, max-delta limits, smoothing, and deterministic handoff only | DSP/host owner | Before control-plane integration | G06, G07, G09 |

## 11. Cross-Section Traceability

| Recommendation | Traceability |
|----------------|--------------|
| Use hybrid deterministic DSP plus compact causal TCN control-plane inference | `FR-001`, `FR-007`, `Decision 1`, `Decision 2`, Recommended Framework |
| Keep deterministic DSP as audio-rate Data Plane and fallback baseline | `FR-005`, `FR-008`, `Decision 1`, `Decision 5`, Signal Integrity Policy |
| Reject production direct neural audio-callback inference in this phase | `FR-005`, `FR-015`, `Decision 5`, Validation Gates G04-G09 |
| Evaluate spectral balance, dynamics, stereo imaging, and harmonic enhancement separately | `FR-002`, Mastering Objective Profile, Validation Gates G01-G03/G08 |
| Use composite multi-objective losses rather than loudness-only optimization | `FR-010`, `Decision 3`, Training Roadmap, Validation Gates G01-G03 |
| Use staged teacher-student training and leakage-resistant splits | `FR-011`, `Decision 4`, Training Roadmap, Risk Register R01-R02/R07 |
| Enforce the Safety Layer as a deterministic boundary condition before any control publication | `FR-005`, `FR-012`, `Decision 5`, `Decision 6`, Signal Integrity Policy |
| Require safeguards for true peak, invalid output, phase, aliasing, transitions, latency, CPU, and missing model | `FR-012`, `Decision 6`, Signal Integrity Policy, Validation Gates G06-G09 |
| Require falsifiable go/no-go gates before implementation approval | `FR-014`, `Decision 6`, Validation Gate, Roadmap |
| Treat sparse or restricted reference data as a high-impact risk | `FR-015`, Audio Modality Profile, Risk Register R01-R02 |

## 12. Contract Review Checklist

- [x] All required sections are present.
- [x] No required objective is omitted.
- [x] At least five candidates are compared.
- [x] One primary architecture and one fallback baseline are named.
- [x] Direct audio-callback inference is rejected for production unless future measured proof passes all relevant gates.
- [x] At least eight go/no-go gates are defined.
- [x] Risks and assumptions are explicit.
- [x] The report avoids presenting unmeasured model behavior as proven capability.
