# Tasks: Neural Mastering Implementation Roadmap

**Input**: Design documents from `specs/003-neural-mastering-roadmap/`

**Prerequisites**: `plan.md`, `spec.md`, `research.md`, `data-model.md`, `contracts/`, `quickstart.md`, `implementation-strategy.md`

**Tests**: Required for Core safety logic, audio-thread-adjacent behavior, dataset schemas, build registration, controller handoff, fallback behavior, and validation gates. Write failing tests before the corresponding implementation tasks.

**Organization**: Tasks are grouped by user story to enable independent implementation and testing. The MVP is User Story 1 only: fixed-size neural mastering types plus deterministic Safety Layer projection and tests. Do not add an inference backend in the MVP.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel because it touches different files and has no dependency on incomplete tasks.
- **[Story]**: Maps the task to the user story from `spec.md` for traceability.
- Every task includes an exact repository-relative file path.

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Prepare deterministic, model-free project structure and configuration files used by later implementation phases.

- [X] T001 Create default neural mastering config directory and safety policy fixture in `config/neural-mastering/safety-policy.default.json`
- [X] T002 [P] Create dataset governance default fixture in `config/neural-mastering/dataset-governance.default.json`
- [X] T003 [P] Create disabled proof-of-concept model metadata fixture in `config/neural-mastering/poc-model.default.json`
- [X] T004 Add neural mastering source placeholders to the existing mastering source group in `CMakeLists.txt`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Establish shared contracts, fixed-size runtime vocabulary, and build/test registration that MUST be complete before any user story implementation can be applied.

**CRITICAL**: No planner, model runner, UI/MCP, or audio-chain application work can begin until this phase is complete.

- [X] T005 Define fixed-size target-vector, mask, evidence, gate, fallback, feature-frame, candidate, and validated-plan types in `src/Core/NeuralMasteringTypes.h`
- [X] T006 Define Safety Layer policy API and validation result API in `src/Core/NeuralMasteringSafetyPolicy.h`
- [X] T007 Add deterministic Safety Layer implementation skeleton with default policy construction in `src/Core/NeuralMasteringSafetyPolicy.cpp`
- [X] T008 Register new Core source files in `CMakeLists.txt`
- [X] T009 Register neural mastering unit test translation units in `tests/CMakeLists.txt`

**Checkpoint**: Foundation ready when `NeuralMasteringTypes.h`, `NeuralMasteringSafetyPolicy.h`, and `NeuralMasteringSafetyPolicy.cpp` compile in `MorePhiTests` without adding any model dependency.

---

## Phase 3: User Story 1 - Approve a Safe Implementation Architecture (Priority: P1) MVP

**Goal**: Deliver deterministic Safety Layer projection so reviewers can verify that bounded plan candidates are accepted, projected, rejected, or routed to fallback without model inference or audio-callback work.

**Independent Test**: `ctest --test-dir build/windows-msvc-release --build-config Release -R "NeuralMasteringSafety" --output-on-failure` rejects malformed candidates and accepts only bounded valid plans without requiring any model artifact.

### Tests for User Story 1

- [X] T010 [P] [US1] Add malformed candidate tests for schema mismatch, NaN, Inf, out-of-range targets, and illegal masks in `tests/Unit/TestNeuralMasteringSafetyPolicy.cpp`
- [X] T011 [P] [US1] Add confidence, abstain, review-only, stale-plan, and unsupported-layout tests in `tests/Unit/TestNeuralMasteringSafetyPolicy.cpp`
- [X] T012 [P] [US1] Add max-delta, high-risk mask, last-safe-hold, deterministic-baseline, transparent-bypass, and reject fallback tests in `tests/Unit/TestNeuralMasteringSafetyPolicy.cpp`

### Implementation for User Story 1

- [X] T013 [US1] Implement finite-value, schema-version, runtime-mode, timestamp, confidence, abstain, and review-only checks in `src/Core/NeuralMasteringSafetyPolicy.cpp`
- [X] T014 [US1] Implement target-range, editable-mask, high-risk-mask, and max-delta projection in `src/Core/NeuralMasteringSafetyPolicy.cpp`
- [X] T015 [US1] Implement gate result population for G04, G06, G07, G08, and G09 in `src/Core/NeuralMasteringSafetyPolicy.cpp`
- [X] T016 [US1] Implement fallback selection and last-safe-plan preservation in `src/Core/NeuralMasteringSafetyPolicy.cpp`
- [X] T017 [US1] Add focused neural mastering safety test discovery name or tags in `tests/Unit/TestNeuralMasteringSafetyPolicy.cpp`
- [X] T018 [US1] Run focused safety validation and record the command outcome in `specs/003-neural-mastering-roadmap/quickstart.md`

**Checkpoint**: User Story 1 is complete when invalid-output rejection is automated, valid plans are projected deterministically, fallback modes are observable, and no model artifact or audio-callback inference path is introduced.

---

## Phase 4: User Story 2 - Scope Files and Dependencies Before Coding (Priority: P2)

**Goal**: Implement source, dataset, build, config, and script scaffolding so every targeted artifact has a concrete owner, validation task, and dependency boundary before optional model work.

**Independent Test**: A reviewer can build the project, run dataset schema tests, inspect config/script outputs, and confirm all new files are registered with roles matching the manifest in `implementation-strategy.md`.

### Tests for User Story 2

- [X] T019 [P] [US2] Add dataset provenance, license-status, split-isolation, reference-quality, and unsupported-material tests in `tests/Unit/TestNeuralMasteringDatasetSchema.cpp`
- [X] T020 [P] [US2] Add model-card metadata validation fixture tests in `tests/Unit/TestNeuralMasteringDatasetSchema.cpp`
- [X] T021 [P] [US2] Add no-model baseline runner interface tests in `tests/Unit/TestNeuralMasteringController.cpp`

### Implementation for User Story 2

- [X] T022 [P] [US2] Implement neural mastering model metadata structs in `src/AI/NeuralMasteringModelMetadata.h`
- [X] T023 [P] [US2] Implement dataset item, split, provenance, license, and reference-quality schema declarations in `src/AI/Dataset/NeuralMasteringDatasetSchema.h`
- [X] T024 [US2] Implement dataset schema validation helpers in `src/AI/Dataset/NeuralMasteringDatasetSchema.cpp`
- [X] T025 [P] [US2] Implement offline feature extractor interface aligned with `NeuralMasteringFeatureFrame` in `src/AI/Dataset/NeuralMasteringFeatureExtractor.h`
- [X] T026 [US2] Implement offline feature extractor stub that returns finite schema-valid frames or explicit unsupported status in `src/AI/Dataset/NeuralMasteringFeatureExtractor.cpp`
- [X] T027 [US2] Register AI and dataset neural mastering sources in `CMakeLists.txt`
- [X] T028 [P] [US2] Create dataset audit script with provenance/license/split checks in `scripts/neural-mastering/audit_dataset.py`
- [X] T029 [P] [US2] Create offline feature extraction script contract stub in `scripts/neural-mastering/extract_features.py`
- [X] T030 [P] [US2] Create model-card generation script contract stub in `scripts/neural-mastering/generate_model_card.py`

**Checkpoint**: User Story 2 is complete when manifest-backed source files, dataset schemas, configs, and offline script stubs exist, compile or lint where applicable, and do not require optional model artifacts for normal plugin operation.

---

## Phase 5: User Story 3 - Execute a Phased Roadmap With Gates (Priority: P3)

**Goal**: Add safe application, controller orchestration, validation gates, and release-readiness reporting while preserving the rule that optional learned planning remains review-only or fallback-only until evidence gates pass.

**Independent Test**: A reviewer can run controller/application tests and confirm validated plans are applied only through safe thread domains, invalid plans preserve last-safe state, and gate failures block runtime advancement.

### Tests for User Story 3

- [X] T031 [P] [US3] Add controller tests proving no planner or model work is required from `processBlock()` in `tests/Unit/TestNeuralMasteringController.cpp`
- [X] T032 [P] [US3] Add validated-plan application and invalid-plan preservation tests in `tests/Unit/TestAudioEngine.cpp`
- [X] T033 [P] [US3] Add bypass, transition, true-peak, mono-compatibility, and discontinuity regression cases in `tests/Integration/TestVST3AudioSignalAccuracy.cpp`
- [X] T034 [P] [US3] Add optional planner/controller overhead benchmark cases outside the audio callback in `tests/Performance/BenchmarkSuite.cpp`

### Implementation for User Story 3

- [X] T035 [P] [US3] Implement bounded plan smoothing API in `src/Core/NeuralMasteringPlanSmoother.h`
- [X] T036 [US3] Implement bounded plan smoothing and transition safety checks in `src/Core/NeuralMasteringPlanSmoother.cpp`
- [X] T037 [US3] Add validated-plan application API and last-safe-plan accessors in `src/Core/AutoMasteringEngine.h`
- [X] T038 [US3] Implement non-audio-thread validated-plan application in `src/Core/AutoMasteringEngine.cpp`
- [X] T039 [US3] Add bounded EQ, dynamics, stereo, harmonic, limiter, and loudness target application adapters in `src/Core/AutoMasteringEngine.cpp`
- [X] T040 [P] [US3] Define controller orchestration API for feature sampling, planner invocation, safety projection, and application handoff in `src/AI/NeuralMasteringController.h`
- [X] T041 [US3] Implement controller orchestration with null/deterministic planner path and no audio-callback inference path in `src/AI/NeuralMasteringController.cpp`
- [X] T042 [P] [US3] Define optional model runner interface with null runner and disabled-by-default posture in `src/AI/NeuralMasteringModelRunner.h`
- [X] T043 [US3] Implement null model runner and deterministic baseline runner without external inference dependencies in `src/AI/NeuralMasteringModelRunner.cpp`
- [X] T044 [US3] Register controller, model runner, and smoother sources in `CMakeLists.txt`
- [X] T045 [US3] Add review-only plan status, evidence level, gate failure, and fallback state accessors in `src/Plugin/PluginProcessor.h`
- [X] T046 [US3] Wire controller lifecycle outside `processBlock()` in `src/Plugin/PluginProcessor.cpp`

**Checkpoint**: User Story 3 is complete when validated application is deterministic, controller work stays outside the audio callback, gate failures are visible, and optional model-runner code remains disabled-by-default and fallback-safe.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Validate the implemented slices, update review surfaces, and keep claims aligned with available evidence.

- [X] T047 [P] Add plan-status and fallback explanation prompts in `docs/AI_ASSISTANT_TEST_PROMPTS.md`
- [X] T048 [P] Create objective metrics script contract stub in `scripts/neural-mastering/run_objective_metrics.py`
- [X] T049 [P] Create planner benchmark script contract stub in `scripts/neural-mastering/benchmark_planner.py`
- [X] T050 [P] Create model artifact validation script contract stub in `scripts/neural-mastering/validate_model_artifact.py`
- [X] T051 Add G01-G10 release-readiness checklist output to `scripts/neural-mastering/generate_model_card.py`
- [X] T052 Run Windows release/test configure path from `specs/003-neural-mastering-roadmap/quickstart.md`
- [X] T053 Run focused neural mastering safety tests from `specs/003-neural-mastering-roadmap/quickstart.md`
- [X] T054 Verify no direct neural inference, model loading, file I/O, locking, allocation, exceptions, or network work was added to `src/Plugin/PluginProcessor.cpp`
- [X] T055 Update completed task checkboxes and validation notes in `specs/003-neural-mastering-roadmap/tasks.md`
- [X] T056 [P] Add detailed offline model, dataset, and training plan in `specs/003-neural-mastering-roadmap/neural-training-implementation-plan.md`
- [X] T057 [P] Add pairwise dataset synthesis pipeline in `scripts/neural-mastering/generate_mastering_dataset.py`
- [X] T058 [P] Add modular PyTorch training framework in `scripts/neural-mastering/train_neural_mastering.py`

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies; can start immediately.
- **Foundational (Phase 2)**: Depends on Setup; blocks all user stories.
- **User Story 1 (Phase 3)**: Depends on Foundational; MVP scope and first implementation slice.
- **User Story 2 (Phase 4)**: Depends on Foundational and should start after US1 safety types are stable.
- **User Story 3 (Phase 5)**: Depends on US1 safety projection and US2 metadata/schema boundaries.
- **Polish (Phase 6)**: Depends on the desired implementation phases being complete.

### User Story Dependencies

- **User Story 1 (P1)**: Starts after Foundational; no dependency on other stories.
- **User Story 2 (P2)**: Starts after Foundational, but dataset/model metadata tasks should reuse US1 types where possible.
- **User Story 3 (P3)**: Starts after US1 safety policy passes and US2 metadata boundaries are defined.

### Within Each User Story

- Write tests first and confirm they fail before implementation.
- Define types before policy implementation.
- Implement safety projection before controller, model runner, UI, MCP, or application work.
- Register source files in CMake only after the files exist.
- Run focused tests at each checkpoint before advancing to the next phase.

### Parallel Opportunities

- T002 and T003 can run in parallel after T001 is understood.
- T010, T011, and T012 can be authored in parallel because they target distinct test scenarios in the same test file but must be merged sequentially.
- T019, T020, and T021 can run in parallel because they cover dataset metadata and controller interface tests.
- T022, T023, T025, T028, T029, and T030 can run in parallel because they touch different files.
- T031, T032, T033, and T034 can run in parallel because they cover different test files.
- T035, T040, T042, and T047-T050 can run in parallel because they define independent interfaces or scripts.

---

## Parallel Example: User Story 1

```text
Task: "Add malformed candidate tests for schema mismatch, NaN, Inf, out-of-range targets, and illegal masks in tests/Unit/TestNeuralMasteringSafetyPolicy.cpp"
Task: "Add confidence, abstain, review-only, stale-plan, and unsupported-layout tests in tests/Unit/TestNeuralMasteringSafetyPolicy.cpp"
Task: "Add max-delta, high-risk mask, last-safe-hold, deterministic-baseline, transparent-bypass, and reject fallback tests in tests/Unit/TestNeuralMasteringSafetyPolicy.cpp"
```

## Parallel Example: User Story 2

```text
Task: "Implement neural mastering model metadata structs in src/AI/NeuralMasteringModelMetadata.h"
Task: "Implement dataset item, split, provenance, license, and reference-quality schema declarations in src/AI/Dataset/NeuralMasteringDatasetSchema.h"
Task: "Implement offline feature extractor interface aligned with NeuralMasteringFeatureFrame in src/AI/Dataset/NeuralMasteringFeatureExtractor.h"
Task: "Create dataset audit script with provenance/license/split checks in scripts/neural-mastering/audit_dataset.py"
```

## Parallel Example: User Story 3

```text
Task: "Add controller tests proving no planner or model work is required from processBlock() in tests/Unit/TestNeuralMasteringController.cpp"
Task: "Add validated-plan application and invalid-plan preservation tests in tests/Unit/TestAudioEngine.cpp"
Task: "Add bypass, transition, true-peak, mono-compatibility, and discontinuity regression cases in tests/Integration/TestVST3AudioSignalAccuracy.cpp"
Task: "Define optional model runner interface with null runner and disabled-by-default posture in src/AI/NeuralMasteringModelRunner.h"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1 setup.
2. Complete Phase 2 foundational Core types, Safety Layer API, and test registration.
3. Complete Phase 3 User Story 1 safety projection tests and implementation.
4. Stop and validate with the focused `NeuralMasteringSafety` CTest command.
5. Do not add model inference, UI/MCP mutation, or audio-chain application until MVP safety projection passes.

### Incremental Delivery

1. Setup + Foundational creates the deterministic vocabulary and build/test registration.
2. US1 delivers the MVP safety policy and fallback behavior.
3. US2 adds manifest-backed dataset, config, metadata, and offline validation scaffolding.
4. US3 adds controller/application boundaries and gated runtime review infrastructure.
5. Polish validates claims, scripts, prompt surfaces, and release-readiness gates.

### Stop Conditions

- Stop if any task adds neural inference, model loading, file I/O, locks, allocation, exceptions, network work, or dynamic latency mutation to `processBlock()`.
- Stop if any candidate can bypass `NeuralMasteringSafetyPolicy` before application.
- Stop if low-confidence, stale, unsupported-layout, NaN/Inf, out-of-range, or illegal-mask output can affect applied processor state.
- Stop if model artifacts become required for normal plugin operation.
- Stop if documentation, UI, MCP, or scripts claim measured neural quality without corresponding G01-G10 evidence.

## Notes

- `[P]` tasks identify parallel opportunities but same-file edits must still be merged carefully.
- Every completed implementation task must be marked `[X]` in this file by `/speckit-implement`.
- Keep optional model runner work disabled by default and proof-of-concept only.
- Prefer deterministic fallback and review-only output whenever evidence is missing.
