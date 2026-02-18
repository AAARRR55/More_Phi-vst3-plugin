# MorphSnap - Logic Pro Test Plan

**DAW:** Logic Pro
**Versions Tested:** Logic Pro X, Logic Pro 11
**Platform:** macOS 11+
**Plugin Format:** AU (Audio Unit)

---

## Prerequisites

1. Logic Pro X or later installed
2. MorphSnap.component installed in:
   - `/Library/Audio/Plug-Ins/Components/` (system)
   - `~/Library/Audio/Plug-Ins/Components/` (user)
3. At least one third-party AU plugin for hosting tests
4. Core Audio configured

---

## Logic Pro Specific Considerations

### AU Validation
- Logic requires AU plugins to pass `auval` validation
- Run: `auval -v aufx Mrph Mrps` (for AU effect)
- Plugin must pass all validation stages

### Smart Controls
- Logic may map plugin parameters to Smart Controls
- Test parameter mapping behavior

### Automation Modes
- Logic has multiple automation modes (Read, Touch, Latch, Write)
- Test each mode with morph parameters

---

## Test Cases

### TC-LP-001: AU Validation

**Priority:** P0 - Critical
**Steps:**
1. Open Terminal
2. Run: `auval -v aufx Mrph Mrps`
3. Observe validation output

**Expected Result:**
- All validation stages pass
- No warnings or errors
- Plugin marked as valid

**Status:** ⏳ Pending

---

### TC-LP-002: Plugin Load

**Priority:** P0 - Critical
**Steps:**
1. Open Logic Pro
2. Create new project
3. Insert > Audio Unit > MorphSnap
4. Verify plugin window opens

**Expected Result:**
- Plugin appears in AU menu
- Loads without crash
- UI displays correctly

**Status:** ⏳ Pending

---

### TC-LP-003: Hosted Plugin (AU)

**Priority:** P0 - Critical
**Steps:**
1. Add MorphSnap to channel
2. Load an AU instrument
3. Play via MIDI

**Expected Result:**
- AU plugin loads in host
- MIDI triggers audio
- Audio passes through

**Status:** ⏳ Pending

---

### TC-LP-004: Automation Recording

**Priority:** P1 - High
**Steps:**
1. Add MorphSnap
2. Set automation mode to "Touch"
3. Record-enable track
4. Move morphX during playback
5. Play back recording

**Expected Result:**
- Automation recorded
- Touch mode works correctly
- Playback reproduces movements

**Status:** ⏳ Pending

---

### TC-LP-005: Smart Controls

**Priority:** P2 - Medium
**Steps:**
1. Add MorphSnap
2. Open Smart Controls
3. Verify parameters are mapped
4. Adjust via Smart Control
5. Verify plugin responds

**Expected Result:**
- Parameters appear in Smart Controls
- Adjustments affect plugin
- No mapping conflicts

**Status:** ⏳ Pending

---

### TC-LP-006: Project Save

**Priority:** P1 - High
**Steps:**
1. Create project with MorphSnap
2. Capture snapshots
3. Save project
4. Close Logic
5. Reopen project

**Expected Result:**
- Project loads correctly
- Snapshots preserved
- All state intact

**Status:** ⏳ Pending

---

### TC-LP-007: MCP Integration

**Priority:** P2 - Medium
**Steps:**
1. Add MorphSnap
2. Check MCP status
3. Connect AI client
4. Execute commands

**Expected Result:**
- MCP server starts
- External connections work
- Commands execute

**Status:** ⏳ Pending

---

### TC-LP-008: Apple Silicon Native

**Priority:** P1 - High (if applicable)
**Steps:**
1. On Apple Silicon Mac, run Logic natively (not Rosetta)
2. Load MorphSnap
3. Verify AU loads correctly
4. Test all functionality

**Expected Result:**
- Plugin runs natively
- No Rosetta required
- Performance optimal

**Status:** ⏳ Pending

---

## Logic Pro Specific Bugs

| Bug ID | Description | Severity | Status |
|--------|-------------|----------|--------|
| | | | |

---

## Notes

- Test with different sample rates (44.1kHz, 48kHz, 96kHz)
- Test with I/O buffer sizes: 32, 64, 128, 256, 512, 1024
- Verify behavior with "Process Buffer Range" settings
- Test with "Multi-threading" options enabled/disabled
