# Data Model: Neural Architecture Audit

## Audit Scope

**Purpose**: Defines the decision boundary for the audit.

**Fields**:
- `feature_name`: Human-readable audit name.
- `project_objective`: Product goal being evaluated.
- `primary_learning_task`: Proposed task category, such as parameter recommendation, control-trajectory planning, or audio-conditioned preset selection.
- `excluded_tasks`: Task categories rejected or deferred.
- `runtime_boundary`: Where model use may occur: audio, message, MCP, background, offline, or dataset contexts.
- `decision_questions`: Questions the audit must answer before implementation planning.

**Relationships**:
- Owns many Data Modality Profiles.
- Owns many Model Candidates.
- Produces one Recommended Architecture and one Risk Register.

**Validation Rules**:
- Must distinguish control/parameter recommendation from raw audio generation.
- Must state whether each runtime path is permitted, constrained, or prohibited.

## Data Modality Profile

**Purpose**: Describes each input/output data category used to evaluate model suitability.

**Fields**:
- `name`: Modality name.
- `direction`: Input, output, label, metadata, or validation signal.
- `availability`: Present, partial, generated, external, or missing.
- `shape_category`: Fixed vector, variable sequence, image-like tensor, sparse mask, class label, text embedding, or metadata record.
- `expected_dimensionality`: Known or estimated size category.
- `quality_risks`: Missing values, noisy labels, domain shift, unstable identity, leakage, or imbalance.
- `relevance`: Why the modality matters to the learning task.

**Relationships**:
- Referenced by Model Candidates and the Training Plan.
- Constrains the Recommended Architecture.

**Validation Rules**:
- Must include audio-derived features, parameter snapshots, control trajectories, plugin metadata, text instructions, and validation labels where applicable.
- Must document variable plugin parameter counts and masks.

## Model Candidate

**Purpose**: Captures an evaluated model family.

**Fields**:
- `family`: Transformer, CNN, RNN/LSTM/GRU, GAN, Diffusion, MLP, retrieval, or other baseline.
- `target_task_fit`: How well it matches the More-Phi task.
- `data_fit`: Modalities it handles well or poorly.
- `accuracy_potential`: Expected quality relative to alternatives.
- `compute_profile`: Latency, memory, CPU/GPU needs, and offline/live suitability.
- `training_complexity`: Data, tuning, stability, and reproducibility burden.
- `interpretability`: Ability to explain recommendations and failures.
- `safety_profile`: Ability to enforce masks, bounds, high-risk controls, and confidence thresholds.
- `recommendation_status`: Selected, fallback, rejected, deferred, or offline-only.

**Relationships**:
- Consumes one or more Data Modality Profiles.
- One selected candidate becomes the Recommended Architecture.

**Validation Rules**:
- At least five model families must be evaluated.
- Each candidate must include task fit, efficiency, and real-time suitability.

## Recommended Architecture

**Purpose**: Defines the selected model approach and fallback baseline.

**Fields**:
- `primary_model`: Selected primary architecture.
- `fallback_model`: Baseline used when the primary model is unavailable or uncertain.
- `input_contract`: Required modalities and masks.
- `output_contract`: Parameter targets, deltas, trajectories, confidence, and safety flags.
- `runtime_classification`: Where inference may run.
- `safety_gates`: Bounds, deny-lists, confidence thresholds, smoothing, and rate limits.
- `deployment_notes`: Optional runtime, graceful fallback, and proof-of-concept prerequisites.

**Relationships**:
- Derived from Model Candidates and Data Modality Profiles.
- Governed by the Training Plan and Evaluation Benchmark.

**Validation Rules**:
- Must name one primary architecture and one fallback baseline.
- Must explicitly prohibit audio-callback inference unless future proof validates real-time safety.

## Training Plan

**Purpose**: Defines how a recommended model would be trained and tuned.

**Fields**:
- `dataset_sources`: Generated datasets, parameter snapshots, rendered audio, text intents, expert references, and labels.
- `preprocessing`: Feature extraction, normalization, masking, tokenization, segmentation, and augmentation.
- `hyperparameters`: Context window, model size, embeddings, dropout, learning rate, schedule, batch size, and early stopping.
- `loss_functions`: Continuous, discrete, masked, trajectory, constraint, perceptual, and preference losses.
- `split_strategy`: Leakage-resistant split by plugin, source audio, preset, genre, or scenario.
- `training_phases`: Pretraining, fine-tuning, calibration, distillation, and ablation.

**Relationships**:
- Trains the Recommended Architecture.
- Produces metrics consumed by Evaluation Benchmark.

**Validation Rules**:
- Must include deterministic seeds or reproducibility controls in future implementation.
- Must account for sparse expert labels and synthetic-data bias.

## Evaluation Benchmark

**Purpose**: Defines how success is measured.

**Fields**:
- `quality_metrics`: Expert agreement, parameter error, trajectory error, perceptual feature error, and usefulness ratings.
- `safety_metrics`: Constraint violation rate, unsafe-control changes, output bounds, and confidence calibration.
- `runtime_metrics`: p50/p95/p99 latency outside the audio callback, CPU use, memory use, and throughput.
- `generalization_metrics`: Held-out plugin, source material, preset, genre, and scenario performance.
- `acceptance_thresholds`: Numeric or review-score targets required for adoption.

**Relationships**:
- Validates the Training Plan and Recommended Architecture.
- Feeds Risk Register decisions.

**Validation Rules**:
- Must include at least three validation metrics and a leakage-resistant split strategy.
- Must include user-perceived usefulness and latency acceptability.

## Risk Register

**Purpose**: Tracks blockers and follow-up proof needs.

**Fields**:
- `risk_id`: Stable identifier.
- `description`: Risk statement.
- `category`: Data, model, safety, runtime, UX, integration, or evaluation.
- `impact`: Low, medium, high, or critical.
- `likelihood`: Low, medium, or high.
- `mitigation`: Recommended action.
- `owner_context`: Future role or workstream likely responsible.
- `status`: Open, mitigated, accepted, or deferred.

**Relationships**:
- Informed by all other entities.
- Drives future task planning and proof-of-concept work.

**Validation Rules**:
- Must include data availability, audio-thread safety, parameter safety, domain shift, and model overclaiming risks.
