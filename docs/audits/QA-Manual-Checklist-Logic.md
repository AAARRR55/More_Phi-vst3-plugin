# QA Manual Checklist — Logic Pro

**Tester:** ________________  **Date:** ________________  **Build:** ________________

**DAW Version:** Logic Pro 11+  **OS:** macOS

---

## Common Tests (8.1)

| # | Test | Procedure | Result | Notes |
|---|------|-----------|--------|-------|
| 8.1.1 | Plugin scan | Open Audio Units Manager, scan for plugins | ☐ PASS ☐ FAIL | MorphSnap AU appears with correct name and version |
| 8.1.2 | Instantiation | Insert MorphSnap on audio instrument track | ☐ PASS ☐ FAIL | Must load without crash; UI renders correctly |
| 8.1.3 | Parameter list | Open automation view, verify parameter list | ☐ PASS ☐ FAIL | All automatable parameters must be visible |
| 8.1.4 | Automation write | Enable automation, move morph position | ☐ PASS ☐ FAIL | DAW must record automation |
| 8.1.5 | Automation playback | Play back recorded automation | ☐ PASS ☐ FAIL | Smooth morph movement, no zipper noise |
| 8.1.6 | State save/load | Save project, close Logic Pro, reopen | ☐ PASS ☐ FAIL | Full state restoration — snapshots, morph position, hosted plugin |
| 8.1.7 | Multi-instance (4x) | Load 4 instances on separate tracks with different hosted presets | ☐ PASS ☐ FAIL | Independent state per instance, no cross-talk |
| 8.1.8 | Plugin bypass | Toggle bypass button | ☐ PASS ☐ FAIL | Audio passes cleanly when bypassed, no clicks/pops on toggle |
| 8.1.9 | Latency reporting | Check latency compensation indicator | ☐ PASS ☐ FAIL | Must match actual processing delay |
| 8.1.10 | Undo/redo | Cmd+Z / Cmd+Shift+Z after parameter change | ☐ PASS ☐ FAIL | Correct state at each step, no corruption |

---

## Logic Pro-Specific (8.4)

| # | Test | Procedure | Result | Notes |
|---|------|-----------|--------|-------|
| 8.4.1 | AU validation | Run `auval -v aufx Mrph Mrph` in Terminal | ☐ PASS ☐ FAIL | Must pass auval without errors |
| 8.4.2 | Smart Controls | Open Smart Controls for MorphSnap | ☐ PASS ☐ FAIL | Parameters must be mappable to Smart Controls |
| 8.4.3 | Track stack | Use MorphSnap inside a track stack | ☐ PASS ☐ FAIL | Full functionality — no crash or routing issues |
| 8.4.4 | AU MIDI effects | Insert MorphSnap as MIDI FX on instrument track | ☐ PASS ☐ FAIL | MIDI notes must trigger snapshot recalls |
| 8.4.5 | Bounce/render | Bounce track with MorphSnap offline, compare with real-time render | ☐ PASS ☐ FAIL | Offline render must match real-time audio (no artifacts) |

---

## Stress Tests

| # | Test | Procedure | Result | Notes |
|---|------|-----------|--------|-------|
| S.1 | Sample rate change | Change sample rate in Preferences while playing (44.1 > 48 > 96 > 44.1 kHz) | ☐ PASS ☐ FAIL | No crash, no audible glitch |
| S.2 | Buffer size change | Change buffer size in Preferences while playing (32 > 128 > 512 > 1024 > 32) | ☐ PASS ☐ FAIL | No crash, clean transition |
| S.3 | 5-minute drift mode | Enable Drift Free mode, let run 5 minutes with Pro-Q 4 hosted | ☐ PASS ☐ FAIL | No audible artifacts, no parameter stuck states |

---

## Morph Audio Validation (Manual Listening)

| # | Test | Procedure | Result | Notes |
|---|------|-----------|--------|-------|
| A.1 | Smooth morph | Create 2 snapshots with drastically different EQ curves, morph between them | ☐ PASS ☐ FAIL | Smooth spectral transition, no clicks/pops/zipper noise |
| A.2 | MIDI note trigger | Route MIDI track to MorphSnap, play C3-B3 chromatically | ☐ PASS ☐ FAIL | Each note triggers correct snapshot slot |
| A.3 | CC1 sweep | Automate CC1 from 0 to 127 and back | ☐ PASS ☐ FAIL | Smooth morph position follow, no zipper noise |

---

## Overall Result

**Decision:** ☐ PASS  ☐ FAIL

**Blockers:**

______________________________________________________________

______________________________________________________________

______________________________________________________________

**Tester Signature:** ________________  **Date:** ________________
