# Epic Planning Document: MorphSnap AI-Powered Parameter Morphing Plugin

**Epic Key:** MORPH-001
**Epic Title:** AI-Powered VST3/AU Parameter Morphing Engine
**Status:** In Progress
**Created:** 2026-02-18
**Project:** D:/morphy

---

## 1. Epic Overview

### Purpose

MorphSnap (Morphy) is a high-performance VST3/AU audio plugin that functions as an advanced parameter morphing engine. It enables users to capture, interpolate, and crossfade between the parameter states of other hosted plugins in real-time, with groundbreaking MCP (Model Context Protocol) integration for AI-driven automation.

### User Value

- **Producers/Sound Designers**: Create evolving, dynamic sounds through parameter morphing
- **Mix Engineers**: Automate complex transitions between mix states
- **Live Performers**: Real-time control over multiple plugin parameters via a single XY pad
- **AI-Assisted Workflows**: Leverage LLM intelligence for creative parameter suggestions and automation

### Scope

| In Scope | Out of Scope |
|----------|--------------|
| VST3 plugin format (Windows/macOS/Linux) | VST2 format (deprecated) |
| AU plugin format (macOS) | AAX format (Pro Tools) |
| Plugin hosting for parameter capture | Audio effect processing |
| MCP client/server for AI connectivity | Built-in AI model inference |
| 12-snapshot morphing system | Unlimited snapshots |
| XY pad, fader, elastic, drift morph modes | 3D morphing surface |
| Vector-based responsive UI | Skinnable user themes |

### Key Stakeholders

- **End Users**: Audio producers, sound designers, live performers
- **DAW Vendors**: FL Studio, Ableton, Logic Pro, Cubase compatibility
- **AI Service Providers**: OpenAI, Anthropic, local LLM backends

---

## 2. Epic Goals & Success Metrics

### Primary Goal

Deliver a production-ready, DAW-compatible VST3/AU plugin that enables real-time parameter morphing with AI-driven automation capabilities through MCP server integration.

### Success Metrics

| Metric | Target | Measurement Method |
|--------|--------|-------------------|
| CPU Usage (48kHz/256 samples) | < 2% | Profiling in DAW |
| Real-Time Safety | Zero audio thread allocations | Static analysis + runtime checks |
| DAW Compatibility | 5+ major DAWs | Manual testing matrix |
| MCP Connection Latency | < 100ms round-trip | Automated tests |
| Test Coverage | 90% branch coverage | lcov/codecov |
| UI Frame Rate | 60 FPS | Performance monitoring |
| Plugin Load Time | < 500ms | Startup profiling |

### KPIs

- Zero critical bugs in production release
- User adoption: 1000+ downloads in first month
- AI feature utilization: 30%+ of users connect to MCP

---

## 3. Requirements Summary

### Functional Requirements

| ID | Requirement | Priority |
|----|-------------|----------|
| FR-001 | Host up to 12 plugin parameter snapshots in a clock-like arrangement | High |
| FR-002 | Support XY pad morphing with bilinear interpolation | High |
| FR-003 | Support fader-based morphing through all snapshots | High |
| FR-004 | Implement elastic physics mode with spring-mass-damper dynamics | Medium |
| FR-005 | Implement drift mode with Perlin noise autonomous movement | Medium |
| FR-006 | Capture parameter states from hosted plugins | High |
| FR-007 | Provide MCP server for AI client connectivity | High |
| FR-008 | Support AI-driven parameter suggestions | Medium |
| FR-009 | Support AI-generated morph trajectories | Medium |
| FR-010 | Provide visual feedback (level meters, spectrum, waveform) | Medium |
| FR-011 | Support MIDI learn for remote control | Medium |
| FR-012 | State persistence across DAW sessions | High |
| FR-013 | Preset system for morph configurations | Medium |

### Non-Functional Requirements

| ID | Requirement | Category |
|----|-------------|----------|
| NFR-001 | Audio thread must never block or allocate memory | Performance |
| NFR-002 | UI must maintain 60 FPS during morphing | Performance |
| NFR-003 | Plugin must load within 500ms | Performance |
| NFR-004 | All cross-thread communication must be lock-free | Architecture |
| NFR-005 | Memory footprint must not exceed 100MB | Resource |
| NFR-006 | Support sample rates from 44.1kHz to 192kHz | Compatibility |
| NFR-007 | Support buffer sizes from 32 to 8192 samples | Compatibility |
| NFR-008 | Binary size must not exceed 20MB | Distribution |

### Business Rules

- Snapshots are immutable once captured unless explicitly updated
- AI suggestions require explicit user acceptance before application
- MCP connections require authentication token
- Protected parameters (bypass, mute, master volume) are excluded from morphing

### Edge Cases

- Hosted plugin parameter count changes mid-session
- MCP server disconnect during AI-driven automation
- Rapid morph position changes (anti-zipper filtering required)
- Snapshot slot overflow handling
- Invalid/missing hosted plugin references

---

## 4. Technical Change Overview

### Component Status Matrix

| Component | Type | Status | Risk | Dependencies |
|-----------|------|--------|------|--------------|
| `src/Plugin/PluginProcessor.*` | Enhancement | Implemented | Low | JUCE 8.x |
| `src/Plugin/PluginEditor.*` | Enhancement | Partial | Medium | UI Components |
| `src/Core/ParameterState.h` | Complete | Implemented | Low | None |
| `src/Core/SnapshotBank.*` | Complete | Implemented | Low | ParameterState |
| `src/Core/InterpolationEngine.*` | Complete | Implemented | Low | None |
| `src/Core/PhysicsEngine.*` | Complete | Implemented | Medium | None |
| `src/Core/GeneticEngine.*` | Enhancement | Implemented | Medium | None |
| `src/Core/MorphProcessor.*` | Complete | Implemented | Low | InterpolationEngine |
| `src/Host/PluginHostManager.*` | Critical | Implemented | High | JUCE Plugin Host |
| `src/Host/ParameterBridge.*` | Critical | Implemented | Medium | PluginHostManager |
| `src/AI/MCPServer.*` | Critical | Implemented | Medium | JUCE Networking |
| `src/AI/MCPToolHandler.*` | Enhancement | Implemented | Medium | MCPServer |
| `src/MIDI/MIDIRouter.*` | Complete | Implemented | Low | JUCE MIDI |
| `src/Preset/MetaPresetManager.*` | Enhancement | Implemented | Low | State System |
| `src/UI/MorphPad.*` | Critical | Implemented | Medium | VectorRenderer |
| `src/UI/SnapshotRing.*` | Complete | Implemented | Low | MorphPad |
| `src/UI/AIStatusPanel.*` | Critical | Partial | Medium | MCPServer |
| `src/UI/BreedingPanel.*` | Enhancement | Implemented | Medium | GeneticEngine |

### Risk Assessment

| Risk | Level | Mitigation |
|------|-------|------------|
| FL Studio plugin-hosting compatibility | High | 4MB stack allocation, extensive testing |
| Real-time audio thread safety | High | Static analysis, lock-free design |
| MCP protocol changes | Medium | Abstract client interface, version negotiation |
| DAW-specific automation quirks | Medium | Per-DAW compatibility testing |
| UI performance on low-end systems | Medium | Scalable rendering, adaptive quality |

---

## 5. Impact Analysis

### Codebase Impact

**Files Created:** 33 source files
**Files Modified:** N/A (greenfield project)
**Lines of Code:** ~15,000+ C++

### Data Model Changes

```
Plugin State XML Structure:
<MORPHSNAP_STATE>
  <PARAMETERS>...</PARAMETERS>
  <SNAPSHOTS>
    <SLOT index="0" name="..." color="...">
      <PARAM_VALUES>...</PARAM_VALUES>
    </SLOT>
    <!-- 12 slots total -->
  </SNAPSHOTS>
  <HOSTED_PLUGINS>
    <PLUGIN id="..." name="..." />
  </HOSTED_PLUGINS>
  <MCP_CONFIG port="30001" token="..." />
</MORPHSNAP_STATE>
```

### API Changes

| Endpoint | Type | Description |
|----------|------|-------------|
| `morphsnap://audio/context` | Resource | Audio analysis data |
| `morphsnap://plugin/state` | Resource | Current plugin state |
| `analyze_audio` | Tool | Request audio analysis |
| `generate_morph_trajectory` | Tool | Generate morphing path |
| `set_morph_position` | Tool | Set XY pad position |
| `capture_snapshot` | Tool | Capture current state |

### UI/UX Changes

- Central morphing pad (300x300 px default, scalable)
- 12 snapshot slots in ring arrangement
- Mode selector bar (XY/Fader/Elastic/Drift)
- AI status panel with connection indicator
- Plugin browser panel for hosted plugins
- Macro knob strip for additional controls

### Rollback Plan

1. State file format is versioned
2. Backward compatibility maintained for 2 major versions
3. Plugin validates state on load, rejects incompatible versions
4. Preset files are self-contained JSON/XML

### Tradeoffs

| Decision | Chosen | Alternative | Rationale |
|----------|--------|-------------|-----------|
| Plugin hosting | Internal JUCE host | External process | Lower latency, simpler deployment |
| AI protocol | MCP (JSON-RPC) | WebSocket custom | Industry standard, LLM-native |
| UI rendering | JUCE software | OpenGL/Metal | Cross-platform consistency |
| Snapshot count | 12 fixed | Unlimited | UI simplicity, performance |

---

## 6. Testing Strategy

### Unit Testing

**Framework:** Catch2 v3.x
**Target Coverage:** 90% branch coverage

| Module | Test File | Coverage Target |
|--------|-----------|-----------------|
| ParameterState | `tests/Unit/TestParameterState.cpp` | 95% |
| SnapshotBank | `tests/Unit/TestSnapshotBank.cpp` | 95% |
| InterpolationEngine | `tests/Unit/TestInterpolation.cpp` | 100% |
| PhysicsEngine | `tests/Unit/TestPhysicsEngine.cpp` | 90% |
| MorphProcessor | `tests/Unit/TestMorphProcessor.cpp` | 90% |
| MCPServer | `tests/Unit/TestMCPServer.cpp` | 80% |

### Integration Testing

| Scenario | Components | Validation |
|----------|------------|------------|
| Morph position → Parameter output | MorphProcessor, ParameterBridge | Verify interpolated values |
| Snapshot capture → State persistence | SnapshotBank, PresetSerializer | Verify round-trip |
| MCP command → Parameter update | MCPServer, MorphSnapProcessor | Verify latency < 100ms |
| Plugin load → Host detection | PluginHostManager, PluginScanner | Verify correct detection |

### Performance Testing

| Test | Target | Tool |
|------|--------|------|
| Audio thread allocations | 0 per buffer | Custom allocator tracking |
| ProcessBlock CPU time | < 0.5ms @ 256 samples | Google Benchmark |
| UI frame time | < 16.67ms | Frame profiler |
| Memory footprint | < 100MB | Heap profiling |

### Test Locations & Conventions

```
tests/
├── Unit/              # Isolated component tests
│   └── Test*.cpp      # Pattern: Test{Component}.cpp
├── Integration/       # Cross-component tests
│   └── *Integration.cpp
├── Performance/       # Benchmarks
│   └── *Benchmark.cpp
├── scripts/           # Test utilities
│   └── run_vst3_validator.py
└── testing-strategy.md
```

---

## 7. User Behavior Testing

### E2E Scenarios

| Scenario | Steps | Expected Outcome |
|----------|-------|------------------|
| Basic Morph | 1. Load plugin 2. Capture snapshot 3. Move pad | Parameters interpolate smoothly |
| AI Connection | 1. Start MCP server 2. Connect client 3. Send command | Command executed within 100ms |
| Preset Save/Load | 1. Configure morph 2. Save preset 3. Load preset | State restored exactly |
| DAW Automation | 1. Record morphX/Y automation 2. Play back | Automation reproduces movement |

### Acceptance Test Cases

| ID | Test Case | Preconditions | Steps | Expected Result |
|----|-----------|---------------|-------|-----------------|
| AT-001 | Capture snapshot from hosted plugin | Plugin loaded, host plugin present | Click snapshot slot | Slot populated with parameters |
| AT-002 | XY morph between 4 snapshots | 4 snapshots captured | Drag pad to corner | Corner snapshot values applied |
| AT-003 | MCP connection | MCP server running | Enter URI, connect | Status shows connected |
| AT-004 | AI trajectory generation | MCP connected, AI enabled | Request trajectory | Path displayed on pad |
| AT-005 | Preset persistence | State configured | Save, reload plugin | State preserved |

### User Flows

```
Basic Workflow:
[Load Plugin] → [Host Target Plugin] → [Capture Snapshots] → [Morph on Pad] → [Automate/Record]

AI-Assisted Workflow:
[Load Plugin] → [Connect to MCP] → [Enable AI] → [Request Suggestions] → [Apply/Modify]
```

### Regression Testing

- All unit tests must pass
- All integration tests must pass
- DAW compatibility matrix must be verified
- Performance benchmarks must not regress > 10%

---

## 8. Implementation Notes

### Patterns to Follow

1. **Lock-Free Communication**: Use `LockFreeQueue<T>` for all cross-thread data transfer
2. **RAII Resource Management**: All resources managed via smart pointers
3. **Atomic Parameters**: Use `std::atomic<T>` for thread-safe parameter access
4. **Pre-Allocated Buffers**: All audio buffers allocated in `prepareToPlay()`

### Architecture Decisions

| Decision | Rationale |
|----------|-----------|
| JUCE 8.x framework | Industry standard, active development, VST3/AU support |
| Embedded MCP server | Simplifies deployment, no external dependencies |
| 12-slot fixed design | Optimal for XY interpolation, UI simplicity |
| Physics-based morph modes | Provides natural-feeling automation |

### Technical Debt

| Item | Reason | Remediation Plan |
|------|--------|------------------|
| No SIMD optimization yet | Complexity tradeoff | Phase 2 optimization |
| Limited DAW testing | Resource constraints | Expand test matrix |
| Basic error handling | MVP focus | Comprehensive error system |

### Security Considerations

1. MCP authentication token required for all connections
2. No unvalidated JSON parsing
3. Plugin state sanitization on load
4. Rate limiting on MCP commands

---

## 9. Acceptance Criteria

Definition of done for this epic:

- [ ] All functional requirements (FR-001 through FR-013) implemented
- [ ] Test coverage meets 90% branch coverage target
- [ ] Zero audio thread allocations verified
- [ ] DAW compatibility verified (FL Studio, Ableton, Logic, Cubase, Reaper)
- [ ] MCP server connectivity validated with multiple AI backends
- [ ] Documentation updated (README, ARCHITECTURE, API docs)
- [ ] Performance benchmarks met (CPU < 2%, UI 60 FPS)
- [ ] Security review completed for MCP integration
- [ ] Beta testing feedback incorporated
- [ ] Installer/packages created for Windows/macOS

---

## 10. Open Questions & Risks

### Blockers

| ID | Blocker | Status | Resolution |
|----|---------|--------|------------|
| BLK-001 | FL Studio plugin-hosting stack requirements | Resolved | 4MB stack configuration |

### Unknowns

| ID | Unknown | Impact | Investigation Needed |
|----|---------|--------|---------------------|
| UNK-001 | Apple Silicon native AU validation | Medium | Test on M1/M2 hardware |
| UNK-002 | Linux VST3 DAW compatibility | Low | Test in Reaper/Bitwig Linux |
| UNK-003 | MCP protocol version compatibility | Medium | Test with multiple AI backends |

### Assumptions

1. Users have MCP-capable AI backend available (or can install one)
2. Target DAWs support plugin hosting within effects
3. Users understand parameter morphing concept
4. Network connectivity available for AI features

### Risks

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| DAW compatibility issues | Medium | High | Extensive testing matrix |
| MCP protocol changes | Low | Medium | Abstract interface, versioning |
| Performance issues on low-end systems | Medium | Medium | Adaptive quality settings |
| AI backend instability | Medium | Low | Graceful degradation, local fallback |

---

## 11. Codebase Analysis

### Affected Modules

```
D:/morphy/
├── src/
│   ├── Plugin/          # Main entry point - PluginProcessor, PluginEditor
│   ├── Core/            # Domain logic - SnapshotBank, Interpolation, Physics
│   ├── Host/            # Plugin hosting - PluginHostManager, ParameterBridge
│   ├── AI/              # MCP integration - MCPServer, MCPToolHandler
│   ├── MIDI/            # MIDI handling - MIDIRouter
│   ├── Preset/          # State management - MetaPresetManager, PresetSerializer
│   └── UI/              # User interface - MorphPad, Panels, LookAndFeel
```

### Patterns Discovered

**API/Protocol Patterns:**
- JSON-RPC 2.0 for MCP communication
- Lock-free SPSC queues for thread isolation
- Atomic parameters for real-time safety

**Component Patterns:**
- JUCE AudioProcessor as main plugin entry
- AudioProcessorValueTreeState for parameter management
- Component hierarchy for UI
- Listener patterns for cross-component communication

**Testing Patterns:**
- Catch2 test framework
- Test file naming: `Test{Component}.cpp`
- Mock objects for isolated testing

### Reference Implementations

| Feature | Reference | Location |
|---------|-----------|----------|
| Parameter interpolation | InterpolationEngine | `src/Core/InterpolationEngine.cpp` |
| Snapshot management | SnapshotBank | `src/Core/SnapshotBank.cpp` |
| MCP protocol handling | MCPServer | `src/AI/MCPServer.cpp` |
| UI morphing pad | MorphPad | `src/UI/MorphPad.cpp` |

---

## 12. Linked Tickets

**Epic:** MORPH-001 - AI-Powered VST3/AU Parameter Morphing Engine

### Ticket Breakdown

| Key | Summary | Type | Points | Status |
|-----|---------|------|--------|--------|
| MORPH-010 | Core Plugin Architecture | Foundation | 8 | Done |
| MORPH-011 | Parameter State Management | Foundation | 5 | Done |
| MORPH-012 | Snapshot Bank Implementation | Core | 8 | Done |
| MORPH-013 | Interpolation Engine | Core | 8 | Done |
| MORPH-014 | Physics Engine (Elastic Mode) | Core | 5 | Done |
| MORPH-015 | Genetic Engine (Breeding) | Enhancement | 5 | Done |
| MORPH-016 | Plugin Host Manager | Critical | 13 | Done |
| MORPH-017 | Parameter Bridge | Critical | 8 | Done |
| MORPH-018 | MCP Server Implementation | Critical | 13 | Done |
| MORPH-019 | MCP Tool Handler | Core | 8 | Done |
| MORPH-020 | MIDI Router | Core | 5 | Done |
| MORPH-021 | Preset System | Core | 8 | Done |
| MORPH-022 | MorphPad UI Component | Critical | 13 | Done |
| MORPH-023 | Snapshot Ring UI | Core | 5 | Done |
| MORPH-024 | AI Status Panel | Enhancement | 5 | Partial |
| MORPH-025 | Plugin Browser Panel | Core | 5 | Done |
| MORPH-026 | Macro Knob Strip | Enhancement | 3 | Done |
| MORPH-027 | LookAndFeel Theme System | UI | 8 | Done |
| MORPH-028 | DAW Compatibility Testing | QA | 13 | Pending |
| MORPH-029 | Performance Optimization | QA | 8 | Pending |
| MORPH-030 | Documentation | Docs | 5 | Partial |

**Total Story Points:** 158
**Completed:** 135
**Remaining:** 23

---

## 13. Implementation Roadmap

### Phase 1: Foundation (Complete)
- Core plugin architecture
- Parameter state management
- Snapshot bank
- Interpolation engine

### Phase 2: Core Features (Complete)
- Physics engine
- Plugin hosting
- Parameter bridging
- Morphing modes

### Phase 3: AI Integration (Complete)
- MCP server
- Tool handlers
- AI status panel

### Phase 4: UI Implementation (Complete)
- MorphPad component
- Snapshot ring
- Theme system
- All panels

### Phase 5: Testing & Polish (In Progress)
- [ ] DAW compatibility testing
- [ ] Performance optimization
- [ ] Complete test coverage
- [ ] Documentation finalization

### Phase 6: Release (Pending)
- [ ] Beta testing
- [ ] Bug fixes
- [ ] Installer creation
- [ ] Public release

---

## Document Metadata

**Version:** 1.0
**Created:** 2026-02-18
**Author:** Claude Code
**Last Updated:** 2026-02-18
**Status:** Ready for Review

---

## Implementation Plan

**Implementation Plan:** [IMPLEMENTATION_PLAN.md](./IMPLEMENTATION_PLAN.md)

### Remaining Work Summary

| Wave | Tickets | Points | Focus |
|------|---------|--------|-------|
| 1 | MORPH-024, MORPH-029 | 11 | Polish & Optimize |
| 2 | MORPH-028 | 13 | DAW Compatibility Testing |
| 3 | MORPH-030 | 4 | Documentation |

### Next Steps

1. Run `/execute-ticket MORPH-024` to complete AI Status Panel
2. Run `/execute-ticket MORPH-029` for performance optimization
3. Run `/execute-ticket MORPH-028` for DAW compatibility testing
4. Run `/execute-ticket MORPH-030` to complete documentation
5. Run `/complete-epic MORPH-001` to finalize the epic
