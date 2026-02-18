# MorphSnap - Reaper Test Plan

**DAW:** Reaper
**Versions Tested:** 6.x, 7.x
**Platform:** Windows 10/11, macOS 11+, Linux
**Plugin Format:** VST3

---

## Prerequisites

1. Reaper 6.0 or later installed
2. MorphSnap.vst3 installed in VST3 path
3. At least one third-party plugin for hosting tests
4. Audio device configured (ASIO/JACK/Core Audio)

---

## Reaper Specific Considerations

### Cross-Platform
- Reaper runs on Windows, macOS, and Linux
- Test on all supported platforms

### Flexible Routing
- Reaper allows complex routing
- Test various routing scenarios

### SWS Extensions
- Some users may have SWS extensions installed
- Test compatibility

---

## Test Cases

### TC-RP-001: Plugin Scan

**Priority:** P0 - Critical
**Steps:**
1. Open Reaper
2. Options > Preferences > VST
3. Clear cache and re-scan
4. Verify MorphSnap found

**Expected Result:**
- Plugin appears in scan results
- No scan errors
- Plugin available in FX browser

**Status:** ⏳ Pending

---

### TC-RP-002: Add to Track

**Priority:** P0 - Critical
**Steps:**
1. Create new track
2. Click FX button
3. Add MorphSnap
4. Verify UI opens

**Expected Result:**
- FX window shows MorphSnap
- UI renders correctly
- No crashes

**Status:** ⏳ Pending

---

### TC-RP-003: Hosted Plugin

**Priority:** P0 - Critical
**Steps:**
1. Add MorphSnap
2. Load hosted plugin
3. Create MIDI item
4. Play notes

**Expected Result:**
- Hosted plugin loads
- MIDI plays correctly
- Audio outputs

**Status:** ⏳ Pending

---

### TC-RP-004: Automation

**Priority:** P1 - High
**Steps:**
1. Add MorphSnap
2. Show track envelopes
3. Select morphX
4. Draw automation
5. Play project

**Expected Result:**
- Envelope controls parameter
- Automation plays back
- Editing works correctly

**Status:** ⏳ Pending

---

### TC-RP-005: Project Save

**Priority:** P1 - High
**Steps:**
1. Create project with MorphSnap
2. Configure snapshots
3. Save project (.rpp)
4. Close Reaper
5. Reopen project

**Expected Result:**
- Project loads
- Snapshots restored
- State preserved

**Status:** ⏳ Pending

---

### TC-RP-006: Linux Compatibility

**Priority:** P2 - Medium (Linux only)
**Steps:**
1. On Linux system with Reaper
2. Install MorphSnap via Wine or natively
3. Load plugin
4. Test basic functionality

**Expected Result:**
- Plugin loads
- Audio works
- UI functional

**Status:** ⏳ Pending

---

### TC-RP-007: Multiple Instances

**Priority:** P2 - Medium
**Steps:**
1. Add MorphSnap to track 1
2. Add MorphSnap to track 2
3. Load different hosted plugins
4. Process simultaneously

**Expected Result:**
- Both instances work
- No conflicts
- Performance acceptable

**Status:** ⏳ Pending

---

### TC-RP-008: MCP Integration

**Priority:** P2 - Medium
**Steps:**
1. Add MorphSnap
2. Verify MCP starts
3. Connect client
4. Test commands

**Expected Result:**
- MCP server runs
- Connections accepted
- Commands work

**Status:** ⏳ Pending

---

## Reaper Specific Bugs

| Bug ID | Description | Severity | Status |
|--------|-------------|----------|--------|
| | | | |

---

## Notes

- Test with different audio subsystems (ASIO, WASAPI, DirectSound on Windows)
- Test with ReaPlugs in chain
- Verify behavior with "Anticipative FX processing" setting
- Test freeze/unfreeze functionality
