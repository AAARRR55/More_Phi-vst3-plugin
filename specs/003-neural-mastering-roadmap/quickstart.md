# Quickstart: Neural Mastering Implementation Roadmap

**Feature directory**: `specs/003-neural-mastering-roadmap`  
**Plan**: `specs/003-neural-mastering-roadmap/plan.md`

## Purpose

Use this quickstart to review the implementation plan before generating tasks or writing code. The goal is to confirm that neural mastering work starts with deterministic safety, testable contracts, and fallback behavior rather than unproven model inference.

## Review Order

1. Read [spec.md](./spec.md) to confirm the stakeholder requirements and success criteria.
2. Read [implementation-strategy.md](./implementation-strategy.md) to understand the technical architecture, targeted file manifest, phased roadmap, and dependency mapping.
3. Read [plan.md](./plan.md) to confirm technical context, constitution gates, project structure, and design outputs.
4. Read [research.md](./research.md) to confirm all major implementation choices are resolved.
5. Read [data-model.md](./data-model.md) to confirm entity shapes and state transitions.
6. Review contracts:
   - [neural-mastering-plan.schema.json](./contracts/neural-mastering-plan.schema.json)
   - [model-card.schema.json](./contracts/model-card.schema.json)
   - [validation-gates.md](./contracts/validation-gates.md)

## Planning Checks

Confirm these statements are true before `/speckit-tasks`:

- The first coding phase is core types and deterministic Safety Layer projection.
- Optional model runner work is blocked until safety, feature-frame, application, and dataset-governance phases are defined.
- Direct neural inference inside `processBlock()` remains rejected.
- Missing or invalid model artifacts resolve to deterministic fallback or review-only output.
- Plan candidates contain bounded targets, deltas, masks, confidence, evidence level, and gate results rather than audio buffers.
- Dataset governance includes provenance, license status, split isolation, and reference-quality review.
- UI/MCP work is status-first and cannot bypass the Safety Layer.
- No professional quality claim is made without objective, listening, runtime, fallback, and governance evidence.

## Suggested First Task Slice

If implementation proceeds, start with the smallest safe slice:

1. Add fixed-size `NeuralMasteringTypes` definitions.
2. Add `NeuralMasteringSafetyPolicy` with schema, finite, range, mask, max-delta, staleness, confidence, and fallback checks.
3. Add `TestNeuralMasteringSafetyPolicy.cpp` with malformed candidate fixtures.
4. Register the test file in `tests/CMakeLists.txt`.
5. Run the targeted Catch2/CTest command for the new safety tests.

Do not add an inference backend in the first task slice.

## Validation Commands

Use the repository's normal build/test path after task implementation. Typical Windows-safe commands are:

```bash
cmake --preset windows-msvc-safe
cmake --build --preset windows-safe --parallel 2
```

For test-enabled validation, use the release/test preset path when appropriate:

```bash
cmake --preset windows-msvc-release
cmake --build build/windows-msvc-release --config Release --parallel 2
ctest --preset windows-tests
```

For focused test runs after adding neural mastering safety tests:

```bash
ctest --test-dir build/windows-msvc-release --build-config Release -R "NeuralMasteringSafety" --output-on-failure
```

Validation outcome for the MVP safety slice on 2026-05-18:

```bash
cmake -B build/neural-mastering-validation -S . -G "Visual Studio 17 2022" -A x64 -T host=x64 -DMORE_PHI_BUILD_TESTS=ON -DMORE_PHI_COPY_PLUGIN_AFTER_BUILD=OFF -DMORE_PHI_SAFE_BUILD_MODE=ON -DMORE_PHI_MSVC_MP=2 -DMORE_PHI_ENABLE_LTO=OFF
cmake --build build/neural-mastering-validation --config Release --target MorePhiTests --parallel 2
ctest --test-dir build/neural-mastering-validation --build-config Release -R "NeuralMasteringSafety" --output-on-failure
```

Result: Configure succeeded, `MorePhiTests` built successfully, and all 3 `NeuralMasteringSafety` CTest cases passed. The documented `build/windows-msvc-release` preset path was not reused because that existing build tree had a CMake toolset-cache mismatch; no cache directories were deleted during validation.

Additional validation outcome for US2/US3 scaffolding and signal-safety coverage on 2026-05-18:

```bash
cmake --build build/neural-mastering-validation --config Release --target MorePhiTests --parallel 2
ctest --test-dir build/neural-mastering-validation --build-config Release -R "NeuralMastering|VST3 audio signal accuracy|AutoMasteringEngine applies only validated" --output-on-failure
build/neural-mastering-validation/tests/Release/MorePhiTests.exe "VST3 neural mastering plan transitions keep signal bounded and mono-compatible"
build/neural-mastering-validation/tests/Release/MorePhiTests.exe "MorePhiProcessor processBlock does not invoke neural mastering planner or model"
cmake --preset windows-msvc-release
```

Result: `MorePhiTests` rebuilt successfully; the focused CTest command passed 16/16 matching cases; the direct transition and `processBlock()` guard tests passed. The `windows-msvc-release` configure preset still failed because the existing `build/windows-msvc-release` tree has a generator toolset cache mismatch (`host=x64` versus the previous cache); the cache was not deleted or overwritten.

## Stop Conditions

Stop and revisit the plan if any proposed task:

- Adds model inference, file I/O, lock waiting, network work, allocation, exceptions, or dynamic latency mutation to the audio callback.
- Allows a plan candidate to bypass the Safety Layer.
- Applies low-confidence, stale, unsupported-layout, NaN/Inf, out-of-range, or illegal-mask output.
- Requires a model artifact for normal plugin operation.
- Claims measured model quality, CPU headroom, latency, or listening preference without corresponding gate evidence.
- Persists model state into plugin projects before compatibility and fallback behavior are approved.

## Implementation Validation Notes

2026-05-18:

- `cmake --preset windows-msvc-release` was attempted and stopped at configure because the existing preset build directory has a conflicting CMake toolset cache.
- Existing validation build tree used: `build/neural-mastering-validation`.
- Focused Catch2 command passed: `build\neural-mastering-validation\tests\Release\MorePhiTests.exe "[NeuralMasteringSafety],[NeuralMasteringDataset],[NeuralMasteringController]" --reporter compact` (2435 assertions in 12 test cases).
- Focused CTest command passed: `ctest --test-dir build\neural-mastering-validation --build-config Release -R "NeuralMasteringSafety" --output-on-failure` (3/3 tests).
- Broader CTest command passed: `ctest --test-dir build\neural-mastering-validation --build-config Release -R "NeuralMastering" --output-on-failure` (8/8 tests).
- Python script syntax check passed: `python -m py_compile scripts\neural-mastering\audit_dataset.py scripts\neural-mastering\extract_features.py scripts\neural-mastering\generate_model_card.py scripts\neural-mastering\run_objective_metrics.py scripts\neural-mastering\benchmark_planner.py scripts\neural-mastering\validate_model_artifact.py`.
- `src/Plugin/PluginProcessor.cpp` diff was checked: neural mastering wiring is limited to constructor/prepare/release lifecycle, with no neural inference, model loading, file I/O, allocation, exceptions, network work, or new neural path in `processBlock()`.
