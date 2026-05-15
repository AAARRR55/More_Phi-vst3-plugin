# More-Phi V2 Implementation — Agent Team Design

**Date:** 2026-02-26
**Approach:** Module-Ownership (7 parallel agents, zero file conflicts)
**Source Spec:** `docs/MORE_PHI_V2_SPEC.md` (1512 lines)

## Agent Roster

| # | Agent | Type | Files Owned | Depends On |
|---|-------|------|-------------|------------|
| 1 | Interfaces & Types | `coder` | `Core/I*.h`, `Core/ModulationTypes.h`, `Core/SpectralTypes.h` | None |
| 2 | Modulation Engine | `coder` | `Core/ModulationMatrix.*`, `Core/LFO.*`, `Core/EnvelopeFollower.*`, `Core/StepSequencer.*`, `Core/MacroKnob.h`, `Core/ModulationEngine.*` | #1 (interfaces) |
| 3 | Spectral DSP | `coder` | `Core/SpectralMorphEngine.*`, `Core/TransientDetector.*`, `Core/PhaseVocoder.*`, `Core/FormantMorphEngine.*` | #1 |
| 4 | Granular DSP | `coder` | `Core/GranularMorphEngine.*`, `Core/GrainPool.*`, `Core/HybridBlend.*` | #1 |
| 5 | Preset Library | `coder` | `Preset/PresetLibrary.*`, `Preset/PresetEntry.h`, `Preset/CloudSyncClient.*`, `Preset/PresetSearch.*` | None |
| 6 | Integration | `coder` | Modifies `Plugin/PluginProcessor.h/cpp`, creates `Core/VAEMorphEngine.*` | #1-5 ideally, but can scaffold stubs |
| 7 | Test Suite | `tester` | `tests/Unit/TestModulation.cpp`, `tests/Unit/TestSpectral.cpp`, `tests/Unit/TestGranular.cpp`, `tests/Unit/TestPresetLibrary.cpp`, `tests/Mocks/MockV2Interfaces.h` | #1-5 |

## Conventions

- Namespace: `morephi`
- Audio thread: `noexcept`, zero alloc after `prepare()`, no locks
- Headers: `#pragma once`, JUCE-style formatting
- Interfaces: pure virtual, `I` prefix, `virtual ~I() = default`
- Tests: Catch2 v3, `TEST_CASE` / `SECTION` style
- Memory: `std::array` for fixed buffers, `std::vector` resized only in `prepare()`

## File Conflict Prevention

Each agent creates files in non-overlapping paths. Only agent #6 (Integration) modifies existing files. All others create new files exclusively.
