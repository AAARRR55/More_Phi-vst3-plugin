# Validation Gates Contract: Neural Mastering Implementation Roadmap

**Purpose**: Defines the gate vocabulary and pass/fail expectations used by implementation tasks, tests, scripts, reviews, and release decisions.

## Gate Rules

- Every gate result MUST include a stable gate ID, status, evidence level, measurement method, threshold, owner role, and decision rule.
- Gates without measured evidence MUST be marked `planning`, `research-estimate`, or `not-measured` and MUST NOT be used for production quality claims.
- Runtime gates G04-G09 MUST pass before any optional neural runtime behavior can advance beyond deterministic fallback or review-only output.
- G10 MUST pass before any model training/evaluation output supports professional quality claims.
- Failure of a high-severity runtime, artifact, fallback, or data-governance gate results in no-go for production enablement.

## Gate Catalog

| Gate | Area | Measurement method | Threshold | Current evidence level | Decision rule |
|------|------|--------------------|-----------|------------------------|---------------|
| G01 | Identity transparency | Identity/already-mastered set with loudness-matched objective analysis | Median loudness drift under 0.25 LU and true-peak increase under 0.3 dB | Future prototype-measured | No-go for runtime if exceeded |
| G02 | Composite objective quality | Spectral, phase, dynamics, stereo, identity, and artifact validation score | Student reaches at least 95% of teacher composite validation score | Future prototype-measured | No-go for student deployment below threshold |
| G03 | Blind/expert listening | Level-matched blind expert A/B/X or panel review | At least 80% reviewed excerpts have no blocker rating; no artifact class recurs in more than 10% | Future listening evidence | No-go if unresolved blockers recur |
| G04 | Latency boundary | Worst-case runtime measurement under smallest supported buffer and sample-rate targets | No neural work in audio callback; future permitted runtime path must show explicit deadline headroom | Planning until benchmarked | No-go for audio-callback inference without measured pass |
| G05 | CPU headroom | DAW-like stress benchmark with background contention | Background/control-plane inference stays below 5% of one performance core and causes zero dropouts | Future benchmark | No-go if overload affects audio continuity |
| G06 | Invalid output rejection | Fuzz invalid outputs, missing artifacts, version mismatch, NaN/Inf, out-of-range controls | 100% invalid cases rejected without audio interruption | Planning until automated test | No-go if invalid output propagates |
| G07 | Bypass and transition safety | Automated and listening checks for active/bypass, wet/dry, plan changes, and latency compensation | Zero discontinuities above release threshold; no unannounced latency change; no parameter jump beyond max-delta limits | Future automated/listening test | No-go if transitions are audible or unsafe |
| G08 | Artifact rate | Alias, IMD, pumping, transient smear, pre-ringing, mono fold-down, overs, and HF residual tests | Zero true-peak overs; no high-severity recurring artifact; artifact classes below release-defined tolerance | Future prototype-measured | No-go if high-severity artifacts remain |
| G09 | Fallback behavior | Missing, late, invalid, overloaded, and low-confidence model scenarios | Hold last safe controls, deterministic baseline, review-only, or transparent bypass without blocking | Planning until automated/integration test | No-go if fallback blocks, underruns, clicks, or interrupts audio |
| G10 | Data governance | Dataset audit for provenance, license, split isolation, and reference quality | 100% training/evaluation material has acceptable provenance and no known split leakage | Planning until governance review | No-go if provenance or leakage is unresolved |

## Status Vocabulary

- `pass`: Gate has sufficient evidence and met threshold.
- `fail`: Gate has evidence and did not meet threshold.
- `not-measured`: Gate is defined but evidence has not been collected.
- `not-applicable`: Gate does not apply to the current phase or artifact.

## Decision Vocabulary

- `proceed`: Phase may continue.
- `review-only`: Output may be shown but not applied.
- `fallback-only`: Deterministic baseline or last-safe hold must be used.
- `no-go`: Runtime or release advancement is blocked.
- `release-approved`: All applicable release gates have measured-pass evidence and owner signoff.
