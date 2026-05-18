# Implementation Plan: Neural Mastering Framework

**Branch**: `002-neural-mastering-framework` | **Date**: 2026-05-18 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/002-neural-mastering-framework/spec.md`

## Summary

Produce an exhaustive technical audit and architectural feasibility study for neural mastering/mixing in More-Phi. The plan recommends a hybrid framework: deterministic mastering DSP remains responsible for audio-rate processing, while a compact causal Temporal Convolutional Network (TCN) operates in offline, preview, background, or low-rate control-plane contexts to emit bounded mastering targets, parameter plans, masks, and confidence scores; any direct neural audio transformation remains research-only until proven real-time safe.

## Technical Context

**Language/Version**: Documentation and planning artifacts for a C++20 JUCE 8 VST3/AU plugin; optional future model training/prototyping can be language-agnostic but must export auditable, CPU-safe artifacts before runtime consideration.

**Primary Dependencies**: Existing More-Phi architecture, JUCE/CMake build context, dataset generation concepts under `src/AI/Dataset/`, mastering DSP components under `src/Core/`, MCP/AI automation boundaries under `src/AI/`, and optional future CPU inference backend such as ONNX Runtime only after proof-of-concept validation.

**Storage**: Feature artifacts in `specs/002-neural-mastering-framework/`; no production storage changes in this planning phase. Future training artifacts must preserve dataset provenance, plugin/version context, parameter schemas, split metadata, and model card information.

**Testing**: Planning validation through checklist review, contract conformance, quickstart review, and future CMake/Catch2/performance/listening validation gates before implementation. No executable code is introduced by this plan.

**Target Platform**: Professional DAW environments via More-Phi VST3 on Windows/Linux/macOS and AU on macOS, with conservative CPU-only assumptions for real-time feasibility.

**Project Type**: Audio plugin architecture audit and implementation roadmap; no runtime code changes in this Speckit planning phase.

**Performance Goals**: Final audit must define go/no-go targets for objective audio quality, blind listening scores, real-time latency, CPU headroom, bypass safety, artifact rate, model availability fallback, and deterministic behavior under low buffer sizes.

**Constraints**: Audio callback remains free of model inference, allocation, blocking I/O, locks, exceptions, and dynamic latency changes unless a future proof demonstrates bounded behavior. Neural outputs must be bounded controls, parameter vectors, masks, embeddings, plans, or analysis reports unless direct audio processing passes stricter proof gates.

**Scale/Scope**: Covers spectral balance, dynamic range control, stereo imaging, harmonic enhancement, multi-channel PCM inputs, temporal dependencies from transient to whole-track timescales, candidate architecture comparison, training/loss strategy, inference safeguards, and staged roadmap through proof-of-concept validation.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- Real-time safety: PASS. The plan is documentation-only and recommends keeping neural inference off `processBlock()` by default; any future exception requires measured bounded, allocation-free, non-blocking execution.
- Thread domains: PASS. Recommended neural workflows are offline, preview, background, message-thread, or queued low-rate control outputs; no unsafe hosted-plugin mutation is planned.
- Parameter safety: PASS. The plan requires normalized bounded outputs, parameter masks, type-aware ranges, max-delta limits, smoothing, confidence gating, and deterministic fallback.
- AI/MCP boundaries: PASS. Learned behavior is framed as auditable recommendations or bounded automation, not unavailable calibrated intelligence; no remote inference is required for real-time audio.
- Persistence and compatibility: PASS. This phase makes no runtime persistence changes; future model artifacts must include provenance, schema, versioning, and disabled/unavailable fallback behavior.
- Tests and validation: PASS. The plan defines objective benchmarks, blind listening tests, contract checks, performance gates, and proof-of-concept measurements before implementation approval.

## Project Structure

### Documentation (this feature)

```text
specs/002-neural-mastering-framework/
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   └── audit-report-contract.md
└── checklists/
    └── requirements.md
```

### Source Code (repository root)

```text
CLAUDE.md
specs/002-neural-mastering-framework/
```

**Structure Decision**: This phase is a Speckit planning and audit-design deliverable. It creates and updates only feature planning artifacts plus the Speckit plan pointer in `CLAUDE.md`; production source files, tests, CMake targets, and plugin runtime code are intentionally out of scope until `/speckit-tasks` and implementation planning authorize them.

## Phase 0: Research Decisions

Research is consolidated in [research.md](./research.md). The key resolved decisions are:

1. Primary architecture direction is hybrid deterministic DSP plus compact causal TCN control-plane inference.
2. Direct neural audio transformation is not the default production path because it is too risky for real-time, transparent mastering without proof.
3. Offline teacher models may be used for research quality, then distilled to bounded CPU-friendly students for preview/control workflows.
4. Losses must combine multi-resolution spectral, complex/phase, waveform, loudness, stereo, perceptual, identity, and artifact penalties.
5. Signal integrity gates must include clipping, true peak, phase correlation, mono compatibility, aliasing, bypass continuity, CPU headroom, and fallback behavior.

## Phase 1: Design Outputs

- Data model: [data-model.md](./data-model.md)
- Contract: [contracts/audit-report-contract.md](./contracts/audit-report-contract.md)
- Quickstart: [quickstart.md](./quickstart.md)
- Agent context: `CLAUDE.md` Speckit pointer updated to `specs/002-neural-mastering-framework/plan.md`

## Post-Design Constitution Check

- Real-time safety: PASS. Design contracts explicitly classify runtime paths and reject audio-callback inference unless future proof gates pass.
- Thread domains: PASS. Entities separate offline analysis, background inference, control-plan output, deterministic DSP, and review artifacts.
- Parameter safety: PASS. Contracts require bounded target controls, semantic range documentation, smoothing/fallback rules, and high-risk control handling.
- AI/MCP boundaries: PASS. Report contract requires evidence levels and explicit confidence/unknown handling rather than overclaiming model capability.
- Persistence and compatibility: PASS. No production persistence change is planned; future artifacts must include dataset/model provenance and version metadata.
- Tests and validation: PASS. Quickstart and contracts define review checks and future validation gates for objective metrics, listening tests, performance, and safety.

## Complexity Tracking

No constitution violations are introduced in this planning phase.
