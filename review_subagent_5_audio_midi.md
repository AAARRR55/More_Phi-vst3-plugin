# Sub-Agent 5 Report: Audio I/O, MIDI & Features

## Executive Summary

This audit covers the More-Phi audio I/O pipeline, MIDI routing, modulation system, and all advanced morph engines (Granular, Spectral, Formant, VAE, A/B Compare). The codebase demonstrates strong real-time safety practices (zero-allocation audio paths, seqlocks, SPSC queues, xorshift32 PRNGs). However, three **critical** issues were found: a formant engine output buffer under-allocation that causes dropouts for large block sizes, a spectral engine transient-detection bug that breaks stereo coherence, and unit-test files that test self-contained mocks instead of the actual production implementations. Several high-priority DSP and architectural issues were also identified.

---

## Critical Issues (CRASH / AUDIO CORRUPTION / INCORRECT DSP)

### Issue 1: FormantMorphEngine Output Buffer Under-Allocation Causes Dropouts
- **Location**: `FormantMorphEngine.cpp:74`, `FormantMorphEngine.cpp:215-226`
- **Severity**: Critical
- **Description**: `prepare()` sizes the per-channel `outputBuffer` to `fftSize_ + hopSize_` (2560 samples for the default 2048/512 configuration) and completely ignores the `maxBlockSize` parameter. The `processBlock()` drain uses `i % (fftSize_ + hopSize_)` to read from this buffer. When the host delivers a block larger than 2560 samples, the modulo wraps and re-reads already-consumed (or zeroed) positions, causing an audible dropout of `numSamples - 2560` samples. The shift-left logic at the end of the block also only shifts by `std::min(numSamples, outBufLen)`, so the tail of the block is never filled with valid overlap-add data.
- **Root Cause**: The `prepare()` method signature accepts `maxBlockSize` but the parameter is unused (commented as `/*maxBlockSize*/`). The output buffer is sized as if `numSamples` can never exceed `fftSize_ + hopSize_`, which is an invalid assumption for hosts that deliver large blocks (e.g., 4096 samples).
- **Recommended Fix**: Size the output buffer to `fftSize_ + std::max(hopSize_, maxBlockSize)`, mirroring `SpectralMorphEngine::prepare()`. Remove the modulo indexing in `processBlock()` and read linearly (`outputBuffer[i]`), since the shift-left logic already treats the buffer as a flat delay line.

```cpp
// In prepare()
ch.outputBuffer.assign(static_cast<size_t>(fftSize_ + std::max(hopSize_, maxBlockSize)), 0.0f);

// In processBlock()
outPtr[i] = ch.outputBuffer[i];  // Remove modulo
```

### Issue 2: SpectralMorphEngine Transient Detection Only Affects Channel 0
- **Location**: `SpectralMorphEngine.cpp:242-247`
- **Severity**: Critical
- **Description**: `processBlock()` only calls `transientDetector_.process()` when `c == 0` (left channel). The `effectiveAlpha` computed from the detector is applied to the left channel's `processFrame()`, but the right channel continues to use the unmodified `alpha`. During a transient, the left channel snaps to `hardSwitch` while the right channel interpolates normally. This destroys stereo coherence and can collapse the stereo image or cause comb-filtering artifacts.
- **Root Cause**: The per-channel loop computes `effectiveAlpha` locally and applies it per-channel. The transient detector state should drive the alpha for **both** channels to preserve stereo phase relationships.
- **Recommended Fix**: Compute `effectiveAlpha` once per block (outside the channel loop) after the first channel's hop is processed, or run the detector on a mono mix of both channels and apply the same `effectiveAlpha` to all channels.

```cpp
float effectiveAlpha = alpha;
if (transientPreserve_.load(std::memory_order_relaxed))
    effectiveAlpha = transientDetector_.process(ch0.magA.data(), numBins_, alpha);

for (int c = 0; c < numChannels; ++c)
{
    // ... use effectiveAlpha for every channel
    processFrame(channels_[c], effectiveAlpha);
}
```

### Issue 3: Unit Tests for Granular/Spectral/Formant Engines Test Mocks, Not Production Code
- **Location**: `tests/Unit/TestGranularEngine.cpp`, `tests/Unit/TestSpectralEngine.cpp`
- **Severity**: Critical (Test Coverage Gap)
- **Description**: Both test files contain self-contained `GranularMorphEngine`, `SpectralMorphEngine`, `FormantMorphEngine`, and `TransientDetector` classes **inside the test translation unit** (in the `more_phi::test` namespace). They do not `#include` the production headers (`src/Core/GranularMorphEngine.h`, etc.). The tests exercise these mocks, not the actual production implementations. This means the production engines are completely untested by the unit test suite, and regressions in the real code will never be caught.
- **Root Cause**: The test files were written as API-contract tests before the production implementations existed, but they were never updated to link against the real classes.
- **Recommended Fix**: Replace the mock classes with `#include` of the production headers and instantiate the real engines. The test helpers (e.g., `fillSine`, `rms`) can remain. Update any API mismatches and add tests that exercise the actual `prepare()`/`processBlock()` code paths.

---

## High-Priority Issues (DSP / MIDI / ARCHITECTURAL)

### Issue 4: ModulationMatrix Clamps Per-Route Instead of Per-Destination
- **Location**: `ModulationMatrix.cpp:104-105`
- **Severity**: High
- **Description**: In `apply()`, the output is clamped after each route is accumulated: `output[dest] = clamp(output[dest] + delta, 0, 1)`. If two or more routes target the same parameter, the first route may saturate to 0 or 1, and the second route's contribution is lost. This makes the modulation result order-dependent and causes silent saturation when multiple LFOs/envelopes modulate the same target.
- **Root Cause**: The clamp is applied inside the per-route loop rather than after all routes to a destination have been accumulated.
- **Recommended Fix**: Accumulate into a local `std::array<float, MAX_ROUTES>` or reuse the `output` vector with a two-pass algorithm: first accumulate all deltas, then clamp once per destination.

### Issue 5: MIDIRouter Sidechain Envelope Coefficients Are Block-Size Dependent
- **Location**: `MIDIRouter.h:80-81`, `MIDIRouter.cpp:121-124`
- **Severity**: High
- **Description**: `scAttackCoeff_ = 0.5f` and `scReleaseCoeff_ = 0.9f` are applied per-block as `sidechainEnvelope_ += coeff * (rms - sidechainEnvelope_)`. Because this is a one-pole IIR updated once per block, the actual time constant in seconds depends on the block size and sample rate. A 128-sample block at 48kHz updates ~375 times per second, while a 1024-sample block updates only ~47 times per second. The envelope is therefore ~8x slower at 1024 samples, causing missed transients or delayed triggering.
- **Root Cause**: The coefficients are hard-coded constants rather than being derived from the sample rate and block size.
- **Recommended Fix**: Compute the coefficient per-block from the desired time constant in seconds:

```cpp
const float dt = static_cast<float>(numSamples) / sampleRate;
const float attackTime  = 0.001f; // 1 ms
const float releaseTime = 0.010f; // 10 ms
const float scAttackCoeff  = 1.0f - std::exp(-dt / attackTime);
const float scReleaseCoeff = 1.0f - std::exp(-dt / releaseTime);
```

### Issue 6: GranularMorphEngine Additive Mixing Can Cause Amplitude Buildup
- **Location**: `GranularMorphEngine.cpp:209-218`
- **Severity**: High
- **Description**: The granular engine adds its output to `bufA` using `+=`. When both the spectral and granular engines are active, `bufA` already contains the spectral-morphed audio (or the hosted plugin's output), and the grain cloud is summed on top without any gain compensation. With high grain density (up to 100 grains/sec) and unit-amplitude inputs, the grain cloud can peak at 10–20x the input level. The comment states "Callers should apply a makeup gain proportional to 1/sqrt(density)", but no such compensation is applied in `PluginProcessor::processBlock()`.
- **Root Cause**: The engine is designed as an additive layer but the hybrid blend weights in `PluginProcessor` are not normalized, and there is no automatic gain control on the grain output.
- **Recommended Fix**: Add a `1.0f / std::sqrt(activeGrainCount * 0.5f + 1.0f)` normalization factor in `renderGrains()`, or clamp the output in `processBlock()` before the `+=` mix.

### Issue 7: reconfigureAudioDomainProcessing() Busy-Waits on Message Thread
- **Location**: `PluginProcessor.cpp:2019-2020`
- **Severity**: High
- **Description**: `reconfigureAudioDomainProcessing()` spins on an atomic counter with `while (audioDomainUsers_.load(...) > 0) juce::Thread::yield();`. This is called from the message-thread timer callback. On systems with audio-thread priority inversion or under heavy load, the message thread can spin for an unbounded amount of time, freezing the UI. A yield-loop is not a robust synchronization primitive.
- **Root Cause**: No blocking semaphore or condition variable is used; the message thread polls the audio thread's activity flag.
- **Recommended Fix**: Use a `juce::WaitableEvent` or a counting semaphore. The audio thread signals the event when it decrements `audioDomainUsers_` to zero.

### Issue 8: Output Gain Parameter Lacks Smoothing
- **Location**: `PluginProcessor.cpp:1537-1543`
- **Severity**: High
- **Description**: `outputGain` is applied as a single scalar multiplication per block using `juce::Decibels::decibelsToGain(gainDb)`. If the user or automation changes the gain rapidly, the gain can jump discontinuously between blocks, causing zipper noise (audible clicks at block boundaries).
- **Root Cause**: The gain is applied block-by-block without a one-pole smoothing filter.
- **Recommended Fix**: Add a `std::atomic<float> smoothedGain_` and a per-sample or per-block linear ramp toward the target gain. A 5–10 ms ramp is sufficient for smooth transitions.

---

## Medium-Priority Issues (EDGE CASE / ROBUSTNESS)

### Issue 9: MacroKnobStrip Hardcoded to First 8 Parameters
- **Location**: `MacroKnobStrip.cpp:19-27`, `MacroKnobStrip.cpp:73-82`
- **Severity**: Medium
- **Description**: The 8 macro knobs always map to parameter indices 0–7 of the hosted plugin. There is no UI or API to map a macro to a different parameter index. For plugins where critical parameters are not in the first 8 slots, the macro knobs are useless.
- **Root Cause**: The implementation is a minimal MVP with no parameter-mapping layer.
- **Recommended Fix**: Add a `macroMappings_[8]` array that stores the target parameter index for each knob, with a right-click or drag-and-drop UI to assign parameters.

### Issue 10: FormantMorphEngine prepare() Ignores maxBlockSize Parameter
- **Location**: `FormantMorphEngine.cpp:59`
- **Severity**: Medium
- **Description**: The `prepare()` signature is `void prepare(double sampleRate, int /*maxBlockSize*/)` — the parameter is commented out. This is a direct contributor to Issue 1 (buffer under-allocation). Even if the output buffer is resized, the circular input buffer and other scratch vectors are also sized without considering the actual block size the host will deliver.
- **Root Cause**: Oversight during implementation; the parameter was added to the interface but not wired through.
- **Recommended Fix**: Remove the comment and use `maxBlockSize` to size the output buffer and any other block-dependent scratch space.

### Issue 11: StepSequencer Smoothing Time Constant Varies with Block Size
- **Location**: `StepSequencer.cpp:195-196`
- **Severity**: Medium
- **Description**: The smoothing IIR coefficient is `localSmoothing * 0.99f`, applied once per `process()` call (i.e., once per block). A 512-sample block at 48kHz calls `process()` ~94 times per second; a 1024-sample block calls it ~47 times per second. The resulting low-pass cutoff is therefore ~2x lower for larger blocks, making the step transitions sound slower and more sluggish.
- **Root Cause**: The coefficient is a constant per-block value, not a sample-rate-normalized time constant.
- **Recommended Fix**: Compute the coefficient from the desired time constant (e.g., 10 ms) and the block duration: `coeff = std::exp(-dt_block / tau)`.

### Issue 12: MIDIRouter Dropped-Event Counter Misses Channel-Filtered Drops
- **Location**: `MIDIRouter.cpp:33-44`
- **Severity**: Medium
- **Description**: When the preallocated MIDI buffer is full and a channel-filtered event arrives (i.e., an event on a different channel than the configured filter), the event is silently dropped with `continue`, but `droppedEventCount_` is **not** incremented. The counter only increments for non-consumed events in the main `if (!consumed)` branch. This makes the diagnostic counter unreliable for detecting MIDI overflow when channel filtering is active.
- **Root Cause**: The `continue` statement in the channel-filter branch skips the drop-counter logic.
- **Recommended Fix**: Add `else { droppedEventCount_.fetch_add(1, ...); }` inside the channel-filter branch when the buffer is full.

### Issue 13: LFO / StepSequencer MIDI Velocity Persists Across Note-Off
- **Location**: `ModulationEngine.cpp:98-100`
- **Severity**: Medium
- **Description**: `processMIDI()` sets `midiVelocity_` on `isNoteOn()` but never resets it on `isNoteOff()`. The velocity source in the modulation matrix therefore holds the last note-on velocity indefinitely, even when no notes are playing. This is unconventional for a MIDI velocity modulation source.
- **Root Cause**: By design ("last note" behavior), but not documented in the public API or user manual.
- **Recommended Fix**: Either reset `midiVelocity_` to 0.0f on note-off, or rename the source to `LastNoteVelocity` and document the behavior.

### Issue 14: Parameter Application Skipped Entire Block if Touch-State Lock Contended
- **Location**: `PluginProcessor.cpp:1321-1399`
- **Severity**: Medium
- **Description**: If `touchStateLock_.tryEnter()` fails (because the message thread is holding it during a snapshot recall), the entire parameter-application loop is skipped for that block. The hosted plugin retains its parameters from the previous block. Under heavy UI load, this can cause parameter update stuttering.
- **Root Cause**: The lock guards both the `lastApplied_`/`touchCooldown_` reads and the actual `paramBridge.setParameterNormalized()` calls. The parameter write itself does not need the lock.
- **Recommended Fix**: Separate the lock into two scopes: acquire the touch lock only for the touch-detection reads, but always apply the morph values regardless of lock contention.

---

## Low-Priority Issues (STYLE / DOCUMENTATION)

### Issue 15: ABCompareEngine Has Typo and Weak LRA Logic
- **Location**: `ABCompareEngine.cpp:68-72`
- **Severity**: Low
- **Description**: The variable `lfusCandidateDev` is misspelled (should be `lufsCandidateDev`). More importantly, the LRA comparison `candidate.lra < baseline_.lra - 1.0f` unconditionally prefers higher LRA, which is not a universal mastering goal. Some genres prefer lower LRA.
- **Root Cause**: Hardcoded preference without user-configurable target.
- **Recommended Fix**: Add a `targetLRA` field to `Metrics` and compare absolute deviation from target.

### Issue 16: ABCompareEngine spectralScore Always Zero in readCurrentMetrics()
- **Location**: `ABCompareEngine.cpp:48`
- **Severity**: Low
- **Description**: `readCurrentMetrics()` sets `m.spectralScore = 0.f` with a comment that it is populated elsewhere. But the `SpectralBalanceAnalyser` integration is not present in the assigned files. This means the spectral score comparison in `compareAndDecide()` never triggers, making the "worse on ≥2 metrics" logic effectively a two-metric check (LUFS + LRA).
- **Root Cause**: Incomplete integration of spectral analysis into the A/B compare path.
- **Recommended Fix**: Either integrate the spectral analyzer or remove the spectral score comparison and document the limitation.

### Issue 17: Misleading Comment on MidiBuffer Pre-Allocation
- **Location**: `PluginProcessor.h:496`
- **Severity**: Low
- **Description**: The comment says "Pre-allocated MIDI buffer to avoid per-block heap allocation", but `juce::MidiBuffer` has no `reserve()` API. It merely retains capacity after `clear()` via its internal `HeapBlock`. The comment implies explicit pre-allocation, which is not what happens.
- **Root Cause**: Informal comment that conflates "retains capacity" with "pre-allocated".
- **Recommended Fix**: Change the comment to "Re-used MIDI buffer — retains capacity across clear() calls".

### Issue 18: MIDIRouter prepare() Parameter Is Unused
- **Location**: `MIDIRouter.cpp:10-15`
- **Severity**: Low
- **Description**: `prepare(int /*expectedMidiEventsPerBlock*/)` accepts a parameter but does not use it. The preallocated storage is always `MAX_MIDI_EVENTS = 256`. The caller could pass 128, but the storage is fixed.
- **Root Cause**: API design mismatch.
- **Recommended Fix**: Either use the parameter to set `MAX_MIDI_EVENTS` dynamically (e.g., `std::max(expected, 256)`) or remove the parameter from the signature.

### Issue 19: VAEMorphEngine Is a Stub with No ONNX Integration
- **Location**: `VAEMorphEngine.cpp:42-82`
- **Severity**: Low (Documented)
- **Description**: `loadModel()` returns `true` but sets `backendMode_ = Stub`. `encode()` and `decode()` trigger `jassertfalse` and return empty vectors. The class is fully documented as a V2 MVP stub, but the assertion can crash debug builds if a UI path accidentally calls encode/decode.
- **Root Cause**: Placeholder implementation awaiting ONNX Runtime integration.
- **Recommended Fix**: Remove `jassertfalse` from the stub paths; return empty vectors gracefully in all builds. The debug assert is hostile to exploratory UI usage.

---

## Positive Findings (What Is Done Well)

1. **Zero-Allocation Audio Path**: All engines (Granular, Spectral, Formant) use pre-allocated `std::vector` and `std::array` buffers. No heap allocations occur in `processBlock()` after `prepare()`.

2. **Lock-Free Audio-Thread Primitives**: The modulation matrix uses a seqlock + double-buffer swap for message-thread writes and audio-thread reads. The step sequencer and macro array use the same seqlock pattern. This avoids priority inversion on the audio thread.

3. **SPSC Command Queue**: `LockFreeQueue<ParamCommand, 8192>` carries parameter changes from UI/MCP threads to the audio thread with a power-of-2 ring buffer and cache-line-aligned indices. Queue health monitoring (`getCommandQueueUsage()`, `isCommandQueueHealthy()`) is exposed for diagnostics.

4. **Consistent xorshift32 PRNG**: `GranularMorphEngine`, `LFO`, `StepSequencer`, and `ModulationEngine` all use the same lock-free xorshift32 implementation, avoiding `<random>` and its heap/lock overhead.

5. **Touch Detection with Cooldown**: The per-parameter touch-detection system (`lastApplied_`, `touchCooldown_`, `touchMorphX_`/`touchMorphY_`) prevents the morph engine from overwriting manual knob changes. The cooldown is dynamically computed from sample rate and block size to maintain ~200 ms regardless of host configuration.

6. **Sidechain Threshold Pre-Computation**: `sidechainThresholdLinear_` is computed from the dB threshold in `syncStateFromAPVTS()` (message thread), avoiding `std::pow` on the audio thread.

7. **Proper JUCE Denormal Handling**: `juce::ScopedNoDenormals` is constructed at the top of `processBlock()`, preventing FPU stalls in the physics and smoothing loops.

8. **Comprehensive Integration Tests**: `TestVST3MidiAndSidechain.cpp`, `TestVST3AudioSignalAccuracy.cpp`, and `TestVST3ParameterAutomation.cpp` exercise the actual `MorePhiProcessor` with real JUCE APVTS parameters, audio buffers, and MIDI buffers, validating end-to-end signal flow.

9. **MIDI Note Range and Channel Filtering**: `MIDIRouter` correctly maps C3–B3 (notes 48–59) to slots 0–11, consumes both note-on and note-off to prevent leaked trigger notes, and supports configurable channel filtering with omni (0) default.

10. **Latency Accounting**: `LatencyManager` aggregates latency from the hosted plugin, oversampling, FFT window, and mastering chain, and reports the total via `setLatencySamples()` so the DAW applies correct Plugin Delay Compensation.

---

## Files Audited

| File | Lines | Focus Area |
|------|-------|------------|
| `src/MIDI/MIDIRouter.h` | 95 | MIDI routing, sidechain trigger |
| `src/MIDI/MIDIRouter.cpp` | 145 | MIDI filtering, envelope follower |
| `src/Plugin/PluginProcessor.h` | 696 | Audio I/O, state atomics, queue API |
| `src/Plugin/PluginProcessor.cpp` | 2269 | processBlock, lifecycle, state persistence |
| `src/Core/ModulationEngine.h` | 236 | LFO/envelope/sequencer ownership |
| `src/Core/ModulationEngine.cpp` | 475 | Source ticking, MIDI→modulation mapping |
| `src/Core/ModulationMatrix.h` | 204 | Double-buffer routing, seqlock |
| `src/Core/ModulationMatrix.cpp` | 268 | Apply logic, route management |
| `src/Core/LFO.h` | 108 | Waveform shapes, tempo sync |
| `src/Core/LFO.cpp` | 195 | S&H, Random, phase advancement |
| `src/Core/StepSequencer.h` | 152 | Direction modes, seqlock config |
| `src/Core/StepSequencer.cpp` | 202 | Clock, smoothing, ping-pong logic |
| `src/UI/MacroKnobStrip.h` | 32 | 8-knob UI layout |
| `src/UI/MacroKnobStrip.cpp` | 92 | Parameter enqueue, sync timer |
| `src/Core/FormantMorphEngine.h` | 241 | Cepstral lifter API, STFT params |
| `src/Core/FormantMorphEngine.cpp` | 411 | FFT, IFFT, OLA, envelope transplant |
| `src/Core/GranularMorphEngine.h` | 209 | Grain pool, circular buffers, pitch LUT |
| `src/Core/GranularMorphEngine.cpp` | 388 | Scheduling, rendering, source mixing |
| `src/Core/SpectralMorphEngine.h` | 214 | Phase vocoder, log-magnitude morph |
| `src/Core/SpectralMorphEngine.cpp` | 523 | STFT, IF, transient snap, OLA |
| `src/Core/VAEMorphEngine.h` | 190 | Latent space API, stub design |
| `src/Core/VAEMorphEngine.cpp` | 240 | Stub encode/decode/interpolate |
| `src/Core/ABCompareEngine.h` | 100 | Metrics, timer, rollback slot |
| `src/Core/ABCompareEngine.cpp` | 92 | Decision logic, capture/restore |
| `src/Core/GrainPool.h` | 263 | Hann envelope, 128-grain pool |
| `tests/Unit/TestMIDIRouting.cpp` | 123 | C3-B3, CC1, pass-through |
| `tests/Unit/TestAudioEngine.cpp` | 877 | Oversampling, SNR, latency, DC |
| `tests/Unit/TestGranularEngine.cpp` | 733 | **Mock-based tests** |
| `tests/Unit/TestSpectralEngine.cpp` | 817 | **Mock-based tests** |
| `tests/Unit/TestSidechainTrigger.cpp` | 166 | Edge detection, round-robin |
| `tests/Integration/TestVST3MidiAndSidechain.cpp` | 196 | End-to-end MIDI + sidechain |
| `tests/Integration/TestVST3AudioSignalAccuracy.cpp` | 293 | Bypass, gain, SNR, deterministic |
| `tests/Integration/TestVST3ParameterAutomation.cpp` | 195 | APVTS automation, parameter exposure |

---

*Report generated by Sub-Agent 5 (Audio I/O, MIDI & Features Validator).*
