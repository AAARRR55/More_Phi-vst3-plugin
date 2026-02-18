# Morphing Engine Blueprint

## Responsibilities

- Manage snapshot capture and retrieval.
- Compute morph interpolation over up to 12 snapshots.
- Provide multiple morph modes (XY, Fader, Elastic, Drift).
- Output smoothed, normalized parameter values.

## Core Types

- `ParameterValue`: `{ parameterId, normalizedValue }`
- `Snapshot`: fixed-size or sparse set of parameter values.
- `SnapshotBank`: indexed storage for 12 slots + metadata.
- `MorphPosition`: `{ x, y, phase, mode }`
- `MorphFrame`: resolved parameter values for current audio block.

## Processing Pipeline

1. Read current morph control state (`x`, `y`, mode params).
2. Resolve active snapshot set and blend weights.
3. Interpolate parameter values with selected curve.
4. Apply smoothing/dezippering to avoid stepping noise.
5. Emit final parameter frame for bridge write.

## Mode Rules

- XY mode:
  - Uses 2D weighted blend from nearest logical region.
- Fader mode:
  - Traverses ordered snapshots with fractional index interpolation.
- Elastic mode:
  - Spring-mass-damper integration with bounded velocity.
- Drift mode:
  - Low-frequency stochastic/autonomous movement with clamp.

## Safety Rules

- Clamp all values to `[0.0, 1.0]` before output.
- Ignore protected parameter IDs (bypass, mute, master volume if configured).
- Handle missing parameters by fallback to last-known valid value.

## Performance Targets

- O(P) per block where P = active morphed parameter count.
- No per-block allocation.
- Optional SIMD path guarded by compile-time flags.

## Extension Points

- Add new interpolation curve by extending curve registry.
- Add new mode by implementing `IMorphMode::advance()` and weight resolver.
- Add parameter policies (log taper, stepped, quantized) via per-parameter adapters.

## Epic Ticket Mapping

- `MORPH-011` Parameter state management
- `MORPH-012` Snapshot bank implementation
- `MORPH-013` Interpolation engine
- `MORPH-014` Physics engine (Elastic mode)
- `MORPH-015` Genetic engine (breeding-related morph features)

## Milestone Exit Criteria

- 12-slot snapshot morphing behavior is stable and deterministic.
- XY/Fader/Elastic/Drift modes pass unit and integration targets.
- Audio-thread execution remains allocation-free under stress.
