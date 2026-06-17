# Sub-Agent 3 Report: Physics Engine & Interpolation

## Critical Issues (CRASH / NUMERICAL INSTABILITY / INCORRECT DSP)

### Issue 1: Heavy Elastic Preset is Heavily Underdamped — Causes Oscillation and Overshoot
- **Location**: `PhysicsEngine.cpp:30–34`
- **Severity**: Critical
- **Description**: The `Heavy` preset uses `stiffness = 8.0f` and `damping = 0.95f`. The damping ratio is ζ = c / (2√k) ≈ 0.95 / (2 × 2.828) ≈ **0.168**, which is far below critical damping (ζ = 1). The spring will oscillate and overshoot significantly. The focus area explicitly asks to verify that the spring prevents oscillation and overshoot — it does not.
- **Root Cause**: The damping constant was chosen too low relative to the stiffness. For a spring-damper system, critical damping requires c = 2√k ≈ 5.66 for k = 8. The current c = 0.95 is only ~17 % of critical damping.
- **Recommended Fix**: Tune the Heavy preset to be at least critically damped (or slightly overdamped):
```cpp
case ElasticPreset::Heavy:  stiffness = 8.0f; damping = 5.8f; break;  // ζ ≈ 1.02
```
Alternatively, offer two sub-presets: “Heavy Bouncy” (underdamped) and “Heavy Tight” (critically damped).

### Issue 2: Semi-Implicit Euler Integration Adds Energy to the System
- **Location**: `PhysicsEngine.cpp:47–51`
- **Severity**: Critical
- **Description**: The elastic integrator uses semi-implicit Euler (update velocity first, then position with the *new* velocity). For underdamped oscillators, this method is known to **inject energy** on each step, causing the amplitude to grow rather than decay until the velocity clamp kicks in. Combined with Issue 1 (underdamped Heavy), the spring can diverge before the velocity saturation saves it.
- **Root Cause**: Semi-implicit Euler is not symplectic for damped systems; it artificially increases total energy when ζ < 1 and dt is not infinitesimal.
- **Recommended Fix**: Switch to **velocity Verlet** or **explicit-implicit mixed Euler** (symplectic Euler with damping applied as a half-step). A minimal fix for real-time safety:
```cpp
// Symplectic Euler with damping half-step
const float dampingFactor = 1.0f / (1.0f + damping * subDt * 0.5f);
s.vx = (s.vx + stiffness * (targetX - s.x) * subDt) * dampingFactor;
s.vy = (s.vy + stiffness * (targetY - s.y) * subDt) * dampingFactor;
s.x += s.vx * subDt;
s.y += s.vy * subDt;
```
This is still O(1) per step and preserves energy bounds better.

### Issue 3: MorphPad UI Cursor Misplaced When Fader Source + Physics Mode ≥ 2
- **Location**: `MorphPad.cpp:276–329`
- **Severity**: Critical
- **Description**: In `MorphPad::paint()`, the branch `if (physMode >= 2)` is checked **before** `else if (morphSrc == 1)`. When the user is in **Drift** mode (physMode = 2) but switches to **Fader** source (morphSrc = 1), the cursor is drawn using `morph.getProcessedX()` — which for Fader mode is `faderPos` in **[0, 1]** — without mapping to the [-1, 1] screen space. The cursor appears on the **right half of the pad** instead of along the snapshot clock track. The correct branch (`morphSrc == 1`) is never reached.
- **Root Cause**: Incorrect branch ordering in `paint()`. The fader-specific interpolation logic is shadowed by the physics-mode branch.
- **Recommended Fix**: Reorder the branches so that `morphSrc == 1` is checked first, or explicitly map `procX` to [-1, 1] in the physics branch when `morphSrc == 1`:
```cpp
if (morphSrc == 1)
{
    // ... existing fader cursor logic ...
}
else if (physMode >= 2)
{
    float procX = morph.getProcessedX();
    float procY = morph.getProcessedY();
    // procX/Y are in [-1,1] for XY pad, but [0,1] for fader
    if (morphSrc == 1) { procX = procX * 2.0f - 1.0f; }  // map fader [0,1] → [-1,1]
    cx = centre.x + procX * radius;
    cy = centre.y + procY * radius;
}
```

### Issue 4: Perlin Noise Gradient Bias — Only 4 Diagonal Directions
- **Location**: `PhysicsEngine.cpp:104–110`
- **Severity**: Critical
- **Description**: The `grad()` function hashes to **4 diagonal vectors** (`(±1,±1)`) via `hash & 3`. Standard 2D Perlin uses **8 gradient directions** (including axis-aligned). The restricted gradient set causes visible diagonal striping artifacts in the drift pattern, especially at low chaos (1 octave). The drift trajectory is biased toward diagonal motion rather than free 2D wandering.
- **Root Cause**: Simplified gradient hash using only 2 bits instead of 3.
- **Recommended Fix**: Expand to 8 gradients:
```cpp
static float grad(int hash, float x, float y) noexcept
{
    const int h = hash & 7;
    const float u = h < 4 ? x : y;
    const float v = h < 4 ? y : x;
    return ((h & 1) ? -u : u) + ((h & 2) ? -2.0f*v : 2.0f*v);
}
```
(Or use the classic 8-direction table: `(1,1), (-1,1), (1,-1), (-1,-1), (1,0), (-1,0), (0,1), (0,-1)`.)

---

## High-Priority Issues (ALGORITHMIC / PERFORMANCE)

### Issue 5: Smoothing Applied in Direct Mode — Response is Not Instant
- **Location**: `MorphProcessor.cpp:41–67` (specifically lines 66–67)
- **Severity**: High
- **Description**: `MorphProcessor::process()` unconditionally calls `applySmoothing(output)` after interpolation, **even in Direct mode**. With the default `smoothRate_ = 0.95`, the output follows a single-pole IIR with a long decay (~0.5 s per block at 44.1 kHz/512). Users selecting “Direct” expect instant parameter tracking; they get a smoothed glide instead.
- **Root Cause**: Smoothing is architecturally a separate post-process, but it is not bypassed when the user explicitly requests no physics (Direct).
- **Recommended Fix**: Either (a) skip `applySmoothing()` when `mode == MorphMode::Direct && smoothRate_ == 0.0f`, or (b) document that Direct mode still uses the global smoothing rate, and provide a UI toggle to disable smoothing independently.

### Issue 6: Smoothing Time Constant is Not Sample-Rate Independent
- **Location**: `MorphProcessor.cpp:119–189`
- **Severity**: High
- **Description**: The single-pole smoother `smoothed = smoothed * rate + output * (1 - rate)` is applied **once per process block**. The coefficient `rate` is a fixed atomic value, never compensated for `dt`. At 96 kHz (≈ 2× the block rate of 48 kHz), the smoother updates twice as often in wall-clock time, so it converges ~2× faster. A project saved at 44.1 kHz will morph with a different perceptual speed when opened at 96 kHz.
- **Root Cause**: The smoothing coefficient is a dimensionless per-block constant, not a time-constant such as `exp(-dt / τ)`.
- **Recommended Fix**: Convert the rate to a time-constant form:
```cpp
const float tau = 1.0f / (1.0f - rate);  // approximate time constant in blocks
const float blockRate = std::exp(-dt / (tau * referenceDt));  // dt-compensated
```
Or simply compute `rate` from a desired cutoff frequency in Hz:
```cpp
const float freqHz = 10.0f;  // user-tunable cutoff
const float rate = std::exp(-2.0f * juce::MathConstants<float>::pi * freqHz * dt);
```

### Issue 7: Drift Time Accumulator — Float Precision Degradation Over Long Sessions
- **Location**: `MorphProcessor.cpp:105` (driftTime_ member)
- **Severity**: High
- **Description**: `driftTime_ += dt` every block. `dt` is ~0.0116 s at 44.1 kHz/512. After **24 hours**, `driftTime_ ≈ 86,400`. At this magnitude, a `float` has a precision of ~0.005 s. After **10 days**, precision drops to ~0.05 s, which is **larger than `dt` itself**. The accumulator may fail to increment, freezing the Perlin phase and making the drift pattern static.
- **Root Cause**: Unbounded accumulation of a `float` time variable.
- **Recommended Fix**: Wrap `driftTime_` modulo the Perlin period (256) to keep it small:
```cpp
driftTime_ += dt;
if (driftTime_ > 256.0f) driftTime_ -= 256.0f;  // Perlin period = 256
```
Or use `double` for `driftTime_` (64-bit is fine for a single scalar on the audio thread).

### Issue 8: Elastic Position is Not Clamped During Sub-Step Integration
- **Location**: `PhysicsEngine.cpp:41–52`
- **Severity**: High
- **Description**: The adaptive sub-step loop updates `s.x` and `s.y` without any bounds check. If the host pauses the audio engine for a long time (e.g., debugger break, DAW transport stop), `dt` can be large (seconds). Although `numSteps` increases, the per-sub-step position change can still overshoot the `[-1,1]` target range. The clamp is only applied **after** the loop in `MorphProcessor::updatePhysics()`. During the loop, the spring might compute extreme positions that push `grad()` or other downstream code into unusual states (though no direct crash path exists).
- **Root Cause**: Missing per-step position clamping inside the integration loop.
- **Recommended Fix**: Clamp position inside the loop to prevent excursion:
```cpp
s.x  += s.vx * subDt;
s.y  += s.vy * subDt;
s.x = std::clamp(s.x, -1.0f, 1.0f);
s.y = std::clamp(s.y, -1.0f, 1.0f);
```
(Note: clamping position changes the physics slightly, but for UI spring-back it is acceptable.)

### Issue 9: GeneticEngine::breed() Calls juce::Logger::writeToLog — Risk of Audio-Thread Allocation
- **Location**: `GeneticEngine.cpp:21–24`
- **Severity**: High
- **Description**: When `parentA.parameterCount != parentB.parameterCount`, `breed()` calls `juce::Logger::writeToLog()`. JUCE’s default logger may allocate strings or lock a file stream. If `breed()` is ever triggered from the audio thread (e.g., via MCP automation or MIDI-triggered breeding), this violates real-time safety and can cause dropouts.
- **Root Cause**: Logging inside a core DSP-adjacent algorithm.
- **Recommended Fix**: Remove the logger call from `breed()`. If mismatch detection is needed, set a flag or return a status enum; let the caller decide how to log.

### Issue 10: setDiscreteMap(std::vector<bool>&&) Copies Instead of Moving
- **Location**: `MorphProcessor.h:65–68`
- **Severity**: High
- **Description**: The rvalue-reference overload forwards to the lvalue-reference overload: `setDiscreteMap(map)`. Because `map` has a name inside the function body, it is an lvalue. The bit-packed `std::vector<bool>` is copied element-by-element into the double buffer instead of being moved.
- **Root Cause**: Missing `std::forward` or explicit move.
- **Recommended Fix**:
```cpp
void setDiscreteMap(std::vector<bool>&& map)
{
    setDiscreteMap(static_cast<const std::vector<bool>&>(map));  // still copies
    // Better: refactor to a private implementation that accepts by value:
    // void setDiscreteMapImpl(std::vector<bool> map);  // move into here
}
```

---

## Medium-Priority Issues (EDGE CASE / ROBUSTNESS)

### Issue 11: compute2D Leaves output Unchanged When No Snapshots Are Occupied
- **Location**: `InterpolationEngine.cpp:336–338`
- **Severity**: Medium
- **Description**: If `totalWeight < kEpsilon` (no occupied snapshots), `compute2D` returns early without writing to `output`. If the caller passes an uninitialized or stale buffer, garbage values may be forwarded to the hosted plugin. The `computeWithRetry` fallback also leaves `output` untouched.
- **Root Cause**: Missing zero-fill fallback.
- **Recommended Fix**: Fill with a safe default before returning:
```cpp
if (totalWeight < kEpsilon)
{
    std::fill(output.begin(), output.end(), 0.5f);
    return;
}
```

### Issue 12: Perlin static_cast<int> Overflow for Large Coordinate Values
- **Location**: `PhysicsEngine.cpp:117–118`
- **Severity**: Medium
- **Description**: `static_cast<int>(std::floor(x))` overflows `int` (undefined behavior) if `x > INT_MAX` (≈ 2.1×10⁹). While `driftTime_` is bounded by session length, `perlin()` is a `public static` function and could be called with arbitrary inputs from future code or tests.
- **Root Cause**: Unsafe float-to-int conversion without range check.
- **Recommended Fix**: Wrap or clamp `x` before casting:
```cpp
const float fx = std::floor(x);
const int xi = (static_cast<int64_t>(fx) & 255);  // use 64-bit if fx may be large
```
Or use `fmodf(x, 256.0f)` before the floor.

### Issue 13: Fader Mode Audio Trail and UI Trail Are Inconsistent
- **Location**: `MorphProcessor.cpp:62–63` and `MorphPad.cpp:76–108`
- **Severity**: Medium
- **Description**: In Fader mode, the audio thread stores `processedX_ = faderPos` (range [0,1]) in the trail buffer, mapped to `(faderPos + 1) * 0.5f`. The UI `timerCallback()` computes a cursor position along the **clock positions** (e.g., interpolating between snapshot 1 at 12-o’clock and snapshot 2 at 1-o’clock). The painted trail (read from `morph.getTrail()`) therefore draws a horizontal line near the center of the pad, while the cursor dot travels along the clock track. The two visual elements do not coincide.
- **Root Cause**: The audio trail is written in fader-normalized space, while the UI cursor is rendered in interpolated-clock space.
- **Recommended Fix**: In Fader mode, either (a) disable the audio trail and let the UI timer draw its own trail, or (b) compute the audio-thread trail position using the same clock-position interpolation that the UI uses.

### Issue 14: Snapshot Addition/Removal During Active Morph Causes Parameter Jumps
- **Location**: `InterpolationEngine.cpp:248–277`
- **Severity**: Medium
- **Description**: If the user captures a new snapshot while the fader is at a fixed position, `occupiedCount` increases. The mapping `scaled = faderPos * (occupiedCount - 1)` changes discontinuously. A fader at 0.5 with 2 snapshots interpolates between slots 0 and 1. After adding a third snapshot, the same 0.5 fader interpolates between slots 1 and 2. The output jumps.
- **Root Cause**: 1D interpolation uses the count of occupied snapshots as the denominator; it is not normalized to the original slot indices.
- **Recommended Fix**: Optional, but to prevent jumps, the fader should map to **slot indices** (0–11) rather than occupied-index ordinals. For example, if only slots 0 and 11 are occupied, the fader 0→1 should interpolate between those two, and inserting a snapshot at slot 5 should not affect the 0→11 range unless the fader moves.

### Issue 15: DiscreteParameterHandler Sophisticated Logic is Not Wired Into the Morph Pipeline
- **Location**: `MorphProcessor.cpp` (no usage of `DiscreteParameterHandler`)
- **Severity**: Medium
- **Description**: `DiscreteParameterHandler` implements HardSwitch, Crossfade, Stepwise, HoldSource, and HoldTarget strategies with cooldown and hysteresis — but `MorphProcessor` never instantiates or calls it. The only discrete handling in `MorphProcessor` is `applyListenFilter()`, which sets `SKIP_SENTINEL`. The full discrete-handling subsystem is effectively dead code in the audio path.
- **Root Cause**: Missing integration step between the discrete handler and the morph processor.
- **Recommended Fix**: Add a `DiscreteParameterHandler` member to `MorphProcessor`, initialize it in `prepare()`, and call `processDiscreteParameters()` after `applySmoothing()` (or before `applyListenFilter()`).

### Issue 16: Crossfade Strategy Allows Non-Step Values for Discrete Parameters
- **Location**: `DiscreteParameterHandler.cpp:125–139`
- **Severity**: Medium
- **Description**: The `Crossfade` strategy directly assigns `state.currentValue = interpolatedValue` (e.g., 0.37) when `morphAmount` is in (0.1, 0.9). Many hosted plugins expect discrete parameters to be exact multiples of `1 / (steps - 1)`. Passing 0.37 may cause the plugin to round it to the nearest step, producing an audible click that the crossfade was meant to avoid.
- **Root Cause**: No quantization to valid steps in the Crossfade path.
- **Recommended Fix**: Quantize the interpolated value to the nearest valid step before assignment:
```cpp
state.currentValue = stepToValue(index, valueToStep(index, interpolatedValue));
```

### Issue 17: MorphProcessor::prepare() Does Not Reset smoothedValues_
- **Location**: `MorphProcessor.cpp:25–39`
- **Severity**: Medium
- **Description**: `prepare()` resizes `smoothedValues_` but does not fill it with zeros or the current snapshot values. If the user changes the hosted plugin (which triggers a new `prepare()` with a different parameter count), the old smoothing state persists. The first few audio blocks will glide from the **previous plugin’s** parameter values to the new ones, causing audible artifacts.
- **Root Cause**: Missing `std::fill(smoothedValues_.begin(), smoothedValues_.end(), 0.0f);` (or current snapshot value) in `prepare()`.
- **Recommended Fix**: Add `std::fill(smoothedValues_.begin(), smoothedValues_.end(), 0.0f);` after resize.

### Issue 18: getClockPositions Non-Unit Radius Returns Reference to Mutable thread_local
- **Location**: `InterpolationEngine.cpp:199–204`
- **Severity**: Medium
- **Description**: When `radius != 1.0f`, the function returns a reference to a `static thread_local` array. If a caller caches the reference and later calls `getClockPositions()` with a different radius, the cached reference silently points to the new scaled data. Current callers (`MorphPad`, `compute2D`) make copies because `auto` drops the reference, but this is a fragile API contract.
- **Root Cause**: Returning a reference to a mutable static buffer.
- **Recommended Fix**: Return by value for non-unit radius, or document that the reference is only valid until the next call. Better yet, always return by value; the compiler will elide the copy (NRVO) for the 96-byte array.

---

## Low-Priority Issues (STYLE / DOCUMENTATION / TESTS)

### Issue 19: No Crossover Ratio Clamping in GeneticEngine::breed
- **Location**: `GeneticEngine.cpp:36–37`
- **Severity**: Low
- **Description**: `crossoverRatio` is not clamped to [0,1]. Values outside this range produce an extrapolation rather than interpolation. While `jlimit(0,1, blended + mutation)` clamps the final output, the blend is not a convex combination and can produce counter-intuitive offspring.
- **Recommended Fix**: `crossoverRatio = std::clamp(crossoverRatio, 0.0f, 1.0f);` at the top of the function.

### Issue 20: Test Endpoint Exactness Uses Overly Generous Margin
- **Location**: `TestDSPQuality.cpp:418–419`
- **Severity**: Low
- **Description**: The endpoint test for `compute1D` at `t = 0.0` and `t = 1.0` allows `margin(0.01f)`. For exact interpolation at `t = 0.0`, the output should be **bit-exact** to `parentA` (multiplying by `1.0f` is exact for IEEE-754). A 0.01 margin masks potential bugs in the SIMD tail loop or buffer overruns.
- **Recommended Fix**: Tighten the margin to `1e-5f` or use `== Approx(vA[p]).margin(0.0f)` and verify that the scalar fallback matches exactly.

### Issue 21: SnapFader Timer Runs at 15 Hz Even When Hidden
- **Location**: `SnapFader.cpp:12`
- **Severity**: Low
- **Description**: `startTimerHz(15)` creates a continuous timer callback. If the fader component is not visible (e.g., user switched to a different tab), it still polls for external state changes and potentially repaints.
- **Recommended Fix**: Start the timer only when `isVisible()` and stop it in `visibilityChanged()`.

### Issue 22: Missing Test for Drift Determinism / Seeding
- **Location**: `TestPhysicsAndGenetic.cpp` (drift tests)
- **Severity**: Low
- **Description**: The drift tests verify that Free mode produces non-zero output and Locked mode stays near the anchor, but there is no test that (a) the drift pattern is deterministic across runs, (b) the drift pattern is independent of wall-clock time (i.e., the same `time` always yields the same output), or (c) the noise can be re-seeded for variation. Since there is no seeding API, this is a latent feature gap.
- **Recommended Fix**: Add a test `updateDrift: same time/speed/distance produces identical output` and consider adding a `setSeed()` API to the `PhysicsEngine` for user-requested variation.

---

## Positive Findings (What is Done Well)

1. **Adaptive Sub-Stepping for Elastic Stability** (`PhysicsEngine.cpp:36–39`) — The `maxStableDt` calculation and `numSteps` subdivision is a solid real-time safety guard. It correctly prevents numerical blow-up when the host delivers large blocks or low sample rates.

2. **Seqlock Retry Pattern in SnapshotBank** (`SnapshotBank.h:260–307`) — The lock-free read with bounded retries (`MAX_READ_RETRIES = 128`) and explicit `atomic_thread_fence` on both sides is a well-executed seqlock. It correctly handles weakly-ordered CPUs (ARM/Apple Silicon) and provides a glitch-free fallback (hold previous frame) on exhaustion.

3. **SIMD Interpolation with Unaligned Loads and Scalar Tail** (`InterpolationEngine.cpp:119–173`) — The AVX2 and SSE2 paths use `_mm256_loadu_ps` / `_mm_loadu_ps` (safe for unaligned vectors) and always fall back to a scalar loop for the remainder. This is exactly how real-time SIMD should be written.

4. **Skip Sentinel for Discrete Parameters** (`MorphProcessor.h:71`) — Using `-1.0f` as `SKIP_SENTINEL` is safe because all normalized parameter values are in `[0,1]`. The caller (`ParameterBridge`) can detect this and skip the parameter write, preventing discrete clicks during morph.

5. **Double-Buffer for Discrete Mask** (`MorphProcessor.h:53–68`) — The atomic index flip (`discreteActiveIndex_`) provides a truly lock-free handoff of the discrete-parameter map from the UI/MCP thread to the audio thread. No `std::shared_ptr` refcount contention, no mutex.

6. **Test Coverage for Sample-Rate Independence** (`TestPhysicsAndGenetic.cpp:90–118`) — The `H-2` regression test runs the spring at 44.1 kHz, 96 kHz, and a fine-step reference, verifying that the wall-clock settling time matches within 0.03. This is a strong signal-quality guard.

7. **Compute1D Handles Non-Contiguous Occupied Slots Correctly** (`InterpolationEngine.cpp:248–277`) — The `occupiedSlots` array correctly maps `faderPos` across whatever slots are occupied, in index order. Endpoint behavior (`faderPos = 0` and `1`) returns exact snapshot values, and the interpolation is continuous across slot boundaries.

8. **SanityConfig Protection is Correct and Well-Tested** (`GeneticEngine.cpp:29–34`, `TestPhysicsAndGenetic.cpp:338–403`) — Both `breed()` and `smartRandomize()` correctly skip `protectedIndices`. The tests verify that protected params retain parentA’s value and that `sanity.enabled = false` disables the guard entirely.

9. **Perlin Octave Limit Prevents Aliasing** (`PhysicsEngine.cpp:157`) — The M-7 fix caps octaves at 4, which is the correct Nyquist-aware limit for typical audio-rate block processing. This prevents high-frequency Perlin gradients from aliasing.

10. **Velocity Saturation and NaN/Inf Recovery** (`PhysicsEngine.cpp:55–59`) — The `std::clamp` to `±10.0f` and the `std::isfinite` fallback (reset to target) are good defensive measures against extreme `dt` values (e.g., transport pause/resume).
