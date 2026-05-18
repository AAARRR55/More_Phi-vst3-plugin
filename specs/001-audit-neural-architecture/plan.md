# Implementation Plan: Neural Architecture Audit

**Branch**: `001-audit-neural-architecture` | **Date**: 2026-05-18 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/001-audit-neural-architecture/spec.md`

**Note**: This template is filled in by the `/speckit.plan` command. See `.specify/templates/plan-template.md` for the execution workflow.

## Summary

Produce a stakeholder-readable technical audit that determines the optimal neural architecture for More-Phi's AI-assisted plugin-control success. The plan treats the primary learning task as bounded control/parameter recommendation conditioned on audio features, plugin metadata, text intent, snapshots, and optional temporal trajectories; it rejects raw audio generation as the primary path unless later evidence overturns that assumption. The recommended architecture to evaluate in the audit is a compact hybrid multimodal temporal Transformer with shallow audio-feature encoding and parameter/type embeddings, plus a retrieval-and-constrained-regression fallback baseline.

## Technical Context

**Language/Version**: C++20 JUCE 8 plugin project; planning artifacts are Markdown deliverables and do not add runtime code.

**Primary Dependencies**: Existing project context: JUCE 8.0.4, CMake, Catch2, nlohmann/json, More-Phi dataset generation, MCP/AI modules, hosted-plugin parameter infrastructure. Future proof-of-concept work may evaluate optional CPU-safe model runtimes, but this plan does not select or add one.

**Storage**: Spec Kit planning documents under `specs/001-audit-neural-architecture/`; audit evidence may reference existing dataset metadata, parameter JSON files, and generated audio-feature artifacts.

**Testing**: Specification checklist review, audit-report contract review, and manual validation against acceptance scenarios. Future implementation phases should add benchmark or proof-of-concept validation before any runtime model integration.

**Target Platform**: More-Phi VST3/AU plugin ecosystem, standalone MCP server, dataset/CLI workflows, and DAW plugin-host environments on Windows/macOS/CI-supported platforms.

**Project Type**: Audio plugin with CLI, embedded/standalone MCP, hosted-plugin parameter control, dataset generation, and AI-assistant control-plane features.

**Performance Goals**: Audit must define model-success targets for expert-reference agreement, safe-parameter behavior, perceived usefulness, and latency acceptability. Runtime recommendations must keep neural inference off the audio callback; any live control-plane inference target should be low-rate, bounded, cached, and validated before adoption.

**Constraints**: Audio-thread paths must not allocate, block, do I/O, throw, run model inference, or mutate hosted-plugin state directly. Hosted-plugin parameters use normalized values and may span up to 2048 parameters with masks, classifications, safety ranges, and variable availability. Dataset features include scalar audio descriptors and optional mel-spectrogram tensors; text/LLM intent must remain control-plane data.

**Scale/Scope**: Audit covers at least five model families, all specified data modalities, input/output dimensionality categories, real-time suitability, training hyperparameters, losses, evaluation metrics, risks, and next actions. No plugin runtime implementation is in scope for this plan.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- Real-time safety: PASS. The plan is documentation-only and the audit explicitly classifies audio-callback inference as prohibited unless a later proof demonstrates bounded, allocation-free, non-blocking behavior.
- Thread domains: PASS. Runtime recommendations must classify work as audio, message, MCP, background, or offline and must use queues, atomics, seqlock/double-buffered state, or message-thread deferral for any future handoff.
- Parameter safety: PASS. The audit must preserve normalized values, stable identity, masks, classifications, high-risk controls, safety ranges, and bounded deltas in all recommendations.
- AI/MCP boundaries: PASS. The audit must distinguish deterministic, heuristic, and learned behavior; recommendations must preserve JSON-RPC/auditable automation boundaries and avoid overstated intelligence claims.
- Persistence and compatibility: PASS. No state schema change is planned; recommendations must consider APVTS, snapshot, hosted-plugin state, graceful model absence, and DAW/plugin-host compatibility before future adoption.
- Tests and validation: PASS. Planning validation is document review against spec and contract; future implementation requires proof-of-concept latency, quality, safety, and generalization benchmarks.

**Post-Design Re-check**: PASS. Phase 0 and Phase 1 artifacts keep the work as an audit deliverable, define real-time-safe runtime boundaries, and add validation contracts without introducing code or architecture violations.

## Project Structure

### Documentation (this feature)

```text
specs/001-audit-neural-architecture/
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   └── audit-report-contract.md
├── checklists/
│   └── requirements.md
└── tasks.md             # Future output from /speckit-tasks
```

### Source Code (repository root)

```text
src/
├── AI/                  # Evidence source for MCP, assistant, dataset, and future control-plane integration
├── Core/                # Evidence source for parameter state, snapshots, and real-time morphing constraints
├── Host/                # Evidence source for hosted-plugin lifecycle and parameter bridging
├── Plugin/              # Evidence source for audio-thread lifecycle and processor ownership
└── UI/                  # Evidence source for message-thread interaction and user-facing control paths

tests/
├── Unit/                # Future unit coverage if model-selection utilities are implemented
└── Integration/         # Future integration coverage if runtime AI/MCP behavior changes

specs/001-audit-neural-architecture/
└── audit-report.md      # Future implementation deliverable, governed by contracts/audit-report-contract.md
```

**Structure Decision**: This is a documentation and technical-analysis feature. The current planning phase creates only Spec Kit artifacts; source directories are evidence sources and future integration targets, not files to modify during planning.

## Complexity Tracking

No constitution violations or complexity exceptions are required for this plan.
