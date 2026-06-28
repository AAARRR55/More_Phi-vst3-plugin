# MORPH-028: DAW Compatibility Testing Checklist

**Version:** More-Phi v3.4.1

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
- [ ] Automation lanes available for all APVTS parameters
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

## Multi-Agent Orchestration Testing (v3.4.0+)

### Core Orchestrator Checks
- [ ] Agent orchestrator starts successfully in DAW
- [ ] MCP server starts on correct port per instance
- [ ] AI goal submission works through UI
- [ ] Agent states display correctly in AI status panel
- [ ] RealtimeControl agent responds to clipping events
- [ ] Creative agent suggestions require approval
- [ ] Graceful degradation when MCP is disabled

### DAW-Specific Orchestrator Tests

#### FL Studio 21+
- [ ] Multiple plugin instances each receive unique MCP ports (base port + instance offset)
- [ ] AI status panel renders correctly in FL Studio's plugin wrapper window
- [ ] No CPU spikes when orchestrator agents perform background analysis

#### Ableton Live 11+
- [ ] AI goal parameter is automatable via Live's parameter lane
- [ ] Agent state parameters appear in Live's automation lane list
- [ ] Freeze/flatten preserves agent configuration state

#### Reaper 7+
- [ ] Multi-instance MCP port allocation works with Reaper's multi-threaded FX processing
- [ ] Agent orchestrator survives Reaper's "Run FX when not playing" idle mode

#### Bitwig Studio 5+
- [ ] Bitwig's parameter modulation integrates with agent-controlled parameters
- [ ] Orchestrator state persists through Bitwig's project save/load with container nesting

#### Logic Pro (macOS only)
- [ ] AU sandbox does not block localhost MCP communication
- [ ] Agent panel renders correctly in Logic's plugin window on both Intel and Apple Silicon

---

## Pass Criteria
All items checked = **MORPH-028 PASS**  
Any crash or data loss = **FAIL** (file bug with DAW name, version, and repro steps)
