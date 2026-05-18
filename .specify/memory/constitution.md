<!--
Sync Impact Report
Version change: unresolved template -> 1.0.0
Modified principles:
- PRINCIPLE_1_NAME -> I. Real-Time Audio Safety Is Non-Negotiable
- PRINCIPLE_2_NAME -> II. Thread Domains and Ownership Stay Explicit
- PRINCIPLE_3_NAME -> III. Parameter Semantics and Host Safety Are Preserved
- PRINCIPLE_4_NAME -> IV. Testable, Deterministic, and Reproducible Changes
- PRINCIPLE_5_NAME -> V. Honest AI, MCP, and Dataset Boundaries
Added sections:
- Technical Constraints
- Development Workflow and Quality Gates
Removed sections:
- Placeholder Section 2
- Placeholder Section 3
Templates requiring updates:
- .specify/templates/plan-template.md: updated
- .specify/templates/spec-template.md: updated
- .specify/templates/tasks-template.md: updated
- .specify/templates/commands/*.md: not present
Follow-up TODOs: None
-->

# More-Phi Constitution

## Core Principles

### I. Real-Time Audio Safety Is Non-Negotiable

Any code reachable from `MorePhiProcessor::processBlock()` MUST be deterministic,
bounded, and real-time safe. Audio-thread code MUST NOT allocate after prepare,
perform blocking I/O, wait on locks, perform network work, run model inference, or
throw exceptions. Any unavoidable try-lock path MUST have a bounded skip or
fallback behavior that preserves audio continuity. New audio-thread buffers MUST
be preallocated during prepare or model/plugin load, and runtime resizing in the
audio callback is prohibited.

Rationale: More-Phi is a DAW plugin. Glitches, deadlocks, and unbounded latency
are product failures even when functional behavior is otherwise correct.

### II. Thread Domains and Ownership Stay Explicit

Features MUST respect the established audio, message, MCP, and background-worker
domains. Cross-thread communication MUST use approved primitives such as the
parameter command queue, atomics, seqlock-protected reads, double-buffered state,
or message-thread deferral. Shared mutable state MUST document its writer,
reader, memory-ordering assumptions, and fallback behavior. `MorePhiProcessor`
remains the subsystem owner; new global singletons are prohibited unless a spec
justifies why owned lifetime is impossible.

Rationale: The plugin hosts third-party code and exposes UI, MIDI, MCP, dataset,
and DSP paths. Clear ownership and thread boundaries are required to keep those
paths safe together.

### III. Parameter Semantics and Host Safety Are Preserved

All hosted-plugin parameter work MUST preserve normalized `[0, 1]` host values,
stable parameter identity, parameter count limits, and parameter type semantics.
Continuous, frequency, decibel, discrete, binary, and enumeration controls MUST
be handled according to their classifications; discrete-like controls MUST not be
interpolated through invalid intermediate values. Dangerous or high-risk controls
such as volume, pitch, bypass, and safety-limited semantic controls MUST respect
configured sanity ranges. External automation from UI, MIDI, MCP, AI, or dataset
tools MUST route through the existing command, bridge, or semantic planning
interfaces rather than writing hosted-plugin state ad hoc.

Rationale: More-Phi's value is expressive morphing without corrupting hosted
plugin state or producing unsafe parameter jumps.

### IV. Testable, Deterministic, and Reproducible Changes

Changes affecting Core, Host, MIDI, Preset, MCP/AI contracts, dataset generation,
state restoration, or audio-thread behavior MUST include appropriate automated
tests or a documented reason tests are not possible. Unit tests MUST cover pure
DSP and data-structure behavior. Integration tests MUST cover plugin lifecycle,
state persistence, MCP contracts, and cross-thread command paths when changed.
Dataset and ML-related changes MUST use deterministic seeds, versioned schemas,
and leakage-resistant train/validation/test splits. Performance-sensitive changes
MUST include a bounded validation strategy appropriate to the affected path.

Rationale: Audio plugins fail in subtle host, timing, and persistence scenarios;
repeatable tests and deterministic datasets are the only scalable guardrail.

### V. Honest AI, MCP, and Dataset Boundaries

AI-facing features MUST describe what is deterministic, heuristic, or learned,
and MUST NOT present unavailable model inference as calibrated intelligence.
MCP tools MUST keep localhost authentication, explicit JSON-RPC contracts,
bounded parameter edits, and auditable method metadata. Neural inference, token
optimization, dataset rendering, scanning, and long-running analysis MUST stay
off the audio thread. Synthetic dataset samples MUST preserve source provenance,
plugin/version context, parameter schemas, validation flags, and split metadata.

Rationale: AI automation can change sound and host state quickly. Trust depends
on safe execution, clear claims, and reproducible training data.

## Technical Constraints

More-Phi is a C++20 JUCE 8 plugin built with CMake. Source code MUST remain in
the `more_phi` namespace and follow the existing layered layout: Plugin owns
entry points, Core owns real-time computation, Host owns plugin lifecycle and
parameter I/O, AI owns MCP and assistant behavior, MIDI owns note/CC routing,
Preset owns persistence, and UI owns message-thread components.

Audio-thread code MUST prefer fixed-size storage, preallocated vectors, atomics,
lock-free queues, seqlock reads, and double-buffer publishing. Message-thread and
background code MAY allocate and perform I/O, but MUST not block the audio path
or hold hosted-plugin state in a way that can deadlock `processBlock()`.

State persistence changes MUST account for APVTS parameters, snapshot data,
hosted-plugin descriptions, opaque hosted state chunks, modulation routes, and
deferred plugin reload constraints. Any persisted schema change MUST include a
migration or a documented compatibility decision.

ML integration MUST target CPU-safe, optional backends such as ONNX Runtime and
MUST provide deterministic fallback behavior when a model is missing. Model
outputs MUST be compact control data, parameter vectors, masks, embeddings, or
plans; audio generation models are out of scope unless a spec proves real-time
and product safety.

## Development Workflow and Quality Gates

Every feature specification MUST define independently testable user stories,
measurable success criteria, edge cases, and relevant assumptions. Every plan
MUST pass the Constitution Check before research and again after design.

The Constitution Check MUST verify real-time safety, thread-domain isolation,
parameter semantics, AI/MCP claim boundaries, persistence impact, and the test or
validation strategy. Any violation MUST be documented in the plan's Complexity
Tracking table with the rejected simpler alternative.

Tasks MUST be organized by user story and include exact file paths. Tests are
mandatory for changes to audio-thread code, Core algorithms, Host lifecycle,
state persistence, MCP contracts, semantic parameter safety, dataset schemas, and
build or packaging behavior. Documentation-only or UI-only changes MAY omit
tests when the plan states why automated validation is not useful.

Before release or handoff, the relevant CMake build and Catch2 test commands
MUST pass for the affected configuration, or the final report MUST state the
exact command not run, why it was skipped, and the residual risk.

## Governance

This constitution supersedes conflicting project practices, generated plans,
task lists, and ad hoc implementation preferences. Runtime instructions in
`AGENTS.md` remain operational guidance, but feature specs and implementation
plans MUST satisfy this constitution.

Amendments MUST update this file, include a Sync Impact Report, state the
semantic version change, and review dependent Spec Kit templates. Major version
changes are required for removed or incompatible principles. Minor version
changes are required for new principles or materially expanded governance. Patch
version changes are reserved for clarifications that do not change obligations.

Compliance is enforced during `/speckit.plan` through the Constitution Check,
during `/speckit.tasks` through task coverage, and during implementation review
through tests, performance validation, and explicit risk reporting. Exceptions
are permitted only when documented with rationale, scope, risk, and follow-up in
the relevant plan.

**Version**: 1.0.0 | **Ratified**: 2026-05-18 | **Last Amended**: 2026-05-18
