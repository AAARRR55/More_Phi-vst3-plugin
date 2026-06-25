# More-Phi Weakpoint Fixes — Technical Plan

**Date:** 2026-07-16  
**Status:** In Progress  
**Based on:** SnappySnap 3.5.0 comparison analysis

## Overview

Five phases addressing the key gaps identified in the More-Phi vs. SnappySnap comparison.
Each phase is independently shippable. Ordered by ascending effort.

---

## Phase 1: Gravity Wells (Per-Snapshot Mass Weights)

**Effort:** Low | **Impact:** High | **Risk:** Minimal

### What
Each snapshot slot gets a user-adjustable "mass" weight (range 0.1–3.0, default 1.0) that
expands or contracts its influence in the 2D morph blend. High mass = wider sweet spot.

### Design

**Data model:**
- Add `std::array<float, 12> snapshotMasses_` to `SnapshotBank`
- Default all to 1.0f
- Persist via `<SLOT mass="1.5"/>` attribute in toXml()/fromXml()

**Algorithm change in `InterpolationEngine::compute2D()`:**
- Current: `weight[i] = 1.0 / max(distSq, epsilonSq)`
- New: `weight[i] = mass[i] / max(distSq, epsilonSq)`
- Mass is factored into the IDW numerator, cleanly scaling each snapshot's pull

**Thread safety:**
- Masses are read lock-free on the audio thread (snapshot seqlock covers them)
- Written on the message/MCP thread under writeLock_

**UI:**
- 12 small sliders (or a ring-drag UI) in the snapshot-editing panel
- OR: shift-drag on a snapshot node in MorphPad to adjust its mass
- APVTS parameters: `snapshotMass1` through `snapshotMass12` (12 float params)

**APVTS integration:**
- 12 new NormalisableRange(0.1f, 3.0f, 0.05f) parameters
- Synced via `syncStateFromAPVTS()` → `SnapshotBank::setMass()`

**Persistence:**
- `toXml()` adds `mass` attribute per SLOT element
- `fromXml()` restores it (default 1.0 for backward compat)

### Files to modify
| File | Change |
|------|--------|
| `src/Core/ParameterState.h` | Add `float mass = 1.0f` field |
| `src/Core/SnapshotBank.h` | Add `setMass()/getMass()`, update toXml/fromXml |
| `src/Core/InterpolationEngine.cpp` | Multiply IDW weight by slot mass in compute2D |
| `src/Plugin/PluginProcessor.cpp` | Add 12 APVTS params, syncStateFromAPVTS |
| `src/Plugin/PluginProcessor.h` | Add snapshot mass accessors |
| `src/UI/MorphPad.cpp` | Shift-drag on snapshot node → adjust mass |
| `src/Core/MorphProcessor.cpp` | Pass mass array to compute2D (or read from bank) |
| `tests/Unit/TestGravityWells.cpp` | Unit tests for weighted blend |

---

## Phase 2: NNI / Voronoi Morphing Engine

**Effort:** High | **Impact:** High | **Risk:** Medium (algorithmic correctness)

### What
Replace (or add option for) Natural Neighbor Interpolation instead of IDW.
NNI uses Voronoi tessellation: only the snapshots whose Voronoi cells are adjacent
to the cursor position contribute to the blend. Eliminates distant-snapshot bleed.

### Algorithm

1. Build Delaunay triangulation of the 2-12 occupied snapshot positions (clock layout)
2. For the cursor point P, compute its Voronoi cell within the triangulation
3. Compute the area stolen from each natural neighbor's original Voronoi cell
4. Normalize these stolen areas → interpolation weights

**Delaunay triangulation:** Use Bowyer-Watson incremental algorithm.
Since we have at most 12 points, O(n²) is fine. Pre-compute on snapshot changes.

**Voronoi area computation:** Sibson's method:
- For cursor P inserted into Delaunay triangulation:
  - Each neighbor's Voronoi cell area is reduced
  - The "stolen" area = weight for that neighbor

### Design

**New class: `VoronoiMorphEngine`** (in `src/Core/`)
- `rebuildTriangulation(positions, occupiedMask)` — called when snapshots change
- `computeWeights(cursorX, cursorY, masses, weights, totalWeight)` — per-block

**Integration into InterpolationEngine:**
- Add `compute2D_Voronoi()` alongside compute2D
- APVTS parameter: `interpolationMode` (0=IDW, 1=Voronoi/NNI)
- `MorphProcessor::process()` branches on interpolation mode

**SIMD:** The weighted accumulation after weight computation is identical
to the existing SIMD path — reuse the same accumulator code.

### Files to modify/create
| File | Change |
|------|--------|
| `src/Core/VoronoiMorphEngine.h` | NEW — Delaunay + NNI computation |
| `src/Core/VoronoiMorphEngine.cpp` | NEW — Bowyer-Watson, Sibson weights |
| `src/Core/InterpolationEngine.h` | Add compute2D_Voronoi declaration |
| `src/Core/InterpolationEngine.cpp` | Add compute2D_Voronoi implementation |
| `src/Core/MorphProcessor.h` | Add interpolation mode atomic |
| `src/Core/MorphProcessor.cpp` | Branch on interpolation mode |
| `src/Plugin/PluginProcessor.cpp` | Add interpolationMode APVTS param |
| `tests/Unit/TestVoronoiMorph.cpp` | Unit tests |

---

## Phase 3: Voronoi Diagram Visualization

**Effort:** Medium | **Impact:** Medium (visual delight) | **Risk:** Low

### What
Render Voronoi cell boundaries, color-coded zones of influence, and per-snapshot
territory on the MorphPad. Active only when NNI mode is selected.

### Design

**Rendering in `MorphPad::paint()`:**
1. Compute Voronoi cells from occupied snapshot positions (center of pad)
2. For each occupied slot:
   - Fill its Voronoi cell with a translucent hue derived from slot index (12-hue wheel)
   - Draw cell boundaries as dashed lines
3. Show gravity-well-adjusted territory sizes (mass affects visual cell size)
4. Highlight the cell containing the cursor

**Colors:** Use a 12-step HSV wheel (hue = slot_index * 30°) with low alpha (0.08–0.15).

**Performance:** Voronoi cells for ≤12 points can be computed analytically.
Use Fortune's algorithm or a simple half-plane intersection approach.
Recompute only when occupied slots change or on resize.

### Files to modify
| File | Change |
|------|--------|
| `src/UI/MorphPad.cpp` | Add Voronoi cell rendering in paint() |
| `src/UI/MorphPad.h` | Add cached Voronoi polygon storage |
| `src/Core/VoronoiMorphEngine.h` | Expose cell polygon computation for UI |

---

## Phase 4: Waypoints Motion Path System

**Effort:** High | **Impact:** High | **Risk:** Medium (state management)

### What
Automated cursor motion paths: free-draw recording, point-to-point (P2P) paths,
and BPM-synced loop playback. Replaces manual cursor + drift as the only motion options.

### Design

**New class: `WaypointEngine`** (in `src/Core/`)
- `Waypoint` struct: {x, y, timeSec, transitionType}
- `Path` struct: vector<Waypoint>, loopMode, totalDuration
- Three construction modes:
  1. **Free-draw:** Record cursor at ~60 Hz while mouse is held → dense waypoint list
  2. **P2P:** User clicks to place up to 24 nodes, linear interpolation between
  3. **BPM-sync:** Path duration quantized to host tempo (1/4 to 32 bars)

**Playback modes:**
- Forward (one-shot or loop)
- Ping-Pong (forward then reverse)
- Loop

**Integration:**
- `MorphProcessor` owns a `WaypointEngine` instance
- When waypoint mode is active, `process()` calls `waypointEngine_.advance(dt)` to get cursor position
- Cursor then feeds into physics (if enabled) → interpolation
- Waypoint cursor overrides manual XY position

**APVTS parameters:**
- `waypointMode` (0=Off, 1=FreeDraw, 2=P2P, 3=BPM)
- `waypointLoopMode` (0=Forward, 1=PingPong)
- `waypointBpmSync` (0=Off, 1/4, 1/2, 1, 2, 4, 8, 16, 32 bars)
- `waypointPlayState` (0=Stop, 1=Play, 2=Record)

**UI:**
- Waypoint editor overlay on MorphPad (transparent path lines + nodes)
- Transport controls (Play/Stop/Record/Clear) below the pad
- Click to add P2P nodes, drag to move them
- Shift-click to start free-draw recording

**Thread safety:**
- Waypoint paths are written on the message thread
- Audio thread reads current playback position (atomic float)
- Path data uses double-buffer pattern (like discreteMap)

### Files to modify/create
| File | Change |
|------|--------|
| `src/Core/WaypointEngine.h` | NEW |
| `src/Core/WaypointEngine.cpp` | NEW |
| `src/Core/MorphProcessor.h` | Add WaypointEngine member, waypoint mode atomics |
| `src/Core/MorphProcessor.cpp` | Integrate waypoint cursor into process() |
| `src/Plugin/PluginProcessor.cpp` | Add 4+ APVTS params |
| `src/UI/MorphPad.cpp` | Add waypoint overlay rendering, edit interaction |
| `tests/Unit/TestWaypointEngine.cpp` | Unit tests |

---

## Phase 5: Smart Preset Caching

**Effort:** Medium | **Impact:** Medium | **Risk:** Low

### What
When recalling a snapshot that uses the same plugin as the currently loaded one,
bypass full VST3 state-chunk recall and directly apply cached parameter values.
Target: 2ms recall (SnappySnap claims this).

### Design

**Cache key:** Plugin UID (from PluginDescription) + snapshot slot index.

**Cache entry:**
- `std::vector<float> parameterValues` (normalized, from snapshot)
- `juce::MemoryBlock stateChunk` (opaque, only for Full recall mode)
- `juce::String pluginUid`
- `uint32_t lastAccessTimestamp`

**Cache size:** 12 entries (one per slot max), ~100 KB total.

**Flow:**
1. `SnapshotBank::recall(slot, bridge)` checks if current plugin UID matches slot's cached UID
2. If match → fast path: apply cached values directly (no state-chunk round-trip)
3. If no match → normal path: apply values + state chunk + record UID in cache
4. On plugin unload → invalidate cache entries

**Implementation in `PluginHostManager`:**
- Add `getCurrentPluginUid()` accessor
- `SnapshotBank` already has the parameter values; the win comes from skipping `setStateInformation()` on the hosted plugin

### Files to modify
| File | Change |
|------|--------|
| `src/Core/SnapshotBank.h` | Add plugin UID tracking per slot, fast-recall path |
| `src/Host/PluginHostManager.h` | Add getCurrentPluginUid() |
| `src/Host/PluginHostManager.cpp` | Publish UID to SnapshotBank on load |
| `tests/Unit/TestPresetCaching.cpp` | Cache hit/miss tests |

---

## Implementation Order

1. **Phase 1 (Gravity Wells)** — ~3 hours, immediate value, paves way for NNI mass scaling
2. **Phase 2 (NNI/Voronoi)** — ~8 hours, core algorithmic upgrade
3. **Phase 3 (Voronoi Viz)** — ~4 hours, depends on Phase 2
4. **Phase 4 (Waypoints)** — ~10 hours, largest new feature
5. **Phase 5 (Caching)** — ~3 hours, nice optimization

**Total estimate:** ~28 hours

## Design Principles

- **Audio-thread safety:** All new code paths must be noexcept, no allocations after prepare()
- **SIMD:** Reuse existing AVX2/SSE accumulation paths
- **Persistence:** All new state must survive DAW save/restore via toXml/fromXml
- **Backward compat:** Old presets without new fields default to sensible values
- **Tests:** Each phase includes dedicated Catch2 unit tests
