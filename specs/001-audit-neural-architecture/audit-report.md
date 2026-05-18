# Neural Architecture Audit: More-Phi VST3/AU

## Executive Summary

**Primary recommendation**: More-Phi should use a compact hybrid multimodal temporal Transformer for future neural assistance, deployed only as an optional control-plane, background, MCP, offline, or dataset workflow. The model should consume audio-derived features, hosted-plugin parameter state, parameter masks and classifications, plugin metadata, optional text intent embeddings, and short control-history windows. It should output bounded normalized parameter targets, deltas, short automation trajectories, confidence values, and safety flags.

**Fallback baseline**: Use retrieval plus constrained regression. The fallback should retrieve similar examples from audio-feature, plugin-metadata, text-intent, and parameter-state indexes, then apply a bounded regressor that respects parameter masks, high-risk control limits, and confidence thresholds.

**Raw audio generation decision**: Raw audio generation is rejected as the primary path for this feature. It is not needed to make More-Phi successful at hosted-plugin morphing and control recommendation, it is harder to validate in a DAW, and it conflicts with the current real-time safety boundary unless a future proof demonstrates bounded, allocation-free, non-blocking execution outside the audio callback. Diffusion or other generative audio approaches may be revisited only for offline variation generation or dataset augmentation.

**Top risks**:

1. **Sparse expert labels** may make supervised control recommendation underdetermined.
2. **Audio-thread safety** would be violated if future implementers run inference or network calls in `processBlock()`.
3. **Hosted-plugin parameter instability** may cause unsafe or invalid recommendations when parameter identity, masks, or high-risk classifications are missing.

**Next actions**:

1. Build the audit report into follow-up proof-of-concept tasks for dataset extraction, offline evaluation, and baseline modeling.
2. Prototype the retrieval plus constrained regression baseline before training the Transformer.
3. Define a non-audio-thread benchmark harness for latency, memory, safety violations, and expert-reference agreement.
4. Add a runtime integration plan only after benchmark evidence proves usefulness, safety, and graceful fallback behavior.

## Project Objective and Learning Task

More-Phi is a JUCE 8 C++20 VST3/AU plugin that hosts third-party plugins and morphs between parameter snapshots using interpolation, physics behavior, audio-domain engines, genetic breeding, dataset generation, and MCP/AI control. The product objective for neural assistance is not to synthesize replacement audio; it is to help choose or plan safe hosted-plugin control states that produce useful morphing, mastering, or sound-design outcomes.

The primary learning task is **bounded multimodal parameter/control recommendation**:

- Predict normalized hosted-plugin parameter targets or deltas.
- Predict short, low-rate control trajectories when temporal movement matters.
- Condition recommendations on audio descriptors, optional spectral summaries, plugin metadata, parameter masks, existing snapshots, text instructions, and validation labels.
- Emit confidence and safety metadata so uncertain or unsafe outputs fall back to deterministic behavior.

### Deterministic, Heuristic, and Learned Behavior

| Behavior class | More-Phi examples | Audit decision |
|----------------|-------------------|----------------|
| Deterministic | APVTS parameter state, lock-free command handoff, parameter normalization, bounds checks, snapshot storage, plugin-host lifecycle | Must remain authoritative for state mutation and safety. |
| Heuristic | Physics interpolation, semantic safety ranges, high-risk control rules, retrieval fallback, confidence thresholds | Should constrain and explain model outputs. |
| Learned | Future Transformer policy, constrained regressor, intent/audio embeddings, calibrated confidence estimates | Optional assistant layer only; never the owner of audio-thread state. |

The model's role is advisory and bounded. It should produce compact control data for existing safe handoff mechanisms rather than directly mutating hosted-plugin state.

## Data Modality and Dimensionality Audit

More-Phi's neural task is multimodal because the desired control recommendation depends on current audio, hosted-plugin state, user intent, parameter semantics, and expected perceptual result.

### Modality Profiles

| Modality | Direction | Availability | Shape category | Expected dimensionality | Quality risks | Relevance |
|----------|-----------|--------------|----------------|-------------------------|---------------|-----------|
| Scalar audio-derived features | Input / validation | Present or generated through dataset workflows | Fixed vector | Small fixed feature vector; research evidence references about 31 floats | May omit timbral detail; source material domain shift | Efficient conditioning for loudness, spectral, dynamics, and perceptual target matching. |
| Mel-spectrogram or spectral summaries | Input / validation | Optional/generated | Image-like or temporal tensor | Medium tensor; prototype evidence references around 128 bands by hundreds of frames | Heavier compute; temporal alignment risk | Adds richer timbre and transient context when scalar features are insufficient. |
| Parameter snapshots | Input / output | Present | Fixed vector plus sparse mask | Up to 12 snapshot slots, each with variable hosted-plugin parameter vectors | Missing slots; stale plugin identity | Core representation for morphing and target recommendation. |
| Normalized parameter vectors | Input / output | Present for hosted plugins | Fixed vector with variable active count | Up to 2048 normalized values in `[0, 1]` plus masks | Unstable ordering; invalid intermediate states for discrete controls | Primary output space for parameter targets and deltas. |
| Parameter masks and classifications | Input / safety metadata | Partial/present | Sparse mask and metadata record | Up to 2048 mask/class entries | Missing metadata; incorrect high-risk classification | Required to avoid unavailable controls and unsafe changes. |
| Time-varying control trajectories | Input / output | Partial/generated | Variable sequence | Short low-rate windows, recommended at 10-50 Hz over 1-30 seconds | Jitter; mismatch with audio-rate expectations | Needed for smooth automation and motion planning beyond static presets. |
| Plugin metadata | Input / metadata | Present/partial | Metadata record | Plugin ID, version, parameter names, categories, host context | Vendor changes; unstable parameter IDs | Allows generalization and held-out plugin evaluation. |
| Text instructions or intent embeddings | Input | Partial through LLM/MCP/UI workflows | Text embedding or class label | Small/medium embedding or intent category | Ambiguity; prompt drift; overclaiming | Helps map user goals to parameter-control recommendations. |
| Expert references and validation labels | Label / validation | Sparse or missing initially | Class labels, preference pairs, target vectors | Depends on collection process | Label noise, bias, leakage | Required for supervised quality targets and acceptance thresholds. |

### Dimensionality Categories

| Data item | Category | Audit guidance |
|-----------|----------|----------------|
| Audio scalar descriptors | Fixed vector | Always include as a low-cost baseline input. |
| Spectral or mel summaries | Image-like / temporal tensor | Use shallow encoders or pooling; avoid heavy audio generation models. |
| Current parameter state | Fixed vector with mask | Preserve normalized values and variable parameter availability. |
| Snapshot context | Small set of fixed vectors | Encode slot identity and snapshot role. |
| Automation history | Variable sequence | Limit context to short low-rate windows; do not operate at audio rate. |
| Parameter type/classification | Metadata record / embedding | Encode continuous, discrete, binary, enumeration, frequency, decibel, bypass, pitch, and volume semantics. |
| Text intent | Text embedding / class label | Use a compact embedding or intent classifier; treat it as control-plane data. |
| Model outputs | Fixed vector, sparse deltas, or variable sequence | Output bounded targets/deltas/trajectories, masks, confidence, and safety flags. |

### Data Quality and Edge Cases

- If expert labels are sparse, train the fallback baseline first and use active learning or preference review to prioritize examples.
- If hosted-plugin parameter identity is unstable, exclude that plugin from model training or require a plugin-specific mapping layer.
- If parameter masks are missing, default to no-change for unknown parameters.
- If high-risk controls are not classified, treat volume, pitch, bypass, and safety-limited controls as constrained or denied until reviewed.
- If source audio, genre, plugin type, or preset distribution shifts, require held-out scenario evaluation before adoption.

## Real-Time and Runtime Suitability Audit

More-Phi's audio callback cannot allocate, block, perform I/O, wait on locks, throw, run network operations, or run model inference. Neural outputs must be produced outside the audio callback and passed through bounded handoff mechanisms.

| Usage path | Classification | Rationale | Required constraints |
|------------|----------------|-----------|----------------------|
| Audio callback inference inside `processBlock()` | Prohibited | Inference can allocate, branch unpredictably, use large memory, block on runtime internals, or exceed worst-case DAW deadlines. | Only reconsider after future proof demonstrates bounded, allocation-free, non-blocking, host-safe execution. |
| Direct hosted-plugin mutation from a model | Prohibited | Bypasses `PluginHostManager`, `ParameterBridge`, masks, command queues, and semantic safety ranges. | Route through existing control-plane handoff and parameter safety mechanisms. |
| Network-hosted live inference | Prohibited for runtime control | Network latency and availability cannot satisfy plugin determinism. | Offline analysis only; never required for playback continuity. |
| Message-thread recommendation preview | Constrained | Safe for UI-visible suggestions if latency is bounded and fallback exists. | Do not block UI for long tasks; use cached/background results where possible. |
| MCP/AI tool recommendation | Constrained | Fits auditable JSON-RPC control-plane behavior. | Enforce auth, rate limits, bounded outputs, confidence thresholds, and queued parameter application. |
| Background worker inference | Permitted with constraints | Suitable for low-rate recommendation and preview generation. | No audio-thread blocking; publish compact results through safe handoff. |
| Offline dataset generation and evaluation | Permitted | Best place for larger experiments and benchmark analysis. | Preserve provenance, split metadata, parameter schemas, and reproducibility controls. |
| Model training | Permitted offline only | Training is compute-heavy and non-real-time. | Use leakage-resistant splits and deterministic seeds. |

Expected live-assistance latency should be framed as user-facing recommendation latency, not sample-level DSP latency. For interactive UI or MCP suggestions, target p50 under 50 ms, p95 under 150 ms, and p99 under 300 ms outside the audio callback. If these targets are missed, use cached recommendations, deterministic heuristics, or the fallback baseline.

## Model-Family Comparison Matrix

Scores use a 1-5 scale where 5 is strongest for More-Phi's primary control/parameter recommendation task.

| Model family | Task fit | Accuracy potential | Efficiency | Memory footprint | Training complexity | Interpretability | Safety fit | Plugin-host suitability | Recommendation |
|--------------|----------|--------------------|------------|------------------|---------------------|------------------|------------|-------------------------|----------------|
| Compact multimodal temporal Transformer | 5 | 5 | 3 | 3 | 3 | 3 | 4 | 4 outside audio callback | Selected primary |
| CNN / shallow audio encoder | 3 | 3 | 4 | 4 | 3 | 3 | 3 | 3 as submodule | Use only as feature encoder |
| RNN/LSTM/GRU | 3 | 3 | 4 | 4 | 3 | 3 | 3 | 4 outside audio callback | Rejected as primary; possible streaming baseline |
| GAN | 1 | 2 | 1 | 2 | 1 | 1 | 1 | 1 | Rejected |
| Diffusion | 2 | 4 offline | 1 | 1 | 2 | 2 | 2 | 1 live / 3 offline | Deferred to offline variation generation |
| MLP / constrained regressor | 3 | 2 | 5 | 5 | 5 | 4 | 5 | 5 outside audio callback | Fallback component |
| Retrieval baseline | 3 | 3 | 4 | 3 | 4 | 5 | 5 | 5 outside audio callback | Fallback component |

### Candidate Findings

**Transformer-style attention** is the best full-task match because More-Phi combines heterogeneous tokens: audio summaries, parameter vectors, masks, type embeddings, plugin metadata, text intent, snapshot slots, and short temporal histories. Attention can model cross-parameter dependencies and variable context better than independent regressors.

**CNNs** are useful for local spectral patterns but insufficient as the full decision model because parameter semantics, masks, plugin metadata, and text intent are not naturally image-like. A shallow CNN or pooling encoder can feed the Transformer when mel or spectral summaries are used.

**RNN/LSTM/GRU models** are efficient for sequences and easier to reason about for streaming control, but they are less flexible for heterogeneous parameter dependencies and multimodal context. They are reasonable baselines for trajectory-only prediction, not the primary architecture.

**GANs** are a poor fit because training stability, controllability, interpretability, and safety constraints are weak for bounded hosted-plugin parameter edits. GAN outputs are difficult to constrain for high-risk controls.

**Diffusion models** can generate high-quality variations offline but are iterative, expensive, and too slow for live control recommendation. They may be useful later for offline exploration or dataset augmentation, not primary plugin assistance.

**MLP/constrained regression and retrieval** provide the safest baseline. They are easier to validate, easier to explain, and can hard-enforce masks and bounds. They should be built first.

## Recommended Architecture

### Primary Model

Use a **compact hybrid multimodal temporal Transformer**:

1. **Audio encoder**: Encode scalar audio features directly and optionally summarize mel/spectral tensors with a shallow CNN or pooling encoder.
2. **Parameter encoder**: Embed normalized parameter values, availability masks, parameter type/classification, snapshot slot identity, and high-risk control flags.
3. **Metadata and intent encoder**: Encode plugin metadata and optional text intent embeddings as control-plane context.
4. **Temporal encoder**: Represent low-rate parameter/control history over a bounded context window.
5. **Transformer policy/decoder**: Fuse modality tokens and produce bounded parameter targets, deltas, or short automation trajectories.
6. **Safety head**: Emit confidence, uncertainty, constraint flags, and fallback recommendations.

### Fallback Baseline

Use **retrieval plus constrained regression**:

- Retrieve nearest examples using audio-feature vectors, plugin metadata, text-intent embeddings, and parameter-state similarity.
- Use a constrained regressor for normalized parameter deltas or target values.
- Enforce parameter masks, deny-lists, high-risk ranges, confidence thresholds, smoothing, and rate limits.
- Fall back to no-change, deterministic morphing, or manual review when similarity is low or confidence is poor.

### Input Contract

Required inputs:

- Current normalized hosted-plugin parameter vector and availability mask.
- Parameter classifications and high-risk control metadata where available.
- At least one audio-derived feature representation.
- Plugin identity, version, and parameter metadata.
- Snapshot context when recommendation depends on morph targets.

Optional inputs:

- Text instruction or intent embedding.
- Short low-rate control trajectory history.
- Expert target, preference, or validation reference during training/evaluation.
- Mel/spectral tensor summaries when scalar descriptors are insufficient.

### Output Contract

Allowed outputs:

- Normalized parameter targets in `[0, 1]`.
- Bounded parameter deltas.
- Short low-rate automation trajectory points.
- Per-parameter masks indicating changed, unchanged, unavailable, or denied controls.
- Confidence and uncertainty values.
- Safety flags for high-risk controls, out-of-distribution inputs, and fallback activation.

Prohibited outputs:

- Direct hosted-plugin mutation.
- Audio-rate control streams.
- Raw audio for the primary product path.
- Network-dependent live-control decisions.
- Unbounded or unmasked parameter writes.

### Safety Gates

- Clamp all continuous outputs to valid normalized ranges.
- Snap discrete, binary, and enumeration controls to valid values.
- Deny or require explicit confirmation for unsafe volume, pitch, bypass, and safety-limited controls.
- Apply confidence thresholds before publishing recommendations.
- Rate-limit and smooth accepted deltas.
- Default to no-change or fallback retrieval when masks, identity, or confidence are insufficient.
- Route accepted recommendations through control-plane handoff rather than direct plugin writes.

## Hyperparameters, Losses, and Training Strategy

### Initial Hyperparameter Ranges

| Hyperparameter | Recommended starting range | Rationale |
|----------------|----------------------------|-----------|
| Control feature rate | 10-50 Hz | Low-rate enough for control-plane work, high enough for perceptible automation. |
| Context window | 1-30 seconds | Captures short musical/control intent without long memory pressure. |
| Model width | 128-512 | Compact enough for CPU-safe optional inference outside the audio callback. |
| Transformer layers | 2-8 | Start small; scale only if benchmarks justify it. |
| Attention heads | 4-8 | Enough for modality/parameter interactions without excessive memory. |
| Parameter/type embeddings | 32-128 dimensions | Encodes metadata and type semantics compactly. |
| Dropout | 0.05-0.20 | Regularizes sparse labels and synthetic bias. |
| Learning rate | 1e-4 to 3e-4 | Stable range for AdamW-style optimization. |
| Weight decay | 1e-5 to 1e-3 | Controls overfitting to plugin/source artifacts. |
| Batch size | 32-256 | Tune by sequence length, feature size, and hardware. |
| Early stopping | 5-10 validation checks without improvement | Avoids overfitting sparse expert references. |

### Loss Functions

- **Continuous normalized values**: Huber loss or MSE on parameter targets and deltas.
- **Discrete, binary, and enumeration controls**: Cross-entropy or focal loss after valid-value snapping.
- **Masked/unavailable controls**: Masked loss so unavailable parameters do not contribute gradients.
- **Trajectories**: Sequence Huber/MSE plus derivative and smoothness penalties.
- **Safety constraints**: Penalties for out-of-range deltas, high-risk control changes, denied-control proposals, and confidence misuse.
- **Perceptual outcomes**: Loss on audio-feature target error where rendered-output references exist.
- **Preference objectives**: Pairwise ranking or preference loss for human/expert comparisons.
- **Calibration**: Confidence calibration loss or post-training calibration against validation errors.

A single MSE loss is insufficient because More-Phi outputs mix continuous, discrete, masked, temporal, safety-sensitive, and perceptual objectives.

### Dataset Collection and Preprocessing

Recommended sources:

- Existing generated datasets and metadata from More-Phi dataset workflows.
- Hosted-plugin parameter snapshots and normalized vectors.
- Rendered audio and scalar audio-feature extraction.
- Optional mel or spectral summaries for richer timbral targets.
- Text intents from UI/MCP/assistant scenarios.
- Expert references, pairwise preferences, and acceptance labels.

Preprocessing requirements:

- Normalize all hosted-plugin parameters as `[0, 1]` values.
- Preserve masks for unavailable or denied controls.
- Encode parameter type, high-risk classification, plugin identity, plugin version, and parameter identity.
- Segment trajectories into bounded low-rate windows.
- Tokenize or embed text intents outside the audio callback.
- Store provenance for source audio, plugin, preset, scenario, render configuration, and split assignment.

### Splits and Leakage Resistance

Use leakage-resistant splits that separate evaluation data by at least one of:

- Held-out plugin.
- Held-out plugin version.
- Held-out source audio.
- Held-out preset family.
- Held-out genre or source-material category.
- Held-out user scenario.

Do not allow near-duplicate renders, variations from the same preset, or the same source-audio/plugin pair to leak across train and validation/test splits.

### Training Phases

1. **Baseline phase**: Build retrieval plus constrained regression and rule-only safety gates.
2. **Representation phase**: Train or select compact audio, parameter, metadata, and intent encoders.
3. **Transformer phase**: Train the compact multimodal temporal Transformer on masked parameter targets and trajectory labels.
4. **Calibration phase**: Calibrate confidence, fallback thresholds, and high-risk control behavior.
5. **Ablation phase**: Measure the value of audio features, text intent, plugin metadata, temporal history, and spectral summaries.
6. **Distillation phase**: If needed, distill the Transformer into a smaller CPU-safe model for optional live control-plane preview.
7. **Review phase**: Run expert and usefulness evaluation before any runtime integration plan.

### Reproducibility Controls

- Use deterministic seeds for dataset splits, model initialization, and training runs.
- Version dataset schemas, feature extraction settings, plugin versions, and label definitions.
- Record model configuration, training commit, hyperparameters, metrics, and calibration thresholds.
- Store rejected examples and safety violations for error analysis.

## Evaluation Benchmarks and Target Accuracy

The model should not be adopted because it is accurate on one metric. It must satisfy quality, safety, latency, and usefulness thresholds together.

### Recommended Acceptance Thresholds

| Category | Metric | Initial target |
|----------|--------|----------------|
| Expert agreement | Top-1 accepted recommendation or expert score | At least 70% agreement on held-out scenarios for MVP; target 80%+ before broad runtime use. |
| Parameter accuracy | MAE on normalized continuous parameters | At least 20% lower MAE than constrained regression on held-out scenarios. |
| Discrete accuracy | Valid discrete/binary/enumeration choice rate | At least 95% valid-choice rate and at least 80% agreement where labels exist. |
| Trajectory quality | Derivative/smoothness error | At least 25% lower jerk or derivative error than unsmoothed direct deltas. |
| Safety | Constraint violation rate | 0 critical violations; under 0.1% non-critical bounded violations after safety gates. |
| High-risk controls | Unsafe volume/pitch/bypass changes | 0 unapproved high-risk changes in validation. |
| Confidence | Calibration error | Expected calibration error under 0.10 on held-out scenarios. |
| Runtime outside audio callback | Inference latency | p50 under 50 ms, p95 under 150 ms, p99 under 300 ms for live-control-plane preview. |
| Memory outside audio callback | Model and working memory | Small enough for plugin-host comfort; target under 100 MB resident overhead for optional runtime preview. |
| Usefulness | Reviewer rating | Average 4 out of 5 or higher for actionable recommendations. |
| Generalization | Held-out plugin/source/scenario degradation | No more than 20% relative quality drop versus in-distribution validation. |

### Validation Workflow

1. Evaluate retrieval plus constrained regression before the Transformer.
2. Compare all candidate families against the same held-out plugin/source/scenario splits.
3. Report quality metrics, safety violations, latency percentiles, memory overhead, and usefulness ratings together.
4. Run ablations to confirm each modality improves results enough to justify its complexity.
5. Treat any audio-thread inference proposal as invalid until separate real-time proof exists.

## Risk Register and Next Actions

| Risk ID | Description | Category | Impact | Likelihood | Mitigation | Owner context | Status |
|---------|-------------|----------|--------|------------|------------|---------------|--------|
| R-001 | Expert labels may be sparse or biased. | Data | High | High | Start with retrieval baseline, collect preference labels, use active learning, and report confidence. | Dataset/training owner | Open |
| R-002 | Synthetic data may not match user or DAW scenarios. | Data | Medium | Medium | Use held-out source/plugin/scenario splits and expert listening/usefulness reviews. | Evaluation owner | Open |
| R-003 | Audio-thread inference could be accidentally introduced later. | Runtime | Critical | Medium | Keep inference prohibited in `processBlock()` and require proof-of-concept latency/safety evidence before runtime integration. | Plugin architecture owner | Open |
| R-004 | Parameter identity or masks may be unstable across hosted plugins. | Integration | High | Medium | Require stable plugin metadata, masks, and fallback no-change behavior for unknown parameters. | Host/plugin owner | Open |
| R-005 | High-risk controls could cause unsafe jumps. | Safety | Critical | Medium | Deny or constrain volume, pitch, bypass, and safety-limited controls unless explicit rules permit changes. | Parameter safety owner | Open |
| R-006 | Transformer may overfit plugin/source artifacts. | Model | High | Medium | Use leakage-resistant splits, ablations, and held-out plugin/source validation. | ML owner | Open |
| R-007 | Model confidence may be poorly calibrated. | Evaluation | Medium | Medium | Add calibration validation and fallback thresholds before user-facing recommendations. | ML/evaluation owner | Open |
| R-008 | Live recommendation latency may feel slow in the UI. | UX | Medium | Medium | Use cached/background inference, p50/p95/p99 benchmarks, and fallback baseline. | UI/control-plane owner | Open |
| R-009 | Users may overtrust AI recommendations. | UX | Medium | Medium | Clearly label learned behavior, expose confidence, and require review for high-risk changes. | Product owner | Open |
| R-010 | Future runtime model dependency may increase deployment complexity. | Integration | Medium | Medium | Keep model runtime optional and use deterministic fallback when unavailable. | Build/release owner | Deferred |
| R-011 | Stakeholder actionability rating is not available until reviewers score the final report. | Evaluation | Medium | High | Schedule stakeholder review and require an average actionability score of 4 out of 5 or higher before implementation planning is considered accepted. | Product/technical lead | Open |

### Prioritized Next Actions

1. Implement a dataset audit that enumerates available audio features, parameter vectors, masks, metadata, and labels.
2. Build retrieval plus constrained regression as the first benchmarkable baseline.
3. Create offline evaluation fixtures for held-out plugin/source/scenario splits.
4. Define a non-audio-thread model runtime experiment only after baseline quality is measured.
5. Run a small Transformer proof of concept against the same baseline metrics.
6. Review safety-gate behavior for high-risk controls before any user-facing automation.

## Requirement and Success-Criteria Traceability

| Requirement | Covered by report sections |
|-------------|----------------------------|
| FR-001 | Project Objective and Learning Task; Executive Summary |
| FR-002 | Data Modality and Dimensionality Audit |
| FR-003 | Data Modality and Dimensionality Audit; Dimensionality Categories |
| FR-004 | Real-Time and Runtime Suitability Audit |
| FR-005 | Model-Family Comparison Matrix |
| FR-006 | Model-Family Comparison Matrix; Candidate Findings |
| FR-007 | Recommended Architecture; Executive Summary |
| FR-008 | Hyperparameters, Losses, and Training Strategy |
| FR-009 | Loss Functions |
| FR-010 | Dataset Collection and Preprocessing; Splits and Leakage Resistance; Training Phases |
| FR-011 | Evaluation Benchmarks and Target Accuracy |
| FR-012 | Risk Register and Next Actions |
| FR-013 | Full report structure, Executive Summary, Next Actions |
| FR-014 | Evidence Method; modality, runtime, model, and training sections |

| Success criterion | Status |
|------------------|--------|
| SC-001 | Satisfied: primary architecture, fallback baseline, and rejected alternatives are named in the Executive Summary, Model-Family Comparison, and Recommended Architecture sections. |
| SC-002 | Satisfied: all named modalities are covered and at least six model families are evaluated. |
| SC-003 | Satisfied: more than four measurable thresholds are defined across expert agreement, safety, usefulness, latency, parameter accuracy, calibration, and generalization. |
| SC-004 | Satisfied: every proposed model usage path is classified as permitted, constrained, or prohibited. |
| SC-005 | Satisfied: the training plan includes multiple validation metrics and leakage-resistant split strategy. |
| SC-006 | Pending stakeholder review: reviewers must rate actionability after reading the final report. |

## Evidence Method

Major recommendations are grounded in these evidence categories:

- **Project objective and architecture documentation**: More-Phi is a hosted-plugin morphing and control system, not a raw audio synthesizer replacement.
- **Dataset and feature behavior**: Existing dataset concepts include audio-derived features, optional spectral summaries, rendered audio, metadata, and validation workflows.
- **Parameter-state constraints**: Hosted-plugin parameters use normalized values, masks, classifications, snapshot state, and high-risk semantic handling.
- **MCP/AI automation boundaries**: AI/MCP changes must remain auditable control-plane actions and route through safe parameter handoff.
- **Established ML model-family characteristics**: Transformers fit heterogeneous multimodal context; CNNs fit local spectral encoding; RNNs fit sequences; GANs and Diffusion models are harder to constrain for live control; retrieval/regression baselines are interpretable and safe.
- **Real-time plugin safety requirements**: Audio-thread inference, blocking work, I/O, allocation, and network calls are prohibited unless a future proof validates safety.

## Quickstart Validation Record

- Planning inputs confirmed: `spec.md`, `plan.md`, and `research.md` define a technical audit, not runtime model implementation.
- Required structure confirmed: the report includes executive summary, modality audit, real-time audit, model-family comparison, recommendation, training plan, evaluation benchmarks, risk register, and next actions.
- Model recommendation quality confirmed: one primary architecture and one fallback baseline are named; Transformer-style, CNN, RNN/LSTM/GRU, GAN, Diffusion, and simple baselines are evaluated.
- More-Phi safety alignment confirmed: audio-callback inference is prohibited, normalized parameter values and masks are preserved, high-risk controls are constrained, and future automation is routed through control-plane handoff.
- Success criteria confirmed except stakeholder rating: the report defines measurable thresholds and must be reviewed by stakeholders for SC-006.
- Residual blocker: stakeholder review score is not available inside this document-only implementation and remains an external acceptance step.
