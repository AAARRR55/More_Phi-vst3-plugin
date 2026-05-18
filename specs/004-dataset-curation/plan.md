# Implementation Plan: Neural Mastering Dataset Curation

**Branch**: `004-dataset-curation` | **Date**: 2026-05-18 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/004-dataset-curation/spec.md`

## Summary

Convert the dataset curation feature into reproducible planning artifacts for selecting, auditing, and governing neural mastering training data. The implementation plan keeps this feature offline and documentation-first: it formalizes dataset candidate records, license ledger entries, source-identity grouping, compatibility/risk scoring, and release-eligibility decisions so future ingestion and training can proceed without hiding licensing, provenance, or split-leakage risks.

## Technical Context

**Language/Version**: Markdown and JSON Schema for the planning deliverables; future ingestion scripts may use Python already present in `scripts/neural-mastering/`, but this plan does not require runtime code changes.

**Primary Dependencies**: `specs/004-dataset-curation/spec.md`, `specs/004-dataset-curation/dataset-curation-report.md`, existing neural mastering training plan in `specs/003-neural-mastering-roadmap/neural-training-implementation-plan.md`, existing offline dataset scripts in `scripts/neural-mastering/`, and More-Phi dataset governance requirements from the constitution.

**Storage**: Feature artifacts in `specs/004-dataset-curation/`; contracts in `specs/004-dataset-curation/contracts/`; no plugin runtime storage or persisted DAW state changes.

**Testing**: Manual/spec review of curation artifacts, schema validation for JSON examples when implementation proceeds, and future dataset-governance script validation before any training data is ingested.

**Target Platform**: Repository documentation and offline data-preparation workflows on developer machines or research workstations; no DAW/plugin runtime platform impact.

**Project Type**: JUCE audio plugin repository with offline AI/dataset research planning artifacts.

**Performance Goals**: A reviewer can identify download priority, dataset role, license posture, overfitting risk, and release-readiness blockers within 15 minutes; all evaluated datasets retain source links and curation status.

**Constraints**: No dataset is marked commercial-release-ready without item-level rights clearance. No audio-thread, hosted-plugin, state persistence, or MCP mutation behavior is introduced. Non-commercial, educational-only, share-alike, gated, mixed-license, or unclear-rights datasets remain research-only or hold-only.

**Scale/Scope**: Planning artifacts cover at least 10 dataset candidates, source-identity grouping, license ledger requirements, candidate scoring, compatibility evaluation, overfitting controls, validation roles, and rejected-source policy.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- Real-time safety: PASS. This feature creates offline specification, research, and contract artifacts only; no `processBlock()`-reachable code or model inference is added.
- Thread domains: PASS. No audio, message, MCP, or background-worker handoff is introduced. Future ingestion remains offline/background and outside hosted-plugin access.
- Parameter safety: PASS. No hosted-plugin parameter writes, normalized host values, or automation paths are modified.
- AI/MCP boundaries: PASS. The plan explicitly distinguishes research-only, validation-only, rejected, and commercial-blocked datasets, preserving honest AI/data claims.
- Persistence and compatibility: PASS. No APVTS, snapshot, hosted-plugin state, model-artifact persistence, or DAW compatibility change is required.
- Tests and validation: PASS. Validation is documentation/schema review now, with future dataset-governance checks required before ingestion or training.

## Project Structure

### Documentation (this feature)

```text
specs/004-dataset-curation/
├── spec.md
├── dataset-curation-report.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   ├── dataset-candidate.schema.json
│   ├── license-ledger.schema.json
│   ├── source-identity.schema.json
│   └── release-eligibility.schema.json
└── checklists/
    └── requirements.md
```

### Source Code (repository root)

```text
scripts/
└── neural-mastering/
    ├── audit_dataset.py
    ├── generate_mastering_dataset.py
    └── train_neural_mastering.py

config/
└── neural-mastering/
    └── dataset-governance.default.json
```

**Structure Decision**: Keep this feature in `specs/004-dataset-curation/` as planning and governance documentation. Existing offline scripts and dataset-governance config are referenced as future consumers, but no script, plugin runtime, CMake, or test target is changed by `/speckit-plan`.

## Phase 0: Research Decisions

Research is consolidated in [research.md](./research.md). Key decisions:

1. Use a staged data mix rather than a single dataset.
2. Treat SonicMasterDataset as the primary immediate research-pair candidate, not commercial-ready proof.
3. Use multitrack datasets for synthetic pair generation only after license and source-identity governance.
4. Reserve ODAQ for perceptual metric calibration rather than model training.
5. Reject streaming, leaked, account-bound, or rights-unclear sources.
6. Require item-level license ledger and source-level split isolation before ingestion.

## Phase 1: Design Outputs

- Data model: [data-model.md](./data-model.md)
- Dataset candidate contract: [contracts/dataset-candidate.schema.json](./contracts/dataset-candidate.schema.json)
- License ledger contract: [contracts/license-ledger.schema.json](./contracts/license-ledger.schema.json)
- Source identity contract: [contracts/source-identity.schema.json](./contracts/source-identity.schema.json)
- Release eligibility contract: [contracts/release-eligibility.schema.json](./contracts/release-eligibility.schema.json)
- Quickstart: [quickstart.md](./quickstart.md)
- Agent context: `AGENTS.md` Speckit pointer updated to `specs/004-dataset-curation/plan.md`

## Post-Design Constitution Check

- Real-time safety: PASS. All contracts describe offline metadata and governance records only.
- Thread domains: PASS. No runtime thread interaction is introduced.
- Parameter safety: PASS. Dataset curation does not alter hosted-plugin parameters or automation.
- AI/MCP boundaries: PASS. Contracts require evidence level, license posture, release status, and rejection reasons so model claims remain bounded.
- Persistence and compatibility: PASS. No plugin state migration or model-artifact loading is introduced.
- Tests and validation: PASS. Quickstart defines reproducible review checks, schema validation expectations, and future dataset-governance gates.

## Complexity Tracking

No constitution violations are introduced by this plan.
