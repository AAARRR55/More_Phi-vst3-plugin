# Research: Neural Mastering Dataset Curation

**Date**: 2026-05-18  
**Scope**: Dataset selection, licensing posture, compatibility, and overfitting controls for offline neural mastering research.

## Decision 1: Use a staged dataset mix

**Decision**: Use separate dataset roles: primary paired research, multitrack synthesis, reference distribution, perceptual validation, and rejected/hold sources.

**Rationale**: Public mastering-specific paired datasets are scarce. A staged mix lets the project train on direct degraded/high-quality pairs, broaden diversity through synthetic stem/mix generation, and calibrate perceptual metrics without pretending every dataset answers the same question.

**Alternatives considered**:

- Single primary corpus only: rejected because it concentrates bias and synthetic degradation risk.
- Only commercial music catalogs: rejected due to rights and reproducibility risk.
- Only multitrack datasets: rejected because they lack verified mastered/unmastered labels at sufficient scale.

## Decision 2: Treat SonicMasterDataset as primary immediate research data

**Decision**: Use SonicMasterDataset as the first direct paired research candidate, gated by source-ID split checks and license/provenance audit.

**Rationale**: It is the closest discovered public match to the current training contract: degraded input audio, high-quality target audio, sample-rate metadata, degradation metadata, prompts, genre/quality metadata, and mastering-relevant degradation families.

**Alternatives considered**:

- Cambridge-MT as primary: rejected for commercial restrictions and insufficient standardized paired labels.
- MTG-Jamendo/FMA as primary: rejected because they are reference-only corpora without paired degraded/unmastered inputs.
- SolidStateBusComp as primary: rejected because it is compressor-specific rather than full mastering.

## Decision 3: Use multitrack datasets for synthesis and robustness, not direct mastering labels

**Decision**: Cambridge-MT, MUSDB18-HQ, MedleyDB, MoisesDB, Open Multitrack Testbed, and Slakh2100 are classified as synthesis, augmentation, or robustness candidates unless manually relabeled or processed through controlled synthetic mastering chains.

**Rationale**: These datasets provide stems, mixtures, and metadata useful for pair generation and stress testing, but they generally lack verified mastered/unmastered target labels. Their strongest use is creating aligned training pairs with documented processing recipes and source-level split isolation.

**Alternatives considered**:

- Treat final mixes as mastered references: rejected because several datasets explicitly lack mastered-status labels.
- Treat stems as pre-master sources directly: rejected because stem mixes need controlled rendering to create meaningful mastering inputs.

## Decision 4: Keep restrictive datasets research-only until rights clearance

**Decision**: Non-commercial, share-alike, educational-only, gated, mixed-license, or unclear-rights datasets must remain research-only or hold-only until explicit legal approval exists.

**Rationale**: Training artifacts can inherit release risk from source data. The project cannot safely ship or market a model trained on material whose license excludes commercial use or whose attribution/share-alike obligations are unresolved.

**Alternatives considered**:

- Accept non-commercial datasets for product prototypes: rejected because prototypes can become release candidates and contaminate model lineage.
- Rely on dataset-level license summaries only: rejected because many music corpora use per-track or upstream licenses.

## Decision 5: Require item-level license ledger

**Decision**: Every candidate item selected for ingestion must have a license ledger entry with source URL, license, attribution, commercial eligibility, non-commercial/share-alike flags, checksum, and approval status.

**Rationale**: Dataset-level recommendations are not enough for music. Per-track and upstream licenses are common, and curation must remain auditable after files are downloaded, transformed, or used to train model artifacts.

**Alternatives considered**:

- Spreadsheet-only notes: rejected because downstream tooling needs structured fields.
- Dataset-level approvals only: rejected because item-level mixed licensing is common.

## Decision 6: Split by source identity, not file or generated pair

**Decision**: All future train/validation/test splits must group by stable source identity: original work, artist, album/session, upstream catalog ID, dataset origin, and derived render family.

**Rationale**: Generated variants from the same song can leak across splits and inflate validation metrics. This risk is acute for degradation datasets, compressor parameter sweeps, alternate masters, stems, and synthetic renders.

**Alternatives considered**:

- Random segment split: rejected because segments from one song can appear in multiple splits.
- Random file split: rejected because synthetic variants or stem renders may be separate files from the same source.

## Decision 7: Reserve ODAQ for perceptual metric calibration

**Decision**: ODAQ should be held out as a metric calibration and benchmark dataset, not used for waveform mastering training.

**Rationale**: ODAQ provides subjective quality scores for stereo audio processed through quality degradation classes. That is useful for checking objective metrics and artifact sensitivity, but it is too small and not mastering-pair-specific enough for model training.

**Alternatives considered**:

- Train a quality predictor directly on ODAQ as part of the main model: deferred because the dataset is small and task-mismatched.
- Ignore subjective-score datasets: rejected because perceptual metric calibration is needed before quality claims.

## Decision 8: Reject streaming, leaked, account-bound, and rights-unclear sources

**Decision**: Streaming-service audio, leaked stems, unofficial multitracks, account-bound commercial stems, and rights-unclear uploads are excluded from training and validation.

**Rationale**: These sources have high legal risk, weak provenance, and poor reproducibility. They would undermine dataset governance and release-readiness review.

**Alternatives considered**:

- Use them only for private experiments: rejected because private experiments can contaminate models or evaluation habits.
- Use separated stems from commercial songs: rejected because derivative data rights are not granted by ordinary streaming or purchase access.
