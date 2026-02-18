# Architecture Blueprint

## Goals

- Keep audio processing deterministic and real-time safe.
- Isolate UI, AI, and hosting concerns from the audio callback.
- Preserve strict module boundaries to avoid cross-layer coupling.

## System Boundaries

- `Plugin`: JUCE entry points (`AudioProcessor`, `AudioProcessorEditor`), orchestration only.
- `Core`: parameter state, snapshots, interpolation, morph processors.
- `Host`: hosted plugin lifecycle, parameter discovery, parameter bridge.
- `AI`: MCP transport + tool adapter layer.
- `UI`: visual controls, visualization, and interaction logic.
- `Preset`: state serialization/versioning.
- `MIDI`: routing, learn, and mapping.

## Thread Model

- Audio thread:
  - Reads atomics and lock-free queues only.
  - No heap allocation, no locks, no blocking I/O.
- Message/UI thread:
  - Owns user interaction and painting.
  - Can perform non-real-time work.
- AI/network thread:
  - Owns MCP sockets and JSON handling.
  - Communicates via lock-free queues and command buffers.

## Data Flow

1. UI/MIDI/AI issues high-level morph commands.
2. Commands are normalized into thread-safe control state.
3. Audio thread interpolates current morph position to target parameter values.
4. Parameter bridge writes values to hosted plugin parameters.
5. Analysis events are sent back to UI/AI through lock-free event channels.

## Contracts

- `Core` must not depend on JUCE UI types.
- `AI` must never call into audio thread code directly.
- `Host` interaction with `Core` occurs through typed DTOs (parameter IDs, normalized values).
- `Preset` serialization format must be versioned and backward-compatible.

## Non-Functional Enforcement

- Add allocation guards around `processBlock` in debug/perf builds.
- Assert thread affinity in entry points (audio vs message thread).
- Bound queue capacities and drop with metrics when saturated.

## Epic Ticket Mapping

- `MORPH-010` Core plugin architecture
- `MORPH-016` Plugin host manager
- `MORPH-017` Parameter bridge
- `MORPH-020` MIDI router
- `MORPH-021` Preset system

## Milestone Exit Criteria

- Module boundaries are enforced by includes/dependencies and ownership rules.
- Audio/UI/AI thread contracts are validated through integration tests.
- State format versioning and load validation are implemented and documented.
