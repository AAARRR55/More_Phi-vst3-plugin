# More-Phi - FL Studio Test Plan

**DAW:** FL Studio
**Versions Tested:** 21.x, 24.x
**Platform:** Windows 10/11
**Plugin Format:** VST3

---

## Prerequisites

1. FL Studio 21 or later installed
2. MorePhi.vst3 installed in VST3 folder
3. At least one third-party VST3 plugin for hosting tests
4. ASIO audio driver configured

---

## FL Studio Specific Considerations

### Stack Size Requirements
- FL Studio uses a smaller stack by default
- More-Phi requires 4MB stack (configured via CMake: `/STACK:4194304`)
- **Known Issue:** Plugin-in-plugin hosting may crash with insufficient stack

### PDC (Plugin Delay Compensation)
- More-Phi reports 0 latency
- No PDC should be applied
- Test with other latency-inducing plugins in chain

### Wrapper Behavior
- FL Studio wraps VST3 in its own wrapper
- Some UI behaviors may differ from native VST3

---

## Test Cases

### TC-FL-001: Plugin Scan and Load

**Priority:** P0 - Critical
**Steps:**
1. Open FL Studio
2. Open Plugin Manager (Options > Manage plugins)
3. Scan for new plugins
4. Verify More-Phi appears in plugin list
5. Add More-Phi to a mixer insert

**Expected Result:**
- Plugin appears in scan results
- Plugin loads without error dialog
- Plugin window opens showing MorphPad

**Status:** ⏳ Pending

---

### TC-FL-002: Hosted Plugin Loading

**Priority:** P0 - Critical
**Steps:**
1. Add More-Phi to mixer insert
2. Click "Load Plugin" button
3. Select a VST3 synth (e.g., Serum, Vital)
4. Wait for plugin to load

**Expected Result:**
- Hosted plugin loads successfully
- Hosted plugin window opens
- No crash or hang

**Status:** ⏳ Pending

---

### TC-FL-003: Audio Throughput

**Priority:** P0 - Critical
**Steps:**
1. Add More-Phi with hosted synth
2. Play MIDI notes via FL Studio piano roll
3. Verify audio passes through to master

**Expected Result:**
- Audio plays correctly
- No dropouts
- No distortion

**Status:** ⏳ Pending

---

### TC-FL-004: Snapshot Capture

**Priority:** P0 - Critical
**Steps:**
1. Load hosted plugin with multiple parameters
2. Modify some parameters
3. Double-click on MorphPad to capture snapshot
4. Modify parameters again
5. Capture second snapshot
6. Move morph cursor between positions
7. Verify parameter interpolation

**Expected Result:**
- Snapshots capture correctly
- Parameters interpolate smoothly
- No audio glitches during morphing

**Status:** ⏳ Pending

---

### TC-FL-005: Automation Recording

**Priority:** P1 - High
**Steps:**
1. Add More-Phi to project
2. Enable "Record automation" in FL Studio
3. Move morphX/morphY during playback
4. Stop recording
5. Play back and verify automation

**Expected Result:**
- Automation is recorded
- Playback reproduces the movements
- Automation can be edited in event editor

**Status:** ⏳ Pending

---

### TC-FL-006: Project Save/Load

**Priority:** P1 - High
**Steps:**
1. Create project with More-Phi + hosted plugin
2. Capture several snapshots
3. Set morph position to specific location
4. Save project (.flp)
5. Close FL Studio
6. Reopen and load project

**Expected Result:**
- Project loads without error
- All snapshots restored
- Morph position restored
- Hosted plugin state restored

**Status:** ⏳ Pending

---

### TC-FL-007: MCP Server

**Priority:** P2 - Medium
**Steps:**
1. Add More-Phi to project
2. Check AI Status Panel shows "MCP Running"
3. Connect external MCP client (e.g., Claude)
4. Send morph command via MCP
5. Verify morph position changes

**Expected Result:**
- MCP server starts on port 30001
- Client can connect
- Commands execute correctly
- UI updates reflect MCP changes

**Status:** ⏳ Pending

---

### TC-FL-008: Physics Modes

**Priority:** P1 - High
**Steps:**
1. Set Physics Mode to "Elastic"
2. Drag morph cursor and release
3. Verify spring-back animation
4. Set Physics Mode to "Drift"
5. Enable drift mode
6. Verify cursor drifts around target

**Expected Result:**
- Elastic mode bounces correctly
- Drift mode moves organically
- No audio artifacts during physics updates

**Status:** ⏳ Pending

---

### TC-FL-009: Multiple Instances

**Priority:** P2 - Medium
**Steps:**
1. Add More-Phi to Insert 1
2. Add More-Phi to Insert 2
3. Load different hosted plugins in each
4. Process audio through both

**Expected Result:**
- Both instances work independently
- No audio corruption
- Memory usage reasonable

**Status:** ⏳ Pending

---

### TC-FL-010: CPU Performance

**Priority:** P2 - Medium
**Steps:**
1. Add More-Phi with hosted plugin
2. Enable all 12 snapshot slots
3. Continuously morph for 60 seconds
4. Monitor CPU usage in FL Studio

**Expected Result:**
- CPU usage < 5%
- No audio dropouts
- UI remains responsive

**Status:** ⏳ Pending

---

## FL Studio Specific Bugs

| Bug ID | Description | Severity | Status |
|--------|-------------|----------|--------|
| | | | |

---

## Notes

- FL Studio's "Smart disable" feature may cause issues with plugin hosting
- Test with "Anticipative fr" both on and off
- Test with different buffer sizes (64, 128, 256, 512, 1024)
