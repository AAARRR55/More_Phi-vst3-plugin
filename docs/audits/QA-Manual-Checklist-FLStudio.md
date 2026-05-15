# QA Manual Checklist — FL Studio

**Tester:** ________________  **Date:** ________________  **Build:** ________________

**DAW Version:** FL Studio 24+  **OS:** Windows 11

---

## Common Tests (8.1)

| # | Test | Procedure | Result | Notes |
|---|------|-----------|--------|-------|
| 8.1.1 | Plugin scan | Open plugin browser, search "More-Phi" | ☐ PASS ☐ FAIL | Should appear with correct name and version |
| 8.1.2 | Instantiation | Add More-Phi to mixer slot | ☐ PASS ☐ FAIL | Must load without crash; UI renders correctly |
| 8.1.3 | Parameter list | Open "Browse parameters" panel | ☐ PASS ☐ FAIL | All automatable parameters must be visible |
| 8.1.4 | Automation write | Right-click morph position, "Create automation clip", move parameter | ☐ PASS ☐ FAIL | DAW must record automation |
| 8.1.5 | Automation playback | Play back recorded automation clip | ☐ PASS ☐ FAIL | Smooth morph movement, no zipper noise |
| 8.1.6 | State save/load | Save project, close FL Studio, reopen | ☐ PASS ☐ FAIL | Full state restoration — snapshots, morph position, hosted plugin |
| 8.1.7 | Multi-instance (4x) | Add to 4 mixer slots with different hosted presets | ☐ PASS ☐ FAIL | Independent state per instance, no cross-talk |
| 8.1.8 | Plugin bypass | Toggle power button on FL wrapper | ☐ PASS ☐ FAIL | Audio passes cleanly when bypassed, no clicks/pops on toggle |
| 8.1.9 | Latency reporting | Check PDC settings for reported latency | ☐ PASS ☐ FAIL | Must match actual processing delay |
| 8.1.10 | Undo/redo | Ctrl+Z / Ctrl+Y after parameter change | ☐ PASS ☐ FAIL | Correct state at each step, no corruption |

---

## FL Studio-Specific (8.2)

| # | Test | Procedure | Result | Notes |
|---|------|-----------|--------|-------|
| 8.2.1 | Stack size | Host FabFilter Pro-Q 4 inside More-Phi | ☐ PASS ☐ FAIL | Must not stack-overflow crash (4 MB stack configured) |
| 8.2.2 | Wrapper compatibility | Use More-Phi inside FL Studio plugin wrapper | ☐ PASS ☐ FAIL | Full functionality within wrapper |
| 8.2.3 | Patcher | Add More-Phi inside FL Studio Patcher | ☐ PASS ☐ FAIL | Full functionality — no crash or broken routing |
| 8.2.4 | Fruity Envelope Controller | Route Fruity Envelope Controller output to More-Phi CC1 | ☐ PASS ☐ FAIL | Morph position must follow envelope shape |

---

## Stress Tests

| # | Test | Procedure | Result | Notes |
|---|------|-----------|--------|-------|
| S.1 | Sample rate change | Options > Audio > change sample rate while playing (44.1 > 48 > 96 > 44.1 kHz) | ☐ PASS ☐ FAIL | No crash, no audible glitch |
| S.2 | Buffer size change | Options > Audio > change buffer size while playing (32 > 128 > 512 > 1024 > 32) | ☐ PASS ☐ FAIL | No crash, clean transition |
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
