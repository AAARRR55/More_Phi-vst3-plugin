# Feature Specification: Neural Mastering Dataset Curation

**Feature Branch**: `004-dataset-curation`

**Created**: 2026-05-18

**Status**: Draft

**Input**: User description: "Identify and curate high-quality, relevant datasets suitable for training our specific model architecture, ensuring they align with our target domain, data modalities, and performance objectives. Conduct a comprehensive search across academic repositories, open-source platforms, and industry-standard databases to find datasets that provide sufficient scale, diversity, and labeling accuracy. Evaluate each potential dataset based on its compatibility with our training pipeline, potential for overfitting, and adherence to licensing requirements."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Select Viable Training Data (Priority: P1)

As an ML lead, I want a curated shortlist of datasets that match the neural mastering target domain so that model training can begin from legally usable, technically compatible, and musically relevant material.

**Why this priority**: Training quality depends more on source data relevance and rights than on model scale. The project must avoid building on datasets that cannot support stereo mastering objectives or cannot be used under the intended research and product posture.

**Independent Test**: A reviewer can inspect the curation report and identify which datasets are recommended, conditionally usable, or rejected, with a clear reason for each decision.

**Acceptance Scenarios**:

1. **Given** the neural mastering architecture requires paired or synthesizable stereo audio, **When** the curation report is reviewed, **Then** every recommended dataset is mapped to its usable training role.
2. **Given** a dataset has unclear or restrictive rights, **When** the curation report is reviewed, **Then** the report states the licensing risk and required approval before use.
3. **Given** a dataset is high scale but mismatched to mastering, **When** the curation report is reviewed, **Then** it is not promoted as primary training data without a bounded role.

---

### User Story 2 - Govern Licensing and Provenance (Priority: P2)

As a release owner, I want each candidate dataset evaluated for license terms, provenance, and commercial-readiness risk so that no model artifact inherits unresolved legal or attribution obligations.

**Why this priority**: Audio datasets frequently mix Creative Commons, non-commercial, educational-only, and per-track licenses. Misclassification can block product release even if research results look strong.

**Independent Test**: A reviewer can trace every dataset recommendation to a license category, source reference, and required follow-up action.

**Acceptance Scenarios**:

1. **Given** a dataset permits research only, **When** it appears in the shortlist, **Then** the report marks it as research-only and excludes it from commercial release training until rights are cleared.
2. **Given** a dataset has per-track licenses, **When** it appears in the shortlist, **Then** the report requires item-level license capture before ingestion.
3. **Given** a dataset is derived from another source, **When** it appears in the shortlist, **Then** the report identifies upstream license or provenance checks.

---

### User Story 3 - Reduce Overfitting and Domain Bias (Priority: P3)

As a validation owner, I want dataset diversity, split-leakage risk, and overfitting risk documented so that training and evaluation represent real mastering behavior instead of memorizing songs, artists, stems, or degradation recipes.

**Why this priority**: Mastering models can overfit to source catalogs, repeated stems, synthetic transformations, or genre imbalance. Curation must define enough controls to make future metrics meaningful.

**Independent Test**: A reviewer can verify that every recommended dataset includes overfitting risks and split requirements.

**Acceptance Scenarios**:

1. **Given** a dataset contains multiple versions of the same source work, **When** it is reviewed, **Then** the report requires source-level split isolation.
2. **Given** a dataset is synthetic or algorithmically degraded, **When** it is reviewed, **Then** the report warns against using it as the sole evidence for professional mastering quality.
3. **Given** a dataset is small or genre-skewed, **When** it is reviewed, **Then** the report assigns it to validation, augmentation, or targeted training rather than broad primary training.

### Edge Cases

- A dataset may be musically relevant but unusable for commercial model training because it is non-commercial, educational-only, or per-track licensed.
- A dataset may provide stems and final mixes but no verified mastered/unmastered labels.
- A dataset may contain high-quality mastered tracks but no corresponding unmastered input.
- A dataset may be large enough for training but synthetic enough to inflate validation results.
- A dataset may include duplicate songs, alternate renders, source-derived variants, or shared artists across splits.
- A dataset may be technically compatible after resampling but still unsuitable because of mono-only audio, lossy encoding, missing provenance, or unclear rights.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The curation MUST evaluate datasets from academic repositories, open-source platforms, and industry-standard or commercial music data sources.
- **FR-002**: Each evaluated dataset MUST include source, access path, scale, audio modality, labeling type, license posture, model-training role, and recommendation status.
- **FR-003**: The curation MUST distinguish primary training candidates, auxiliary pretraining candidates, validation candidates, and rejected or hold candidates.
- **FR-004**: The curation MUST identify whether each dataset supports paired degraded/high-quality audio, multitrack-to-mix synthesis, reference-only modeling, subjective quality validation, or effect-specific pretraining.
- **FR-005**: The curation MUST assess compatibility with stereo waveform mastering objectives, including sample-rate suitability, channel layout, alignment needs, and segment availability.
- **FR-006**: The curation MUST document licensing obligations and restrictions, including non-commercial, share-alike, educational-only, attribution, per-track, gated-access, or unclear-rights requirements.
- **FR-007**: The curation MUST identify overfitting and leakage risks, including duplicated sources, artist overlap, genre imbalance, synthetic degradation repetition, and source-derived variants.
- **FR-008**: The curation MUST provide a recommended dataset mix for initial research training, validation, and commercial-readiness review.
- **FR-009**: The curation MUST identify datasets or data sources that should not be used without additional legal approval or rights clearance.
- **FR-010**: The curation MUST include citations or source links for every dataset recommendation.
- **FR-011**: The curation MUST include a readiness decision for whether immediate training may proceed, whether research-only restrictions apply, and what evidence remains required before product use.
- **FR-012**: The curation MUST not claim that any model trained on the recommended datasets is production-ready without later objective, listening, provenance, and release-gate validation.

### Key Entities *(include if feature involves data)*

- **Dataset Candidate**: A discovered dataset with source, scope, modality, labels, license, access status, risks, and recommendation.
- **Dataset Role**: The intended use of a dataset, such as primary training, auxiliary pretraining, validation, benchmarking, augmentation, or rejection.
- **License Posture**: The allowed and restricted uses of a dataset, including research-only, commercial-cleared, attribution-only, share-alike, or rights-unknown status.
- **Compatibility Assessment**: The evaluation of whether the dataset can feed the neural mastering training process without excessive conversion, label ambiguity, or modality mismatch.
- **Overfitting Risk**: The likelihood that model results will be inflated by duplicate songs, repeated artists, small scale, source leakage, synthetic recipes, or narrow genre coverage.
- **Curation Report**: The sourced artifact containing the evaluated dataset catalog, scoring, recommendations, and follow-up actions.

## Constitution Alignment *(mandatory)*

- **Real-time/audio impact**: No runtime audio-thread behavior changes are introduced. This feature produces dataset and research guidance only.
- **Threading and hosted-plugin boundary**: No hosted-plugin access or cross-thread runtime path is introduced. Any future ingestion or rendering remains offline/background work.
- **AI/MCP/security impact**: The curation must preserve honest AI boundaries by distinguishing research data, validation data, and commercial-release-eligible data. Dataset provenance and licensing restrictions must be explicit before any model artifact is trusted.
- **Platform/build/state impact**: No plugin state, build target, or platform packaging change is required by the curation deliverable.
- **Validation scope**: Review the curation report against source citations, license posture, modality compatibility, overfitting risk, and the Spec Kit checklist. Future ingestion must run separate dataset-governance validation before training.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: The curation report evaluates at least 10 distinct candidate datasets or data sources across academic, open-source, and industry-standard sources.
- **SC-002**: 100% of evaluated candidates include a recommendation status, license posture, compatibility assessment, and overfitting-risk note.
- **SC-003**: At least 3 candidates are identified as viable for immediate research use, with explicit boundaries on what they can and cannot prove.
- **SC-004**: 100% of primary or conditional training candidates include required follow-up actions before commercial model release.
- **SC-005**: The final recommendation includes a dataset mix for initial research training, held-out validation, and perceptual-quality benchmarking.
- **SC-006**: No candidate with unresolved or restrictive licensing is marked as commercial-release-ready.
- **SC-007**: A reviewer can determine within 15 minutes which datasets to download first, which to reserve for validation, and which to exclude.

## Assumptions

- The target model is the offline stereo neural mastering architecture described in the neural mastering roadmap, not an audio-thread runtime model.
- Initial dataset use is research and prototype evaluation unless a dataset is explicitly cleared for commercial model training.
- Item-level license capture remains mandatory for per-track licensed collections.
- Professional mastered/unmastered paired datasets are scarce, so the recommended strategy may combine direct paired degradation datasets, multitrack synthesis sources, reference-only mastered catalogs, and perceptual validation benchmarks.
- Legal review is required before any trained model is shipped or marketed if training used non-commercial, educational-only, share-alike, gated, or mixed-license material.
