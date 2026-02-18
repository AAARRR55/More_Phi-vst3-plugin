# Testing Blueprint

## Test Strategy

- Unit tests for deterministic core math and state logic.
- Integration tests for module boundaries and thread handoff.
- Performance tests for CPU, allocations, and latency targets.
- Manual DAW compatibility matrix before release candidates.

## Unit Coverage Targets

- `SnapshotBank`: 95%+
- `InterpolationEngine`: 95%+
- `MorphMode` implementations: 90%+
- `PresetSerializer`: 90%+
- `MCP request validation`: 85%+

## Integration Scenarios

- Snapshot capture -> morph output correctness.
- UI gesture -> audio-thread parameter application.
- MCP tool call -> internal command -> state update.
- Save/reload plugin state -> identical reconstruction.
- Hosted plugin parameter refresh during session.

## Real-Time Validation

- Assert zero allocations in `processBlock` during stress tests.
- Validate no mutex contention on audio callback path.
- Run variable sample rate/buffer size sweep:
  - 44.1k, 48k, 96k, 192k
  - 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192

## Performance Gates

- CPU: < 2% target baseline (48 kHz / 256 samples, representative project).
- UI: 60 FPS median with active interaction.
- MCP latency: < 100 ms round-trip for standard tool calls.
- Plugin load: < 500 ms on supported target machines.

## CI Recommendations

- Build matrix: Windows/macOS/Linux.
- Run unit + integration suites on each PR.
- Publish coverage report and perf trend artifacts.
- Block merge on failing real-time safety checks.

## Release Validation Checklist

- DAW matrix pass: FL Studio, Ableton Live, Logic Pro, Cubase, Reaper.
- Preset backward compatibility check with previous versions.
- MCP auth and malformed-request hardening checks.
- Smoke test for fresh install + first launch flow.

## Epic Ticket Mapping

- `MORPH-028` DAW compatibility testing
- `MORPH-029` Performance optimization
- `MORPH-030` Documentation and test evidence packaging

## Milestone Gate Criteria

- `MORPH-028` closes only after the full DAW matrix passes with documented results.
- `MORPH-029` closes only after CPU/load/latency targets are met on baseline rigs.
- `MORPH-030` closes only after test reports, architecture docs, and user-facing docs are synchronized.
