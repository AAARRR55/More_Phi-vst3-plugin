# MORPH-028: DAW Compatibility Testing Checklist

Manual validation checklist for each supported DAW.  
Automated integration tests cover the programmatic API contract — this checklist covers host-specific behaviors.

---

## Pre-Test Setup
- [ ] Build Release VST3: `cmake --build build --config Release`
- [ ] Copy `MorePhi.vst3` to system plugin folder
- [ ] Rescan plugins in each DAW

---

## Per-DAW Checklist

### FL Studio 21+
- [ ] Plugin loads without crash in 64-bit mode
- [ ] Parameter automation records & plays back correctly
- [ ] State saves with FL Studio project (`.flp`)
- [ ] State restores when reopening project
- [ ] Sidechain bus routes correctly via FL sidechain routing
- [ ] CPU meter stays under 2% during playback
- [ ] No audio glitches during 5-minute sustained playback

### Ableton Live 11+
- [ ] Plugin loads in instrument/audio effect rack
- [ ] Automation lanes available for all 4 new parameters
- [ ] Session save/restore preserves all state
- [ ] Sidechain input routable via Ableton's routing
- [ ] No issues with Ableton's chunked processBlock sizes

### Reaper 7+
- [ ] Plugin loads via VST3 scan
- [ ] FX parameter list shows all APVTS parameters
- [ ] Project save/restore round-trip works
- [ ] Sidechain pin routing works via Reaper's I/O dialog
- [ ] No thread safety issues with Reaper's multi-threaded FX processing

### Bitwig Studio 5+
- [ ] Plugin loads and UI renders
- [ ] Modulation of new parameters works
- [ ] Sidechain routing via Bitwig's modular routing
- [ ] State persistence through project save/load

### Logic Pro (macOS only)
- [ ] AU validation passes (`auval -v aufx Mrph Mrph`)
- [ ] Plugin loads in Logic channel strip
- [ ] Automation recording works
- [ ] State saves with Logic project

---

## Cross-DAW Validation
- [ ] State exported from DAW A can be imported in DAW B (via preset file, not project)
- [ ] SanityMode protected indices persist across DAW restarts
- [ ] RecallMode setting persists across DAW restarts
- [ ] Sidechain threshold persists across DAW restarts

---

## Pass Criteria
All items checked = **MORPH-028 PASS**  
Any crash or data loss = **FAIL** (file bug with DAW name, version, and repro steps)
