# Morph Plugin Blueprint

This package contains implementation-ready planning docs for the Morphy plugin.

## Structure

- `architecture/ARCHITECTURE_BLUEPRINT.md`: system boundaries, thread model, module contracts
- `morphing-engine/MORPHING_ENGINE_BLUEPRINT.md`: interpolation, snapshot math, morph modes
- `cpp-implementation/CPP_IMPLEMENTATION_BLUEPRINT.md`: C++ standards, folder layout, coding patterns
- `mcp-integration/MCP_INTEGRATION_BLUEPRINT.md`: MCP protocol, tool contracts, safety model
- `ui-design/UI_DESIGN_BLUEPRINT.md`: component hierarchy, interaction model, rendering strategy
- `testing/TESTING_BLUEPRINT.md`: unit/integration/perf plan, gates, CI matrix

## How To Use

1. Start with `architecture/ARCHITECTURE_BLUEPRINT.md` to align boundaries.
2. Implement domain logic via `morphing-engine/` and `cpp-implementation/`.
3. Add AI features with `mcp-integration/`.
4. Build UI from `ui-design/`.
5. Validate with `testing/` before merge/release.

## Baseline Targets

- Real-time safety: zero allocations in audio callback
- CPU target: < 2% at 48 kHz / 256 samples (typical DAW project)
- UI target: 60 FPS on supported desktop platforms
- MCP round-trip target: < 100 ms for non-streaming commands

## Epic Alignment (MORPH-001)

Reference date: 2026-02-18.

- Completed foundation/core work is tracked across `MORPH-010` to `MORPH-027` (with `MORPH-024` partial).
- Remaining milestone work is tracked in:
  - `MORPH-028` DAW compatibility testing
  - `MORPH-029` performance optimization
  - `MORPH-030` documentation completion

## Phase Mapping

- Phase 1 Foundation (Complete): `MORPH-010`, `MORPH-011`, `MORPH-012`, `MORPH-013`
- Phase 2 Core Features (Complete): `MORPH-014`, `MORPH-015`, `MORPH-016`, `MORPH-017`
- Phase 3 AI Integration (Complete): `MORPH-018`, `MORPH-019`, `MORPH-024`
- Phase 4 UI Implementation (Complete): `MORPH-022`, `MORPH-023`, `MORPH-025`, `MORPH-026`, `MORPH-027`
- Phase 5 Testing and Polish (In Progress): `MORPH-028`, `MORPH-029`, `MORPH-030`
- Phase 6 Release (Pending): beta, packaging, and release hardening
