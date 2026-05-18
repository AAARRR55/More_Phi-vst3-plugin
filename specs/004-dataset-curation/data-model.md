# Data Model: Neural Mastering Dataset Curation

## DatasetCandidate

Represents one discovered dataset or source category evaluated for neural mastering.

**Fields**:

- `id`: Stable slug, e.g. `sonicmaster-dataset`.
- `name`: Human-readable dataset/source name.
- `sourceUrl`: Primary source URL.
- `sourceCategory`: `academic`, `open-source`, `industry-standard`, `commercial`, or `rejected-source`.
- `datasetType`: `paired`, `multitrack`, `reference-only`, `quality-benchmark`, `effect-specific`, `synthetic`, or `commercial-source`.
- `scaleSummary`: Human-readable size, duration, row count, or catalog count.
- `audioFormat`: Sample rate, bit depth, channel layout, encoding, and alignment notes when known.
- `labelingType`: Pair labels, stems, metadata tags, subjective scores, prompts, process parameters, or none.
- `recommendedRole`: Primary training, synthesis, auxiliary pretraining, validation, reference distribution, metric calibration, hold, or reject.
- `recommendationStatus`: `recommended`, `conditional`, `research-only`, `validation-only`, `hold`, or `rejected`.
- `compatibilityScore`: 1-5 score from the curation rubric.
- `licensePostureId`: Link to a `LicensePosture`.
- `overfittingRiskId`: Link to an `OverfittingRisk`.
- `requiredFollowUps`: List of follow-up actions before ingestion or release use.
- `evidenceLinks`: Source links used to justify the evaluation.

**Validation Rules**:

- Must include at least one source URL.
- Must include a license posture before any role other than `rejected`.
- Must not be marked commercial-ready when license posture has non-commercial, educational-only, share-alike, gated, mixed, or unknown restrictions.
- Must include overfitting and leakage notes for every candidate.

## DatasetRole

Defines how a dataset may be used in the training and validation strategy.

**Fields**:

- `role`: `primary-paired-training`, `synthetic-pair-generation`, `auxiliary-pretraining`, `reference-distribution`, `perceptual-validation`, `pipeline-debugging`, `hold`, or `reject`.
- `allowedUse`: Plain-language allowed use.
- `disallowedUse`: Plain-language prohibited use.
- `requiredControls`: Split, license, source, or metric controls required before use.

**Validation Rules**:

- A candidate can have multiple roles only if each role has distinct controls.
- `perceptual-validation` datasets must not be included in training unless a later plan explicitly changes that status.
- `hold` and `reject` roles cannot also be primary training roles.

## LicensePosture

Captures rights and obligations at dataset or item level.

**Fields**:

- `licenseId`: Stable license/category ID.
- `declaredLicense`: Declared license text or SPDX-like label when available.
- `licensePattern`: `permissive`, `attribution`, `non-commercial`, `share-alike`, `educational-only`, `mixed-per-track`, `gated`, `unknown`, or `rejected`.
- `commercialEligibility`: `eligible`, `blocked`, `requires-review`, or `unknown`.
- `requiresAttribution`: Boolean.
- `requiresShareAlike`: Boolean.
- `nonCommercialOnly`: Boolean.
- `upstreamLicenseRequired`: Boolean.
- `approvalStatus`: `not-reviewed`, `research-approved`, `commercial-approved`, `blocked`, or `rejected`.
- `approvalNotes`: Required legal or rights notes.

**Validation Rules**:

- `commercialEligibility` must be `blocked` or `requires-review` when `nonCommercialOnly` is true.
- `commercial-approved` requires a source URL and approval note.
- Unknown licenses must block ingestion into release-intent training sets.

## SourceIdentity

Groups all variants derived from the same musical work or source session.

**Fields**:

- `sourceIdentityId`: Stable internal ID.
- `datasetIds`: Datasets where the source appears.
- `workId`: Original work/song ID when known.
- `artistId`: Artist or creator identity.
- `albumOrSessionId`: Album, session, project, or recording batch.
- `upstreamCatalogId`: Jamendo, FMA, Cambridge, MUSDB, MedleyDB, or other origin ID.
- `derivedFamilyId`: Shared ID for stems, alternates, degraded variants, compressor settings, or mastered renders.
- `splitAssignment`: `train`, `validation`, `test`, `holdout`, or `excluded`.
- `knownOverlaps`: Other datasets or candidates suspected to share the same source.

**Validation Rules**:

- All variants sharing a `sourceIdentityId` must share one split assignment.
- Generated variants must include a `derivedFamilyId`.
- Known overlap with another dataset must be resolved before validation metrics are accepted.

## CompatibilityAssessment

Evaluates whether a dataset can be used by the neural mastering training pipeline.

**Fields**:

- `candidateId`: Dataset candidate link.
- `stereoReady`: Boolean.
- `sampleRateReady`: Boolean or required conversion note.
- `pairedReady`: Boolean.
- `alignmentRisk`: `low`, `medium`, `high`, or `unknown`.
- `labelAccuracy`: `verified`, `derived`, `synthetic`, `ambiguous`, or `missing`.
- `pipelineAdapterNeeded`: List of required transformations.
- `recommendedSegmentPolicy`: Crop/pad/downmix/upmix/resample notes.
- `qualityScreeningRequired`: Boolean.

**Validation Rules**:

- `primary-paired-training` requires `pairedReady` true or a documented synthetic-pair recipe.
- Mono-only datasets must not be marked stereo-ready.
- Lossy-encoded datasets require quality screening before waveform reconstruction training.

## OverfittingRisk

Documents leakage and bias risks.

**Fields**:

- `candidateId`: Dataset candidate link.
- `riskLevel`: `low`, `medium`, `high`, or `critical`.
- `riskFactors`: Duplicate source, small catalog, repeated parameter sweeps, artist overlap, synthetic recipes, genre skew, mono-only, lossy encoding, or label ambiguity.
- `mitigations`: Required split, filtering, ablation, validation, or holdout actions.
- `cannotProve`: Claims this dataset cannot support.

**Validation Rules**:

- Repeated derived variants require source-level split mitigation.
- Synthetic datasets must declare that they cannot prove real pre-master performance alone.
- Small or narrow datasets cannot be sole validation evidence.

## CurationReport

The sourced decision artifact for stakeholders.

**Fields**:

- `reportDate`: Creation/update date.
- `targetModel`: Model or training contract the report targets.
- `decisionPosture`: Research, prototype, release-intent, or blocked.
- `datasetCandidates`: Candidate list.
- `recommendedMix`: Dataset roles and initial download order.
- `rejectedSources`: Explicitly rejected source categories.
- `sourceLinks`: Source references.
- `readinessDecision`: Whether training may proceed and under what boundaries.

**Validation Rules**:

- Must evaluate at least 10 candidates.
- Every candidate must have source, recommendation status, license posture, compatibility assessment, and overfitting note.
- Must not claim production model readiness.

## ReleaseEligibility

Summarizes whether a dataset or dataset mix can be used for research, prototype, or product-intent training.

**Fields**:

- `scope`: Dataset, subset, item, or dataset mix.
- `eligibility`: `research-ready`, `prototype-ready`, `commercial-blocked`, `commercial-review-required`, `commercial-ready`, or `rejected`.
- `blockingIssues`: Missing rights, mixed licenses, source leakage, quality mismatch, missing provenance, or legal review.
- `requiredEvidence`: License ledger, checksums, source identity, split report, quality screen, objective metrics, listening evidence.
- `owner`: Responsible role for clearing the decision.

**Validation Rules**:

- `commercial-ready` requires zero unresolved blocking issues.
- A dataset containing any non-commercial item must not be commercial-ready unless that item is excluded or separately licensed.
- Release eligibility must be updated after any dataset membership change.
