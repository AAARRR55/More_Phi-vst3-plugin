# Neural Mastering Dataset Curation

This feature curates datasets for the offline neural mastering research pipeline. It is documentation and governance work only; it does not alter plugin runtime, audio-thread behavior, CMake targets, MCP tools, or DAW state.

## Core Documents

- [Specification](./spec.md)
- [Dataset Curation Report](./dataset-curation-report.md)
- [Implementation Plan](./plan.md)
- [Research Decisions](./research.md)
- [Data Model](./data-model.md)
- [Quickstart](./quickstart.md)
- [Tasks](./tasks.md)

## Catalog Outputs

Structured catalog records live in [`catalog/`](./catalog/). They are intended to make the prose curation report auditable before any dataset is downloaded or used for training.

## Contracts

Machine-readable contracts live in [`contracts/`](./contracts/):

- `dataset-candidate.schema.json`
- `license-ledger.schema.json`
- `source-identity.schema.json`
- `release-eligibility.schema.json`

## Implementation Boundary

All datasets remain research-only or blocked unless their license ledger, source identity report, split policy, and release eligibility records explicitly clear the intended use.
