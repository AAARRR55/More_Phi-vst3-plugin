# Tasks: Neural Mastering Dataset Curation

**Input**: Design documents from `specs/004-dataset-curation/`

**Prerequisites**: `plan.md`, `spec.md`, `research.md`, `data-model.md`, `contracts/`, `quickstart.md`, `dataset-curation-report.md`

**Tests**: No audio-thread, Core DSP, Host lifecycle, state persistence, MCP, build, or plugin runtime tests are required for this documentation/governance feature. Validation tasks focus on JSON schema parsing, contract coverage, release-unsafe claim review, source-link review, and quickstart reproducibility.

**Organization**: Tasks are grouped by user story to enable independent delivery. The MVP is User Story 1: a structured, source-linked dataset candidate catalog and recommended research data mix.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can run in parallel because it touches different files and has no dependency on incomplete tasks.
- **[Story]**: Maps the task to the user story from `spec.md` for traceability.
- Every task includes an exact repository-relative file path.

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Prepare the documentation and catalog locations used by all curation work.

- [X] T001 Create the structured curation catalog directory in `specs/004-dataset-curation/catalog/`
- [X] T002 [P] Create the example records directory in `specs/004-dataset-curation/examples/`
- [X] T003 [P] Create the curation validation log in `specs/004-dataset-curation/validation-notes.md`
- [X] T004 [P] Add a short feature README that links spec, report, plan, tasks, and quickstart in `specs/004-dataset-curation/README.md`

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Finalize shared schemas and reference vocabularies that every user story depends on.

**CRITICAL**: User story catalog work depends on these contracts and reference vocabularies.

- [X] T005 Finalize the dataset candidate contract fields and examples in `specs/004-dataset-curation/contracts/dataset-candidate.schema.json`
- [X] T006 [P] Finalize the license ledger contract fields and conditional restrictions in `specs/004-dataset-curation/contracts/license-ledger.schema.json`
- [X] T007 [P] Finalize the source identity contract fields and overlap resolution values in `specs/004-dataset-curation/contracts/source-identity.schema.json`
- [X] T008 [P] Finalize the release eligibility contract fields and blocking issue values in `specs/004-dataset-curation/contracts/release-eligibility.schema.json`
- [X] T009 Create the dataset role reference vocabulary in `specs/004-dataset-curation/catalog/dataset-roles.json`
- [X] T010 [P] Create the license pattern reference vocabulary in `specs/004-dataset-curation/catalog/license-patterns.json`
- [X] T011 Validate all JSON schema files and record command output in `specs/004-dataset-curation/validation-notes.md`

**Checkpoint**: Foundation is complete when contracts parse, reference vocabularies exist, and downstream curation records have stable enums and schema targets.

---

## Phase 3: User Story 1 - Select Viable Training Data (Priority: P1) MVP

**Goal**: Deliver a structured, source-linked shortlist of viable, conditional, validation-only, hold, and rejected datasets.

**Independent Test**: A reviewer can inspect the structured catalog and identify which datasets are recommended, conditionally usable, or rejected, with a clear role and source link for each.

### Implementation for User Story 1

- [X] T012 [US1] Convert the curated dataset matrix into structured candidate records in `specs/004-dataset-curation/catalog/dataset-candidates.json`
- [X] T013 [P] [US1] Create compatibility assessment records for each evaluated candidate in `specs/004-dataset-curation/catalog/compatibility-assessments.json`
- [X] T014 [P] [US1] Create the recommended staged dataset mix and download order in `specs/004-dataset-curation/catalog/recommended-dataset-mix.json`
- [X] T015 [US1] Add catalog cross-references from each detailed fit note to candidate IDs in `specs/004-dataset-curation/dataset-curation-report.md`
- [X] T016 [US1] Add the first-download research sandbox checklist to `specs/004-dataset-curation/quickstart.md`
- [X] T017 [US1] Validate dataset candidate records against the candidate schema and record results in `specs/004-dataset-curation/validation-notes.md`

**Checkpoint**: User Story 1 is complete when the candidate catalog and recommended mix can be reviewed independently from prose and still satisfy the P1 acceptance scenarios.

---

## Phase 4: User Story 2 - Govern Licensing and Provenance (Priority: P2)

**Goal**: Make licensing, attribution, provenance, and commercial-readiness blockers explicit and traceable.

**Independent Test**: A reviewer can trace every dataset recommendation to a license category, source reference, required follow-up action, and release eligibility status.

### Implementation for User Story 2

- [X] T018 [P] [US2] Create the license ledger template with one representative placeholder item in `specs/004-dataset-curation/catalog/license-ledger.template.json`
- [X] T019 [US2] Create dataset-level license posture records for all evaluated candidates in `specs/004-dataset-curation/catalog/license-postures.json`
- [X] T020 [US2] Create release eligibility decisions for recommended, conditional, validation-only, hold, and rejected groups in `specs/004-dataset-curation/catalog/release-eligibility.json`
- [X] T021 [P] [US2] Create the attribution package template for permissive and attribution-required sources in `specs/004-dataset-curation/catalog/attribution-package.template.md`
- [X] T022 [US2] Add a legal-review follow-up table for non-commercial, educational-only, share-alike, gated, mixed, and unknown-license sources in `specs/004-dataset-curation/dataset-curation-report.md`
- [X] T023 [US2] Validate license ledger and release eligibility records against their schemas and record results in `specs/004-dataset-curation/validation-notes.md`

**Checkpoint**: User Story 2 is complete when no dataset can be interpreted as commercial-release-ready without explicit eligibility evidence.

---

## Phase 5: User Story 3 - Reduce Overfitting and Domain Bias (Priority: P3)

**Goal**: Encode source identity, split isolation, overlap risks, and bias controls so future model validation is not inflated by leakage.

**Independent Test**: A reviewer can verify that every recommended dataset has an overfitting-risk note, split-isolation requirement, and mitigation path.

### Implementation for User Story 3

- [X] T024 [P] [US3] Create the source identity template with required grouping fields in `specs/004-dataset-curation/catalog/source-identity.template.json`
- [X] T025 [US3] Create the source overlap risk register for Cambridge-MT, MUSDB18-HQ, MedleyDB, SolidStateBusComp, and Jamendo-derived candidates in `specs/004-dataset-curation/catalog/source-overlap-risk-register.json`
- [X] T026 [US3] Create the split and leakage policy checklist in `specs/004-dataset-curation/catalog/split-leakage-policy.md`
- [X] T027 [P] [US3] Create overfitting risk records for each evaluated candidate in `specs/004-dataset-curation/catalog/overfitting-risks.json`
- [X] T028 [US3] Add a mitigation table for duplicate sources, artist overlap, synthetic recipes, mono-only material, lossy encoding, and genre skew in `specs/004-dataset-curation/dataset-curation-report.md`
- [X] T029 [US3] Validate source identity and overfitting risk examples and record results in `specs/004-dataset-curation/validation-notes.md`

**Checkpoint**: User Story 3 is complete when future train/validation/test manifests have a clear source-level grouping policy and no generated variant can be split blindly.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Validate the whole curation package and prepare it for `/speckit-implement` or handoff.

- [X] T030 [P] Add schema validation command examples for all contracts and catalog records in `specs/004-dataset-curation/quickstart.md`
- [X] T031 [P] Add final curation acceptance checklist notes in `specs/004-dataset-curation/checklists/requirements.md`
- [X] T032 Run the release-unsafe claim search from quickstart and record results in `specs/004-dataset-curation/validation-notes.md`
- [X] T033 Cross-check every dataset source URL in the curation report and record review notes in `specs/004-dataset-curation/validation-notes.md`
- [X] T034 Confirm no runtime, CMake, plugin state, MCP, or audio-thread files are required for this feature and record the finding in `specs/004-dataset-curation/validation-notes.md`
- [X] T035 Update the quickstart with the final reviewer handoff sequence in `specs/004-dataset-curation/quickstart.md`
- [X] T036 Perform final format validation for task checklist lines and record results in `specs/004-dataset-curation/validation-notes.md`

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies; can start immediately.
- **Foundational (Phase 2)**: Depends on Setup; blocks structured catalog work.
- **User Story 1 (Phase 3)**: Depends on Foundational; MVP scope.
- **User Story 2 (Phase 4)**: Depends on Foundational; can run after or alongside US1 once candidate IDs are stable.
- **User Story 3 (Phase 5)**: Depends on Foundational; benefits from US1 candidate IDs but remains independently reviewable.
- **Polish (Phase 6)**: Depends on the desired user stories being complete.

### User Story Dependencies

- **User Story 1 (P1)**: Starts after Foundational; no dependency on US2 or US3.
- **User Story 2 (P2)**: Starts after Foundational; uses candidate IDs from US1 if available, otherwise can use planned IDs from `dataset-curation-report.md`.
- **User Story 3 (P3)**: Starts after Foundational; uses candidate IDs and dataset source names but does not depend on license decisions.

### Within Each User Story

- Define or verify catalog structure before filling records.
- Fill structured records before validation.
- Update prose report only after structured records have stable IDs.
- Record all validation commands and reviewer findings in `validation-notes.md`.

### Parallel Opportunities

- T002, T003, and T004 can run in parallel after T001 is started.
- T006, T007, T008, and T010 can run in parallel because they touch different contract/reference files.
- T013 and T014 can run in parallel after candidate IDs are defined in T012.
- T018 and T021 can run in parallel with T019/T020 preparation because they touch separate template/documentation files.
- T024 and T027 can run in parallel because source identity and risk records are separate files.
- T030 and T031 can run in parallel during polish.

---

## Parallel Example: User Story 1

```text
Task: "Create compatibility assessment records for each evaluated candidate in specs/004-dataset-curation/catalog/compatibility-assessments.json"
Task: "Create the recommended staged dataset mix and download order in specs/004-dataset-curation/catalog/recommended-dataset-mix.json"
```

## Parallel Example: User Story 2

```text
Task: "Create the license ledger template with one representative placeholder item in specs/004-dataset-curation/catalog/license-ledger.template.json"
Task: "Create the attribution package template for permissive and attribution-required sources in specs/004-dataset-curation/catalog/attribution-package.template.md"
```

## Parallel Example: User Story 3

```text
Task: "Create the source identity template with required grouping fields in specs/004-dataset-curation/catalog/source-identity.template.json"
Task: "Create overfitting risk records for each evaluated candidate in specs/004-dataset-curation/catalog/overfitting-risks.json"
```

---

## Implementation Strategy

### MVP First (User Story 1 Only)

1. Complete Phase 1 setup.
2. Complete Phase 2 foundational contracts and vocabularies.
3. Complete Phase 3 User Story 1 structured candidate catalog and recommended dataset mix.
4. Stop and validate that reviewers can identify download priority, role, compatibility, and recommendation status without reading implementation code.

### Incremental Delivery

1. Setup + Foundational creates the schema and vocabulary base.
2. US1 delivers the MVP dataset shortlist and research download plan.
3. US2 adds licensing, attribution, provenance, and release eligibility governance.
4. US3 adds leakage, overfitting, source identity, and bias controls.
5. Polish validates claims, schemas, links, and handoff instructions.

### Stop Conditions

- Stop if any task marks restrictive, unknown, or mixed-license material as commercial-release-ready without legal approval.
- Stop if generated variants of the same source can cross train/validation/test splits.
- Stop if ODAQ or another validation-only source is included in training records.
- Stop if streaming, leaked, account-bound, or rights-unclear sources enter the recommended dataset mix.
- Stop if any task introduces plugin runtime, audio-thread, MCP mutation, CMake, or DAW state changes for this documentation/governance feature.

## Notes

- `[P]` tasks identify parallel opportunities; same-file report edits still need sequential merging.
- Every task is documentation, schema, or catalog governance work under `specs/004-dataset-curation/`.
- Validation output should be appended to `specs/004-dataset-curation/validation-notes.md`.
- Keep research-only and commercial-release-readiness language explicit throughout the catalog.
