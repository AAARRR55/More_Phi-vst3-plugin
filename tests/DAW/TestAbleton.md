# MorphSnap - Ableton Live Test Plan

**DAW:** Ableton Live
**Versions Tested:** 11.x, 12.x
**Platform:** Windows 10/11, macOS 11+
**Plugin Format:** VST3

---

## Prerequisites

1. Ableton Live 11 Suite or Standard installed
2. MorphSnap.vst3 installed in VST3 folder
3. At least one third-party VST3 plugin for hosting tests
4. ASIO/Core Audio driver configured

---

## Ableton Live Specific Considerations

### Clip vs Track Automation
- Ableton distinguishes between clip automation and track automation
- Test both modes for morph parameters

### Warp Modes
- Audio may be processed differently based on warp settings
- Test with warp off and various warp modes

### Freeze/Flatten
- Test behavior when track is frozen
- Verify plugin state is preserved

---

## Test Cases

### TC-AB-001: Plugin Load

**Priority:** P0 - Critical
**Steps:**
1. Open Ableton Live
2. Open Browser > Plug-ins > VST3
3. Locate MorphSnap
4. Drag to audio or MIDI track

**Expected Result:**
- Plugin appears in VST3 list
- Plugin loads without crash
- Plugin window opens

**Status:** ⏳ Pending

---

### TC-AB-002: Hosted Plugin

**Priority:** P0 - Critical
**Steps:**
1. Add MorphSnap to track
2. Click "Load Plugin" in MorphSnap UI
3. Select a VST3 instrument
4. Play via MIDI

**Expected Result:**
- Hosted plugin loads
- MIDI reaches hosted plugin
- Audio outputs correctly

**Status:** ⏳ Pending

---

### TC-AB-003: Track Automation

**Priority:** P1 - High
**Steps:**
1. Add MorphSnap to track
2. Enable Automation Arm
3. Move morphX during playback
4. Stop and play back
5. View automation lane

**Expected Result:**
- Automation recorded on track
- Playback reproduces movements
- Automation editable in lane

**Status:** ⏳ Pending

---

### TC-AB-004: Clip Automation

**Priority:** P1 - High
**Steps:**
1. Create MIDI clip
2. Show clip envelope
3. Select morphX from envelope dropdown
4. Draw automation
5. Play clip

**Expected Result:**
- Envelope controls morphX
- Loop works correctly
- Envelope editable

**Status:** ⏳ Pending

---

### TC-AB-005: Set State

**Priority:** P1 - High
**Steps:**
1. Configure MorphSnap with snapshots
2. Save Live Set
3. Close Ableton
4. Reopen Live Set

**Expected Result:**
- Set loads correctly
- All snapshots restored
- Morph position restored

**Status:** ⏳ Pending

---

### TC-AB-006: Freeze Track

**Priority:** P2 - Medium
**Steps:**
1. Add MorphSnap with hosted plugin
2. Create some audio/MIDI content
3. Freeze the track
4. Unfreeze
5. Verify plugin state

**Expected Result:**
- Track freezes correctly
- Plugin state preserved after unfreeze
- No crashes

**Status:** ⏳ Pending

---

### TC-AB-007: MCP Integration

**Priority:** P2 - Medium
**Steps:**
1. Add MorphSnap to project
2. Verify MCP server starts
3. Connect external AI client
4. Execute commands

**Expected Result:**
- MCP works in Ableton
- Commands execute
- UI updates

**Status:** ⏳ Pending

---

## Ableton Live Specific Bugs

| Bug ID | Description | Severity | Status |
|--------|-------------|----------|--------|
| | | | |

---

## Notes

- Test in both Session View and Arrangement View
- Test with Warp modes: Beats, Tones, Texture, Complex
- Verify behavior with "Reduce latency when monitoring" option
