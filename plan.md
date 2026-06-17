# More-Phi Comprehensive Technical Review Plan

## Objective
Conduct a multi-agent technical review and refactor assessment of the More-Phi VST3 plugin (v3.3.0) and produce a unified production-readiness report.

## Stage 1 — Parallel Domain Audits (6 Sub-Agents)
All sub-agents run in parallel. Each reads their assigned source files and writes a structured markdown report to `test_reports/subagent_{N}_report.md`.

### Sub-Agent 1: Plugin Hosting & Memory Safety Auditor
**Files:** `src/Host/PluginHostManager.h/cpp`, `src/Host/ParameterBridge.h/cpp`, `src/Host/IPluginHostManager.h`, `src/Host/PluginScanner.h/cpp`, `src/Plugin/PluginProcessor.h/cpp`, `src/Plugin/PluginEditor.h/cpp`, `src/Core/LockFreeQueue.h`, `src/Core/AudioBufferPool.h/cpp`
**Focus:** VST3 lifecycle, memory leaks, thread safety, RAII, parameter handling, state serialization.

### Sub-Agent 2: Snapshot System & State Management Specialist
**Files:** `src/Core/SnapshotBank.h/cpp`, `src/Core/ParameterState.h`, `src/Preset/MetaPresetManager.h/cpp`, `src/Preset/PresetSerializer.h/cpp`, `src/Preset/PresetSerializerV2.h/cpp`, `src/Preset/PresetLibrary.h/cpp`, `src/UI/SnapshotRing.h/cpp`
**Focus:** 12-slot snapshot integrity, state persistence, edge cases, corruption recovery, seqlock safety.

### Sub-Agent 3: Physics Engine & Interpolation Validator
**Files:** `src/Core/InterpolationEngine.h/cpp`, `src/Core/PhysicsEngine.h/cpp`, `src/Core/MorphProcessor.h/cpp`, `src/Core/ModulationEngine.h/cpp`, `src/Core/LFO.h/cpp`, `src/Core/EnvelopeFollower.h/cpp`, `src/UI/MorphPad.h/cpp`, `src/UI/SnapFader.h/cpp`
**Focus:** Direct/Elastic/Drift physics correctness, interpolation stability, edge cases, numerical precision.

### Sub-Agent 4: MCP Server & Communication Protocol Auditor
**Files:** `src/AI/MCPServer.h/cpp`, `src/AI/MCPToolHandler.h/cpp`, `src/AI/MCPToolsExtended.h/cpp`, `src/AI/StandaloneMcp/StandaloneMcpServer.h/cpp`, `src/AI/StandaloneMcp/StandaloneMcpMain.cpp`, `src/AI/InstanceRegistry.h/cpp`, `src/AI/TokenOptimizer.h/cpp`
**Focus:** JSON-RPC compliance, auth tokens, port security, error handling, serialization performance.

### Sub-Agent 5: Audio I/O, MIDI & Features Validator
**Files:** `src/MIDI/MIDIRouter.h/cpp`, `src/Core/GeneticEngine.h/cpp`, `src/Core/DiscreteParameterHandler.h/cpp`, `src/Core/ParameterClassifier.h/cpp`, `src/UI/MacroKnobStrip.h/cpp`, `src/UI/BreedingPanel.h/cpp`, `src/Plugin/PluginProcessor.h/cpp` (audio I/O aspects)
**Focus:** MIDI routing, genetic breeding correctness, discrete parameter snapping, macro knob mapping, audio channel layouts.

### Sub-Agent 6: Production Readiness & Code Quality Reviewer
**Files:** `CMakeLists.txt`, `tests/CMakeLists.txt`, `src/Version.h`, `src/Core/PerformanceProfiler.h/cpp`, `src/Core/AllocationTracker.h`, `src/Core/ThreadPool.h/cpp`, `src/Core/LockFreeQueue.h`, `.clang-tidy`, `tests/` directory, `src/Plugin/PluginProcessor.cpp` (overall quality)
**Focus:** Build configuration, test coverage, performance profiling, warning-free compilation, code style consistency, DAW compatibility assumptions.

## Stage 2 — Synthesis (Lead Architect)
Read all 6 sub-agent reports, identify systemic issues spanning multiple domains, prioritize findings, and produce the unified final report: `test_reports/FINAL_TECHNICAL_REVIEW.md`.

## Deliverable Format
Each sub-agent report must contain:
- **Critical Issues** (bugs, crashes, memory leaks)
- **High-Priority Improvements** (architectural, performance, thread-safety)
- **Medium-Priority Refinements** (edge cases, precision)
- **Low-Priority Enhancements** (documentation, style)
- **Recommended Fixes** (with rationale and suggested code changes for critical items)

The final unified report adds:
- **Systemic Risk Assessment** (cross-domain issues)
- **Production-Readiness Verdict** (blockers and readiness status)
