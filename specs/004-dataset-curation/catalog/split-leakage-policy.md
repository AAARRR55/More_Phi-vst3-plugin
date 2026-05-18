# Split and Leakage Policy

## Rule 1: Split by Source Identity

Assign `train`, `validation`, `test`, `holdout`, or `excluded` at the `sourceIdentityId` level, never at the segment, file, render, degradation, stem, compressor-setting, or prompt level.

## Rule 2: Keep Derived Families Together

All variants sharing a `derivedFamilyId` must share one split:

- Stems and mixes from the same recording project.
- Alternate masters or mix revisions.
- SonicMaster degradation variants from the same source.
- SolidStateBusComp parameter sweeps from the same unmastered song.
- Synthetic renders from the same MIDI source.

## Rule 3: Resolve Cross-Dataset Overlap Before Metrics

If a source is suspected to appear in multiple datasets, either:

1. Place all related sources in the same split, or
2. Exclude the source from validation and test sets until reviewed.

## Rule 4: Hold Validation-Only Material Out of Training

ODAQ and other perceptual validation sources must be marked `held-out-only` and excluded from training manifests.

## Rule 5: Record Unknowns as Blockers

If source identity, artist identity, upstream catalog ID, or derived family cannot be determined, the item must be assigned to `holdout` or `excluded` until a reviewer resolves it.

## Required Evidence

Every training manifest must include:

- Source identity report.
- Derived family grouping report.
- Split assignment summary.
- Known overlap resolution table.
- Validation-only exclusion list.

## Stop Conditions

- A generated variant can cross train/validation/test splits.
- A source has unknown overlap status and appears in validation or test.
- ODAQ or another validation-only source appears in training.
- A rejected or rights-unclear source appears in any split.
