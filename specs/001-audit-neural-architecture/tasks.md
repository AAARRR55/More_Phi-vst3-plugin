# Tasks: Neural Architecture Audit

**Input**: Design documents from `/specs/001-audit-neural-architecture/`

**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/audit-report-contract.md, quickstart.md

**Tests**: This feature is documentation-only. Automated code tests are not required because no runtime code, CMake target, MCP interface, audio-thread path, or plugin state schema changes are planned. Validation is performed by contract review, quickstart review, and acceptance-scenario review against the generated audit report.

**Organization**: Tasks are grouped by user story to enable independent completion and review of each audit increment.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel because the task is read-only or touches a different file/section with no dependency on incomplete tasks
- **[Story]**: Which user story this task belongs to (US1, US2, US3)
- Include exact file paths in descriptions

## Path Conventions

- Feature artifacts: `specs/001-audit-neural-architecture/`
- Final deliverable: `specs/001-audit-neural-architecture/audit-report.md`
- Existing project evidence: `CLAUDE.md`, `src/AI/`, `src/Core/`, `src/Host/`, `src/Plugin/`, `src/UI/`, `tests/`

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Confirm inputs and create the final audit-report structure.

- [X] T001 Review feature requirements in `specs/001-audit-neural-architecture/spec.md`
- [X] T002 [P] Review planning and research decisions in `specs/001-audit-neural-architecture/plan.md` and `specs/001-audit-neural-architecture/research.md`
- [X] T003 [P] Review report contract and data model in `specs/001-audit-neural-architecture/contracts/audit-report-contract.md` and `specs/001-audit-neural-architecture/data-model.md`
- [X] T004 Create required audit report headings in `specs/001-audit-neural-architecture/audit-report.md`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Establish evidence, scope, and safety baselines that all user-story sections depend on.

**CRITICAL**: No user story writing should begin until this phase is complete.

- [X] T005 Add audit scope, evidence method, and requirement traceability scaffold in `specs/001-audit-neural-architecture/audit-report.md`
- [X] T006 Add More-Phi source-evidence inventory covering `CLAUDE.md`, `src/AI/`, `src/Core/`, `src/Host/`, `src/Plugin/`, and `src/UI/` in `specs/001-audit-neural-architecture/audit-report.md`
- [X] T007 Add documentation-only non-goals and real-time safety baseline in `specs/001-audit-neural-architecture/audit-report.md`

**Checkpoint**: The report has a complete skeleton, evidence map, and shared safety assumptions.

---

## Phase 3: User Story 1 - Understand Project and Data Fit (Priority: P1) MVP

**Goal**: Explain More-Phi's neural-assistance objective, data modalities, dimensionality categories, and runtime constraints.

**Independent Test**: A reviewer can read only the objective, modality, dimensionality, and runtime sections in `specs/001-audit-neural-architecture/audit-report.md` and identify the primary learning task, all relevant data categories, and permitted/constrained/prohibited runtime paths.

### Implementation for User Story 1

- [X] T008 [US1] Write the Project Objective and Learning Task section in `specs/001-audit-neural-architecture/audit-report.md`
- [X] T009 [US1] Distinguish deterministic, heuristic, and learned behavior in `specs/001-audit-neural-architecture/audit-report.md`
- [X] T010 [US1] Document audio-derived feature and optional mel-spectrogram modality profiles in `specs/001-audit-neural-architecture/audit-report.md`
- [X] T011 [US1] Document parameter snapshots, normalized parameter vectors up to 2048 entries, masks, and parameter classifications in `specs/001-audit-neural-architecture/audit-report.md`
- [X] T012 [US1] Document control trajectories, plugin metadata, text instructions or intent embeddings, and validation-label modalities in `specs/001-audit-neural-architecture/audit-report.md`
- [X] T013 [US1] Add input and output dimensionality table for fixed vectors, variable temporal windows, sparse masks, discrete controls, text embeddings, and metadata records in `specs/001-audit-neural-architecture/audit-report.md`
- [X] T014 [US1] Classify audio-callback, message-thread, MCP, background, offline, and dataset usage paths as permitted, constrained, or prohibited in `specs/001-audit-neural-architecture/audit-report.md`
- [X] T015 [US1] Validate User Story 1 acceptance scenarios against `specs/001-audit-neural-architecture/spec.md` in `specs/001-audit-neural-architecture/audit-report.md`

**Checkpoint**: User Story 1 is independently reviewable and satisfies FR-001 through FR-004.

---

## Phase 4: User Story 2 - Select a Defensible Neural Architecture (Priority: P2)

**Goal**: Compare model families and recommend one primary architecture plus one fallback baseline.

**Independent Test**: A reviewer can inspect the model-family comparison and recommendation sections in `specs/001-audit-neural-architecture/audit-report.md` and confirm at least five candidate families are evaluated for task fit, accuracy potential, computational efficiency, memory footprint, training complexity, interpretability, safety, and plugin-host suitability.

### Implementation for User Story 2

- [X] T016 [US2] Create the model-family comparison matrix covering Transformer-style, CNN, RNN/LSTM/GRU, GAN, Diffusion, and simple baseline approaches in `specs/001-audit-neural-architecture/audit-report.md`
- [X] T017 [US2] Score each model family for task fit, data fit, accuracy potential, compute profile, training complexity, interpretability, safety, and plugin-host suitability in `specs/001-audit-neural-architecture/audit-report.md`
- [X] T018 [US2] Recommend the compact hybrid multimodal temporal Transformer as the primary architecture in `specs/001-audit-neural-architecture/audit-report.md`
- [X] T019 [US2] Recommend retrieval plus constrained regression as the fallback baseline in `specs/001-audit-neural-architecture/audit-report.md`
- [X] T020 [US2] Explain why raw audio generation, GAN, Diffusion, CNN-only, RNN-only, and MLP-only alternatives are rejected, deferred, or limited in `specs/001-audit-neural-architecture/audit-report.md`
- [X] T021 [US2] Define the recommended architecture input contract, output contract, masks, confidence values, and safety gates in `specs/001-audit-neural-architecture/audit-report.md`
- [X] T022 [US2] Validate User Story 2 acceptance scenarios against `specs/001-audit-neural-architecture/contracts/audit-report-contract.md` in `specs/001-audit-neural-architecture/audit-report.md`

**Checkpoint**: User Story 2 is independently reviewable and satisfies FR-005 through FR-007.

---

## Phase 5: User Story 3 - Define Training and Evaluation Requirements (Priority: P3)

**Goal**: Define hyperparameters, losses, training strategy, evaluation thresholds, and risk-driven next actions.

**Independent Test**: A reviewer can use the training, evaluation, and risk sections in `specs/001-audit-neural-architecture/audit-report.md` to create follow-up implementation or proof-of-concept work without adding modeling assumptions.

### Implementation for User Story 3

- [X] T023 [US3] Define recommended hyperparameter ranges for control rate, context window, model width, layer count, attention heads, embeddings, dropout, learning rate, weight decay, batch size, scheduling, and stopping criteria in `specs/001-audit-neural-architecture/audit-report.md`
- [X] T024 [US3] Define loss functions for continuous parameters, discrete controls, masked controls, trajectories, safety constraints, perceptual features, and preference objectives in `specs/001-audit-neural-architecture/audit-report.md`
- [X] T025 [US3] Define dataset collection, preprocessing, feature extraction, normalization, masking, tokenization, segmentation, and augmentation strategy in `specs/001-audit-neural-architecture/audit-report.md`
- [X] T026 [US3] Define leakage-resistant split strategy, calibration workflow, ablation plan, validation workflow, and reproducibility controls in `specs/001-audit-neural-architecture/audit-report.md`
- [X] T027 [US3] Define evaluation benchmarks and target thresholds for expert agreement, parameter error, safe-parameter behavior, latency acceptability, usefulness ratings, and generalization in `specs/001-audit-neural-architecture/audit-report.md`
- [X] T028 [US3] Create the risk register with data availability, audio-thread safety, parameter safety, domain shift, model overclaiming, runtime, UX, integration, and evaluation risks in `specs/001-audit-neural-architecture/audit-report.md`
- [X] T029 [US3] Validate User Story 3 acceptance scenarios against `specs/001-audit-neural-architecture/spec.md` and `specs/001-audit-neural-architecture/data-model.md` in `specs/001-audit-neural-architecture/audit-report.md`

**Checkpoint**: User Story 3 is independently reviewable and satisfies FR-008 through FR-012.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Complete executive framing, acceptance validation, and final report quality.

- [X] T030 Add executive summary with primary architecture, fallback baseline, raw-audio decision, top three risks, and next actions in `specs/001-audit-neural-architecture/audit-report.md`
- [X] T031 Cross-check every required report section from `specs/001-audit-neural-architecture/contracts/audit-report-contract.md` in `specs/001-audit-neural-architecture/audit-report.md`
- [X] T032 Run quickstart validation from `specs/001-audit-neural-architecture/quickstart.md` and document any residual blockers in `specs/001-audit-neural-architecture/audit-report.md`
- [X] T033 Verify every functional requirement and measurable outcome from `specs/001-audit-neural-architecture/spec.md` is addressed in `specs/001-audit-neural-architecture/audit-report.md`
- [X] T034 Remove unresolved clarification markers, placeholder text, and inconsistent terminology from `specs/001-audit-neural-architecture/audit-report.md`

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies - can start immediately
- **Foundational (Phase 2)**: Depends on Setup completion - blocks all user stories
- **User Story 1 (Phase 3)**: Depends on Foundational completion - MVP audit increment
- **User Story 2 (Phase 4)**: Depends on User Story 1 data-fit and runtime findings
- **User Story 3 (Phase 5)**: Depends on User Story 2 architecture recommendation
- **Polish (Phase 6)**: Depends on all selected user stories being complete

### User Story Dependencies

- **User Story 1 (P1)**: Can start after Foundational phase and has no dependency on other stories
- **User Story 2 (P2)**: Depends on US1 because model-family comparison must use the audited task, modalities, dimensionality, and runtime constraints
- **User Story 3 (P3)**: Depends on US2 because hyperparameters, losses, training phases, and benchmarks must align with the selected architecture

### Within Each User Story

- Evidence and scope before conclusions
- Modality and runtime findings before model comparison
- Model comparison before primary/fallback recommendation
- Recommendation before hyperparameters, losses, and evaluation thresholds
- Risk register after data, runtime, model, and evaluation assumptions are documented

### Parallel Opportunities

- T002 and T003 can run in parallel because they review different planning inputs
- US1 evidence-gathering subtasks T010, T011, and T012 can be researched in parallel before one editor merges findings into `specs/001-audit-neural-architecture/audit-report.md`
- US2 scoring work for candidate families in T016 and T017 can be split by model family before one editor merges the matrix into `specs/001-audit-neural-architecture/audit-report.md`
- US3 training tasks T023 through T026 can be researched in parallel after T021 is complete, then merged sequentially into `specs/001-audit-neural-architecture/audit-report.md`

---

## Parallel Example: User Story 1

```bash
# Research modality sections in parallel, then merge into audit-report.md:
Task: "Document audio-derived feature and optional mel-spectrogram modality profiles in specs/001-audit-neural-architecture/audit-report.md"
Task: "Document parameter snapshots, normalized parameter vectors up to 2048 entries, masks, and parameter classifications in specs/001-audit-neural-architecture/audit-report.md"
Task: "Document control trajectories, plugin metadata, text instructions or intent embeddings, and validation-label modalities in specs/001-audit-neural-architecture/audit-report.md"
```

## Parallel Example: User Story 2

```bash
# Split model-family evidence review in parallel, then merge scores into audit-report.md:
Task: "Compare Transformer-style, CNN, and RNN/LSTM/GRU candidates in specs/001-audit-neural-architecture/audit-report.md"
Task: "Compare GAN, Diffusion, and simple baseline candidates in specs/001-audit-neural-architecture/audit-report.md"
```

## Parallel Example: User Story 3

```bash
# Research training guidance in parallel after the architecture contract is defined:
Task: "Define recommended hyperparameter ranges in specs/001-audit-neural-architecture/audit-report.md"
Task: "Define loss functions in specs/001-audit-neural-architecture/audit-report.md"
Task: "Define leakage-resistant split strategy and validation workflow in specs/001-audit-neural-architecture/audit-report.md"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1: Setup
2. Complete Phase 2: Foundational
3. Complete Phase 3: User Story 1
4. Stop and validate that the audit explains the project objective, data modalities, dimensionality categories, and runtime classifications independently

### Incremental Delivery

1. Complete Setup and Foundational phases so the report has its skeleton, evidence map, and safety baseline
2. Add User Story 1 to establish objective, modality, dimensionality, and runtime findings
3. Add User Story 2 to compare model families and name the primary/fallback recommendation
4. Add User Story 3 to define training, evaluation, risks, and next actions
5. Complete Polish to satisfy the report contract and quickstart validation

### Parallel Team Strategy

With multiple contributors:

1. One contributor owns the final `specs/001-audit-neural-architecture/audit-report.md` merge order
2. Contributors can research different modality, model-family, or training sections in parallel
3. The owner merges findings in dependency order: US1 findings, then US2 recommendation, then US3 training/evaluation, then Polish validation

---

## Notes

- The output remains a technical audit; do not add plugin runtime code, dependencies, trained model artifacts, network services, or audio-thread inference paths while completing these tasks
- Every recommendation must cite at least one evidence category from `specs/001-audit-neural-architecture/contracts/audit-report-contract.md`
- Any criterion that cannot be satisfied must be recorded in the risk register in `specs/001-audit-neural-architecture/audit-report.md`
- Commit after completing the task document or after each completed audit-report increment if requested by the user
