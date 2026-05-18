# Quickstart: Neural Mastering Dataset Curation

This quickstart verifies the planning artifacts for `004-dataset-curation` and defines the next safe steps before any dataset ingestion or training.

## 1. Review the Curation Outputs

Read these files in order:

1. [spec.md](./spec.md)
2. [dataset-curation-report.md](./dataset-curation-report.md)
3. [research.md](./research.md)
4. [data-model.md](./data-model.md)
5. [plan.md](./plan.md)

Expected result: a reviewer can identify recommended, conditional, validation-only, hold, and rejected datasets within 15 minutes.

## 2. Check Contract Coverage

Confirm these contracts exist:

```text
specs/004-dataset-curation/contracts/dataset-candidate.schema.json
specs/004-dataset-curation/contracts/license-ledger.schema.json
specs/004-dataset-curation/contracts/source-identity.schema.json
specs/004-dataset-curation/contracts/release-eligibility.schema.json
```

Expected result: each major governance entity from `data-model.md` has a machine-readable contract.

## 3. Validate No Release-Unsafe Claims

Search the feature docs for commercial-ready claims:

```powershell
rg -n "commercial-ready|production-ready|release-ready" specs/004-dataset-curation
```

Expected result: any occurrence is framed as blocked, conditional, or requiring evidence unless a future legal review explicitly clears it.

## 3a. Validate JSON Contracts and Catalog Records

Parse all schema and catalog JSON files:

```powershell
python -m json.tool specs/004-dataset-curation/contracts/dataset-candidate.schema.json > $null
python -m json.tool specs/004-dataset-curation/contracts/license-ledger.schema.json > $null
python -m json.tool specs/004-dataset-curation/contracts/source-identity.schema.json > $null
python -m json.tool specs/004-dataset-curation/contracts/release-eligibility.schema.json > $null
python -m json.tool specs/004-dataset-curation/catalog/dataset-candidates.json > $null
python -m json.tool specs/004-dataset-curation/catalog/compatibility-assessments.json > $null
python -m json.tool specs/004-dataset-curation/catalog/recommended-dataset-mix.json > $null
python -m json.tool specs/004-dataset-curation/catalog/license-ledger.template.json > $null
python -m json.tool specs/004-dataset-curation/catalog/license-postures.json > $null
python -m json.tool specs/004-dataset-curation/catalog/release-eligibility.json > $null
python -m json.tool specs/004-dataset-curation/catalog/source-identity.template.json > $null
python -m json.tool specs/004-dataset-curation/catalog/source-overlap-risk-register.json > $null
python -m json.tool specs/004-dataset-curation/catalog/overfitting-risks.json > $null
python -m json.tool specs/004-dataset-curation/catalog/dataset-roles.json > $null
python -m json.tool specs/004-dataset-curation/catalog/license-patterns.json > $null
```

Expected result: all commands exit successfully.

If a full JSON Schema validator is installed, validate each record in:

- `catalog/dataset-candidates.json` against `contracts/dataset-candidate.schema.json`
- `catalog/license-ledger.template.json` against `contracts/license-ledger.schema.json`
- each item in `catalog/release-eligibility.json` against `contracts/release-eligibility.schema.json`
- `catalog/source-identity.template.json` against `contracts/source-identity.schema.json`

## 4. Verify Required Dataset Fields

Before ingestion work starts, every selected item must be representable as:

- Dataset candidate record.
- License ledger entry.
- Source identity group.
- Release eligibility decision.

Expected result: no dataset is downloaded into a training set without source URL, license posture, checksum plan, split identity, and approval status.

## 5. First Research Download Order

Use the report's recommended order:

1. SonicMasterDataset for direct paired research, after source-ID and license audit.
2. Cambridge-MT mastering-tagged projects for non-commercial real pre-master validation.
3. MUSDB18-HQ and MedleyDB for multitrack synthesis and deduplication.
4. MoisesDB after taxonomy and license checks.
5. ODAQ for metric calibration only.
6. MTG-Jamendo or FMA only after license-filtered reference subsets are defined.

Expected result: training begins only in a research sandbox and never from rejected or rights-unclear sources.

### Research Sandbox Checklist

Before the first download, create a research-only sandbox record with:

- Dataset candidate ID from `catalog/dataset-candidates.json`.
- Intended role from `catalog/dataset-roles.json`.
- License posture ID from `catalog/license-postures.json` once US2 records are complete.
- Source identity grouping plan from `catalog/source-identity.template.json` once US3 records are complete.
- Download owner and date.
- Checksum plan for every downloaded file.
- Explicit statement that the sandbox is not commercial-release-ready.

Expected result: every downloaded file has a planned catalog, license, source identity, and checksum record before it can enter a training manifest.

## 6. Required Gate Before Training

Before running `scripts/neural-mastering/train_neural_mastering.py`, produce:

- A license ledger for the selected subset.
- A source identity report proving no train/validation/test leakage.
- A quality screen for sample rate, channel layout, codec quality, clipping, and silent/corrupt files.
- A release eligibility decision marking the subset `research-ready` or stricter.

Expected result: no training manifest is treated as valid without provenance, rights, split, and quality evidence.

## 7. What Not To Do

- Do not use streaming-service audio, leaked stems, or unofficial uploads.
- Do not split generated variants of the same source across train and validation.
- Do not use ODAQ for model training.
- Do not treat non-commercial or educational-only datasets as commercial model training data.
- Do not claim professional mastering quality from synthetic degradation validation alone.

## 8. Final Reviewer Handoff

Use this handoff sequence before `/speckit-implement` completion or dataset ingestion:

1. Confirm all tasks in [tasks.md](./tasks.md) are checked.
2. Review [validation-notes.md](./validation-notes.md) for schema, catalog, release-claim, and source-link checks.
3. Review `catalog/recommended-dataset-mix.json` for first download order.
4. Review `catalog/release-eligibility.json` for blocked or review-required statuses.
5. Review `catalog/split-leakage-policy.md` and `catalog/source-overlap-risk-register.json` before creating any training split.
6. Confirm no training manifest exists without license ledger, source identity, split, and quality evidence.
