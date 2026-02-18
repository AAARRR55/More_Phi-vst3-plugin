# C++ Implementation Blueprint

## Toolchain

- C++20 (or project-standard C++ version if constrained).
- JUCE 8.x baseline.
- CMake as single source of build configuration.

## Source Layout

- `src/Plugin`: plugin entry and lifecycle.
- `src/Core`: deterministic domain logic.
- `src/Host`: hosted plugin management.
- `src/AI`: MCP and AI command adapters.
- `src/UI`: component tree and rendering.
- `src/Preset`: serialization and migrations.
- `src/MIDI`: controller mapping and learn.

## Coding Standards

- Prefer value types and explicit ownership.
- Use `std::unique_ptr` for owned heap objects.
- Use `std::atomic` for trivial cross-thread state.
- No exceptions in audio callback path.
- Keep headers minimal; avoid transitive include bloat.

## Real-Time Safe Patterns

- Preallocate fixed buffers in `prepareToPlay`.
- Replace dynamic containers in callback with fixed-capacity structures.
- Use SPSC lock-free queues for event transfer.
- Move JSON/network/file operations to non-audio threads only.

## API Design

- Public interfaces should be small and testable.
- Prefer pure functions for interpolation/math logic.
- Use strongly typed IDs for parameters/snapshots.
- Return explicit status (`enum class ErrorCode`) for recoverable failures.

## Error Handling

- Audio path: fail soft, log minimal counter-based diagnostics.
- UI/AI path: structured errors with user-visible status where needed.
- Persist layer: version checks and migration fallback.

## Definition of Done (Code)

- Unit tests for new logic.
- Integration coverage for thread and module boundaries.
- Profiling pass shows no callback allocations.
- Static analysis warnings are addressed or documented.

## Epic Ticket Mapping

- `MORPH-010` Core plugin architecture
- `MORPH-011` Parameter state management
- `MORPH-016` Plugin host manager
- `MORPH-017` Parameter bridge
- `MORPH-021` Preset system
- `MORPH-025` Plugin browser panel plumbing

## Milestone Checkpoints

- Phase 1 and 2 code paths are complete and regression-safe.
- Remaining implementation focus supports `MORPH-028` and `MORPH-029` performance/compatibility outcomes.
- Documentation annotations in code support `MORPH-030` completion.
