# QA Manual Checklist — Ableton Live

**Tester:** ________________  **Date:** ________________  **Build:** ________________

**DAW Version:** Ableton Live 12+  **OS:** Windows 11 / macOS

---

## Common Tests (8.1)

| # | Test | Procedure | Result | Notes |
|---|------|-----------|--------|-------|
| 8.1.1 | Plugin scan | Preferences > Plug-ins > rescan | ☐ PASS ☐ FAIL | More-Phi appears with correct name and version |
| 8.1.2 | Instantiation | Drag More-Phi to audio track | ☐ PASS ☐ FAIL | Must load without crash; UI renders correctly |
| 8.1.3 | Parameter list | Click plug-in icon > "Configure" > verify params | ☐ PASS ☐ FAIL | All automatable parameters must be visible |
| 8.1.4 | Automation write | Arm track, move morph position, record | ☐ PASS ☐ FAIL | DAW must record automation |
| 8.1.5 | Automation playback | Play back recorded automation | ☐ PASS ☐ FAIL | Smooth morph movement, no zipper noise |
| 8.1.6 | State save/load | Save Live set, close Ableton, reopen | ☐ PASS ☐ FAIL | Full state restoration — snapshots, morph position, hosted plugin |
| 8.1.7 | Multi-instance (4x) | 4 tracks, each with More-Phi + different hosted preset | ☐ PASS ☐ FAIL | Independent state per instance, no cross-talk |
| 8.1.8 | Plugin bypass | Click plug-in activator button | ☐ PASS ☐ FAIL | Audio passes cleanly when bypassed, no clicks/pops on toggle |
| 8.1.9 | Latency reporting | Check latency compensation indicator | ☐ PASS ☐ FAIL | Must match actual processing delay |
| 8.1.10 | Undo/redo | Ctrl+Z / Ctrl+Y after parameter change | ☐ PASS ☐ FAIL | Correct state at each step, no corruption |

---

## Ableton Live-Specific (8.3)

| # | Test | Procedure | Result | Notes |
|---|------|-----------|--------|-------|
| 8.3.1 | Device view | Open More-Phi in Detail/Device view | ☐ PASS ☐ FAIL | All parameters visible in device chain |
| 8.3.2 | Clip automation | Draw automation curve in Arrangement or MIDI clip | ☐ PASS ☐ FAIL | Morph follows drawn automation exactly, no deviation |
| 8.3.3 | Freeze/flatten | Freeze track with More-Phi, then unfreeze | ☐ PASS ☐ FAIL | State must be fully preserved after unfreeze |
| 8.3.4 | Macro mapping | Map morph position to macro knob, move macro | ☐ PASS ☐ FAIL | Macro must control morph position |
| 8.3.5 | Group device | Place More-Phi in a device group | ☐ PASS ☐ FAIL | Full functionality within group — no routing breakage |

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
| A.2 | MIDI note trigger | Route MIDI track to More-Phi, play C3-B3 chromatically | ☐ PASS ☐ FAIL | Each note triggers correct snapshot slot |
| A.3 | CC1 sweep | Automate CC1 from 0 to 127 and back | ☐ PASS ☐ FAIL | Smooth morph position follow, no zipper noise |

---

## Overall Result

**Decision:** ☐ PASS  ☐ FAIL

**Blockers:**

______________________________________________________________

______________________________________________________________

______________________________________________________________

**Tester Signature:** ________________  **Date:** ________________
