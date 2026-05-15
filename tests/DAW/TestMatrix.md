# More-Phi DAW Compatibility Test Matrix

## Overview

This document defines the comprehensive testing strategy for More-Phi VST3/AU plugin across all supported DAWs.

**Plugin Version:** 1.0.0
**Last Updated:** 2026-05-05
**Test Lead:** Development Team

**Current remediation note:** The 2026-05-05 audit remediation keeps DAW
execution manual. Run automated build/CTest plus `vst3_validator` and
`pluginval` first; if those tools or DAWs are not installed, mark the row
blocked with the missing tool/host instead of treating it as passed.

---

## Supported Platforms

| Platform | Plugin Format | Architecture |
|----------|---------------|--------------|
| Windows 10/11 | VST3 | x64 |
| macOS 11+ | VST3, AU | x64, ARM64 |

---

## DAW Test Matrix

| DAW | Version | Platform | Format | Test Status | Notes |
|-----|---------|----------|--------|-------------|-------|
| FL Studio | 21+ | Windows | VST3 | ⏳ Pending | Requires 4MB stack |
| Ableton Live | 11+ | Win/Mac | VST3 | ⏳ Pending | - |
| Logic Pro | X | macOS | AU | ⏳ Pending | AU validation required |
| Cubase | 12+ | Win/Mac | VST3 | ⏳ Pending | - |
| Reaper | 6+ | Win/Mac/Linux | VST3 | ⏳ Pending | - |
| Studio One | 6+ | Win/Mac | VST3 | ⏳ Pending | - |
| Bitwig Studio | 5+ | Win/Mac/Linux | VST3 | ⏳ Pending | - |

---

## Test Categories

### 1. Plugin Loading (P0 - Critical)
- [ ] Plugin loads without crash
- [ ] Plugin window opens correctly
- [ ] Plugin reports correct latency (0 samples)
- [ ] Plugin reports correct tail time
- [ ] Bypass works correctly

### 2. Audio Processing (P0 - Critical)
- [ ] Audio passes through correctly
- [ ] No audio dropouts during morphing
- [ ] No clicks/pops during parameter changes
- [ ] Sample rate changes handled correctly
- [ ] Buffer size changes handled correctly

### 3. Plugin Hosting (P0 - Critical)
- [ ] Can scan for plugins
- [ ] Can load hosted VST3/AU plugins
- [ ] Hosted plugin audio processes correctly
- [ ] Hosted plugin parameters are captured
- [ ] Hosted plugin state persists

### 4. Morphing (P0 - Critical)
- [ ] XY Pad morphing works
- [ ] Fader morphing works
- [ ] Elastic physics mode works
- [ ] Drift physics mode works
- [ ] Snapshot capture/recall works
- [ ] 12 snapshot slots function correctly

### 5. Automation (P1 - High)
- [ ] morphX parameter automatable
- [ ] morphY parameter automatable
- [ ] faderPos parameter automatable
- [ ] Automation recording works
- [ ] Automation playback works
- [ ] Automation curves render correctly

### 6. State Persistence (P1 - High)
- [ ] Plugin state saves correctly
- [ ] Plugin state loads correctly
- [ ] Snapshots persist in saved project
- [ ] Hosted plugin state persists
- [ ] MCP connection state not saved (expected)

### 7. MCP Integration (P2 - Medium)
- [ ] MCP server starts correctly
- [ ] MCP server accepts connections
- [ ] AI commands execute correctly
- [ ] Morph commands work via MCP
- [ ] Snapshot commands work via MCP
- [ ] Error handling works correctly

### 8. UI/UX (P2 - Medium)
- [ ] Window resizes correctly
- [ ] All controls respond to input
- [ ] Visual feedback is correct
- [ ] No UI lag during heavy processing
- [ ] High DPI scaling works

### 9. Performance (P2 - Medium)
- [ ] CPU usage < 2% at 48kHz/256 samples
- [ ] Memory footprint < 100MB
- [ ] No memory leaks over 1 hour use
- [ ] No audio thread allocations

---

## Test Execution Schedule

### Phase 1: Core Functionality (Week 1)
- FL Studio (primary development target)
- Reaper (flexible testing)

### Phase 2: Major DAWs (Week 2)
- Ableton Live
- Cubase
- Logic Pro (requires Mac)

### Phase 3: Additional DAWs (Week 3)
- Studio One
- Bitwig Studio

---

## Defect Severity Levels

| Level | Description | Response Time |
|-------|-------------|---------------|
| **Critical (P0)** | Crash, data loss, audio corruption | Immediate |
| **High (P1)** | Feature broken, significant usability issue | 24-48 hours |
| **Medium (P2)** | Feature impaired, workaround exists | 1 week |
| **Low (P3)** | Minor issue, cosmetic | Next release |

---

## Known Issues

| Issue | DAW | Severity | Status | Notes |
|-------|-----|----------|--------|-------|
| Validator coverage blocked when `vst3_validator` or `pluginval` is absent | All | P1 | Open | Install tools before release sign-off. |
| Manual DAW execution remains pending after 2026-05-05 audit | All | P1 | Open | Use the current built VST3 artifact, not stale installed copies. |

---

## Test Environment Requirements

### Windows
- Windows 10 21H2 or Windows 11
- ASIO driver recommended
- 8GB RAM minimum
- VST3 plugin folder access

### macOS
- macOS 11 Big Sur or later
- Core Audio
- 8GB RAM minimum
- AU validation tool (auval)

---

## Reporting Template

```markdown
## Test Report: [DAW Name] [Version]

**Date:** YYYY-MM-DD
**Tester:** Name
**Platform:** Windows/macOS [version]
**Plugin Version:** 1.0.0

### Summary
- Total Tests: X
- Passed: X
- Failed: X
- Blocked: X

### Failed Tests
| Test ID | Description | Steps to Reproduce | Notes |
|---------|-------------|-------------------|-------|
| | | | |

### Blockers
| Issue | Reason | Workaround |
|-------|--------|------------|
| | | |

### Notes
[Any additional observations]
```

---

## Document Links

- [FL Studio Test Plan](./TestFLStudio.md)
- [Ableton Live Test Plan](./TestAbleton.md)
- [Logic Pro Test Plan](./TestLogic.md)
- [Reaper Test Plan](./TestReaper.md)
- [MCP Integration Tests](../Integration/TestMCPIntegration.cpp)
