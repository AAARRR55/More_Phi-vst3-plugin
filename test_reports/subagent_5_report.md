# Sub-Agent 5 Report: Audio I/O, MIDI, Genetic Breeding & Discrete Parameter Validation
## More-Phi VST3 Plugin v3.3.0

---

## Executive Summary

This report validates the audio I/O pipeline, MIDI routing, genetic breeding engine, discrete parameter handling, macro knobs, and tempo sync integration. **Two critical issues** were identified that can cause silent data corruption or stuck notes, plus **one high-priority architectural flaw** where the UI breeding panel completely bypasses the safety-engineered `GeneticEngine` class. A `ParameterClassifier` bug breaks discrete parameter step quantization, and several MIDI edge cases can produce stuck notes in hosted synthesizers.

---

## Critical Issues

### 1. BreedingPanel Re-implements Breeding Logic — Completely Bypasses `GeneticEngine` and `SanityConfig`
**File:** `src/UI/BreedingPanel.cpp`  
**Lines:** 50–101 (`breedSnapshots()`), 103–141 (`mutateSnapshot()`)

`BreedingPanel::breedSnapshots()` performs its own crossover blending and mutation inline instead of calling `GeneticEngine::breed()`. It never consults `SanityConfig`, meaning **volume, gain, output, bypass, and mute parameters are NOT protected during breeding from the UI**. A user can inadvertently breed a snapshot with the hosted plugin's output gain at 0 dB or bypass enabled, causing silent output or a dead plugin.

**Current code (BreedingPanel.cpp:80–86):**
```cpp
const float alpha = 0.2f + random_.nextFloat() * 0.6f;
std::vector<float> blended(static_cast<size_t>(count), 0.0f);
for (int i = 0; i < count; ++i)
    blended[static_cast<size_t>(i)] = a[static_cast<size_t>(i)] * (1.0f - alpha)
                                    + b[static_cast<size_t>(i)] * alpha;
```

This is a simple linear interpolation with no mutation, no crossover per-parameter, and no sanity protection. The `GeneticEngine` class (`src/Core/GeneticEngine.cpp:27–46`) already implements per-parameter crossover, mutation, and `SanityConfig` guards, but **is never invoked from the UI layer**.

**Recommended fix:**
```cpp
// In BreedingPanel::breedSnapshots()
ParameterState parentA, parentB;
parentA.parameterCount = count;
parentB.parameterCount = count;
// ... fill parentA.values / parentB.values from vectors ...

auto sanity = proc_.getSanityConfig();
auto offspring = GeneticEngine::breed(parentA, parentB,
                                      0.2f + random_.nextFloat() * 0.6f,
                                      0.0f,  // no extra UI mutation here
                                      random_, sanity);
```

Similarly, `mutateSnapshot()` applies a flat random delta to **all** parameters (line 122–126) instead of using `GeneticEngine::smartRandomize()`, which only mutates learned parameters and respects `SanityConfig`. The UI's "Mutate" button will mutate bypass and volume parameters without protection.

---

### 2. `ParameterClassifier` Sets `stepCount = 1` for Discrete/Enumeration — Breaks `DiscreteParameterHandler` Quantization
**File:** `src/Core/ParameterClassifier.cpp`  
**Lines:** 67–68

```cpp
else if (meta.type == ParameterType::Discrete || meta.type == ParameterType::Enumeration)
    meta.stepCount = 1;
```

When `DiscreteParameterHandler::initialize()` reads `stepCount` into `stepCount_` (DiscreteParameterHandler.cpp:33–39), a value of `1` means `maxStep = steps - 1 = 0`.

In `DiscreteParameterHandler::stepToValue()` (line 234–240):
```cpp
const float maxStep = static_cast<float>(steps - 1);
return (maxStep > 0.0f) ? static_cast<float>(step) / maxStep : 0.0f;
```
With `maxStep = 0`, this always returns `0.0f` regardless of the step index. In `valueToStep()` (line 225–232):
```cpp
return std::clamp(static_cast<int>(value * maxStep), 0, steps - 1);
```
With `maxStep = 0`, this always returns `0`. **Every discrete or enumeration parameter is quantized to 0.0 after the first pass through `DiscreteParameterHandler`**, effectively silencing or disabling the hosted plugin's discrete controls (e.g., waveform selectors, filter types, LFO shapes).

**Recommended fix:**
```cpp
// ParameterClassifier.cpp:67-68
else if (meta.type == ParameterType::Discrete || meta.type == ParameterType::Enumeration)
    meta.stepCount = 10;  // or query host.getNumSteps() / host.getParameterNumSteps()
```

Better yet, query the actual step count from the hosted plugin's parameter descriptor:
```cpp
meta.stepCount = static_cast<uint16_t>(host.getParameterNumSteps(index));
```

---

### 3. MIDI Note-Offs in Trigger Range Are Consumed and Never Passed to Hosted Plugin
**File:** `src/MIDI/MIDIRouter.cpp`  
**Lines:** 46–60

```cpp
if (msg.isNoteOn() || msg.isNoteOff())
{
    // ... check if note is in trigger range ...
    if (note >= trigBase && note < trigBase + SnapshotBank::NUM_SLOTS)
    {
        if (msg.isNoteOn() && msg.getVelocity() > 0)
        {
            // trigger snapshot
        }
        consumed = true;  // Consume BOTH note-on and note-off
    }
}
```

If a user plays a note within the C3–B3 trigger range (e.g., C3 = note 48), the **note-on** triggers a snapshot recall and is consumed. The **note-off** is also consumed and **never forwarded to the hosted plugin**. If the hosted plugin is a synthesizer, it will never receive the note-off for C3, causing a **stuck note** until the user sends another note-on on the same channel with velocity 0, or until the host sends an all-notes-off.

This is a **user-experience crash** for anyone using a MIDI keyboard to both trigger snapshots and play the hosted instrument. The note-off must be passed through to the hosted plugin even if the note-on was consumed.

**Recommended fix:**
```cpp
if (note >= trigBase && note < trigBase + SnapshotBank::NUM_SLOTS)
{
    if (msg.isNoteOn() && msg.getVelocity() > 0)
    {
        const int slot = note - trigBase;
        auto cb = snapshotCb_.load(std::memory_order_acquire);
        if (cb) cb(slot, snapshotCtx_.load(std::memory_order_acquire));
        consumed = true;  // Only consume the note-on
    }
    // Note: do NOT consume note-offs — let them pass to the hosted plugin
}
```

---

## High-Priority Improvements

### 4. `GeneticEngine::breed()` and `smartRandomize()` Are Not Real-Time Safe
**File:** `src/Core/GeneticEngine.cpp`  
**Lines:** 18–25, 42–44

```cpp
juce::Logger::writeToLog("GeneticEngine::breed() - Parameter count mismatch: ...");
// ...
offspring.setName("Offspring");
```

`juce::Logger::writeToLog` can allocate memory and acquire locks. `ParameterState::setName` may allocate a `std::string` or `juce::String`. `GeneticEngine` lives in `src/Core/` and is advertised as part of the audio-thread-safe engine. If an MCP tool or future MIDI-triggered breeding path calls `GeneticEngine::breed()` from the audio thread, it will violate the zero-allocation contract.

**Recommended fix:** Remove logging and string allocation from the breed path, or document that `GeneticEngine` must only be called from the message thread.

```cpp
// Remove or guard the log:
#if ! defined(MORE_PHI_TEST_MODE)
    juce::Logger::writeToLog(...);  // only log in debug/tests
#endif
offspring.occupied = true;
offspring.parameterCount = count;
// offspring.setName("Offspring");  // avoid allocation — name is not needed for audio
```

---

### 5. `MacroKnobStrip` Maps Only to Parameters 0–7 with No User Configurable Assignment
**File:** `src/UI/MacroKnobStrip.cpp`  
**Lines:** 15–29, 67–90

The macro knobs are hardcoded to control hosted parameters `0` through `7`. There is no UI mechanism to map a macro knob to parameter 42, or to exclude volume/bypass from the first 8 parameters. If a hosted plugin has volume on parameter 0, the macro knob directly controls it with no sanity protection.

**Recommended fix:** Add a `MacroAssignment` structure and a small configuration UI (right-click menu) to assign each knob to any parameter index, with an optional "Sanity protected" checkbox that reads from `ParameterClassifier`.

---

### 6. `DiscreteParameterHandler` Silently Fails if `outputValues` Is Undersized
**File:** `src/Core/DiscreteParameterHandler.cpp`  
**Line:** 65

```cpp
if (outputValues.size() < interpolatedValues.size()) { assert(false); return; }
```

In a release build, `assert(false)` is a no-op. The function returns without writing anything, leaving `outputValues` uninitialized. Callers in `processBlock` expect the buffer to be filled. If `finalOutput_` is somehow resized smaller than expected (e.g., due to a race or a plugin with >2048 params), this will cause the morph output to contain garbage for that block.

**Recommended fix:** Replace with a safe clamp or clear:
```cpp
if (outputValues.size() < interpolatedValues.size())
{
    jassertfalse;
    outputValues.resize(interpolatedValues.size(), 0.0f);  // safe fallback
}
```

---

### 7. `MIDIRouter` Iterates MIDI Buffer by Value, Not by Reference
**File:** `src/MIDI/MIDIRouter.cpp`  
**Line:** 26

```cpp
for (const auto metadata : midi)
```

`juce::MidiBuffer`'s range-for returns `MidiMessageMetadata` objects that contain a `MidiMessage` (which is a lightweight wrapper around a byte array pointer, but still copies a few members). Copying by value is unnecessary and adds a small per-event overhead. While `MidiMessageMetadata` is small, the idiomatic JUCE pattern is `const auto&`.

**Recommended fix:**
```cpp
for (const auto& metadata : midi)
```

---

### 8. Sidechain Threshold Is Set Every Block Regardless of Whether It Changed
**File:** `src/Plugin/PluginProcessor.cpp`  
**Lines:** 1235–1237

```cpp
midiRouter.setSidechainThreshold(sidechainThresholdLinear_.load(std::memory_order_relaxed));
midiRouter.processSidechain(scBuffer);
```

`setSidechainThreshold` stores an atomic float. The atomic store itself is cheap, but the method is called every single `processBlock` even when the threshold parameter hasn't changed. This is redundant and adds unnecessary atomic traffic on the audio thread.

**Recommended fix:** Cache the last applied linear threshold and only call `setSidechainThreshold` when it changes:
```cpp
static float lastScThresholdLinear = -1.0f;  // member variable, not static local
const float scLinear = sidechainThresholdLinear_.load(std::memory_order_relaxed);
if (scLinear != lastScThresholdLinear)
{
    midiRouter.setSidechainThreshold(scLinear);
    lastScThresholdLinear = scLinear;
}
```

---

### 9. `BreedingPanel::mutateSnapshot()` Does Not Use `smartRandomize()` or `SanityConfig`
**File:** `src/UI/BreedingPanel.cpp`  
**Lines:** 103–141

The mutation button applies a flat `±0.06` delta to **all** parameters, including bypass and volume. It should use `GeneticEngine::smartRandomize()` with the `learnedParams` set from `ParameterClassifier` and `SanityConfig` from the processor.

---

## Medium-Priority Refinements

### 10. `isBusesLayoutSupported` Forces Symmetric I/O
**File:** `src/Plugin/PluginProcessor.cpp`  
**Lines:** 1012–1035

```cpp
if (mainIn != mainOut)
    return false;
```

More-Phi requires the input and output channel sets to be identical. This prevents hosting a mono-only plugin inside a stereo More-Phi instance, because the hosted plugin would see the same channel count as More-Phi. Some DAWs will reject the plugin if the user tries to insert it on a mono track. Consider relaxing this to allow `mainIn` to be mono and `mainOut` to be stereo (with automatic upmixing), or at least documenting the limitation.

---

### 11. `ParameterClassifier::detectTypeFromName()` Can Misclassify Parameters
**File:** `src/Core/ParameterClassifier.cpp`  
**Lines:** 106–189

Name-based heuristics are inherently fragile. For example:
- A parameter named `"Filter Type"` is classified as `Enumeration` (correct).
- A parameter named `"Drive Type"` is also classified as `Enumeration` (might be a continuous drive amount with a mode selector).
- A parameter named `"LFO Rate"` is classified as `Frequency` (correct for Hz, but incorrect if it's a synced beat division like `"1/4", "1/8"`).

**Recommendation:** Add a fallback that reads `host.getParameterNumSteps(index)` and `host.isDiscrete(index)` to override name-based guesses when the host provides clear metadata.

---

### 12. Time Signature Is Read but Never Used
**File:** `src/Plugin/PluginProcessor.cpp`  
**Lines:** 1168–1170

```cpp
const auto signature = info.getTimeSignature().orFallback(juce::AudioPlayHead::TimeSignature{});
transportTimeSigNumerator_.store(signature.numerator, std::memory_order_relaxed);
transportTimeSigDenominator_.store(signature.denominator, std::memory_order_relaxed);
```

The time signature is stored in atomics but never consumed by the modulation engine, the step sequencer, or any other component. If tempo-synced LFOs or bar-synced features are added later, the time signature will be needed. Currently, it's dead code that adds a small per-block overhead. Either use it (e.g., in `ModulationEngine` for bar-length modulation) or remove it to simplify the transport snapshot.

---

### 13. `DiscreteParameterHandler` Strategy Overrides Use `std::vector` Search (`O(n)`)
**File:** `src/Core/DiscreteParameterHandler.cpp`  
**Lines:** 196–212

```cpp
for (const auto& override : strategyOverrides_)
{
    if (override.parameterIndex == index)
        return override.strategy;
}
```

With a small number of overrides, this is fine. But if the user configures overrides for hundreds of parameters, the linear scan inside `processDiscreteParameter()` (called every block for every discrete parameter) becomes `O(n*m)`.

**Recommended fix:** Use a `std::unordered_map<int, BlendStrategy>` or a `std::array` lookup if the parameter count is bounded by `MAX_PARAMETERS`.

---

### 14. `processBlock` Runs `autoMasteringEngine_.analyzeBlock()` Even When Bypassed
**File:** `src/Plugin/PluginProcessor.cpp`  
**Line:** 1549

```cpp
autoMasteringEngine_.analyzeBlock(buffer);
```

This is called after the bypass check, so it runs even when the user has enabled the bypass parameter. The analysis is non-mutating, but it consumes CPU cycles. If the user bypasses the plugin to save CPU, the analyzer still runs. Consider guarding it with `if (!isBypassed)` unless the analysis is explicitly needed for metering while bypassed.

---

## Low-Priority Enhancements

### 15. `refreshDiscreteMap` Defined Outside the `namespace more_phi` Block
**File:** `src/Plugin/PluginProcessor.cpp`  
**Lines:** 2256–2263

```cpp
} // namespace more_phi

// refreshDiscreteMap — called after plugin load to update discrete parameter classification
void more_phi::MorePhiProcessor::refreshDiscreteMap()
{
    ...
}
```

While valid C++, this is unusual and inconsistent with the rest of the file. It should be moved inside the namespace block for readability.

---

### 16. `MIDIRouter::prepare()` Accepts but Ignores `expectedMidiEventsPerBlock`
**File:** `src/MIDI/MIDIRouter.cpp`  
**Line:** 10

```cpp
void MIDIRouter::prepare(int /*expectedMidiEventsPerBlock*/)
{
```

The parameter is commented out but could be used to assert that the preallocated storage (`MAX_MIDI_EVENTS = 256`) is sufficient for the expected load. Consider adding a compile-time or runtime assertion:
```cpp
jassert(expectedMidiEventsPerBlock <= MAX_MIDI_EVENTS);
```

---

### 17. `MacroKnobStrip` Labels Are Static (`"P1"`–`"P8"`) and Never Show Parameter Names
**File:** `src/UI/MacroKnobStrip.cpp`  
**Lines:** 35, 79

The labels are initialized to `"P1"`, `"P2"`, etc. In `syncKnobsToPlugin()`, the label text is updated to the parameter name, but only if `i < count`. If the parameter count is 0, the label shows `"-"`. However, the label font is a hardcoded `"Segoe UI" 10.0f`, which may not be available on macOS or Linux.

**Recommendation:** Use `juce::FontOptions()` with a fallback font, or use JUCE's default sans-serif font.

---

### 18. `getTailLengthSeconds()` Uses `const_cast` to Call Non-Const Host Manager
**File:** `src/Plugin/PluginProcessor.cpp`  
**Lines:** 2230–2248

```cpp
double MorePhiProcessor::getTailLengthSeconds() const
{
    double tail = 0.0;
    auto& hostMgr = const_cast<PluginHostManager&>(hostManager);
    if (auto* plugin = hostMgr.acquirePluginForUse())
    {
        ...
    }
    return tail;
}
```

`const_cast` to remove constness is a code smell. `PluginHostManager` should provide a `const` overload of `acquirePluginForUse()`, or `MorePhiProcessor` should cache the tail length in a mutable atomic.

---

### 19. `DiscreteParameterHandler` Has Unimplemented `Crossfade` Strategy for Discrete Parameters
**File:** `src/Core/DiscreteParameterHandler.cpp`  
**Lines:** 125–140

The `Crossfade` strategy allows smooth interpolation between discrete values when `morphAmount` is between 0.1 and 0.9. This is mathematically questionable for discrete parameters (e.g., crossfading between "Saw" and "Square" as a continuous value). It is also not wired to any UI or automated strategy selection. Consider removing it or documenting it as experimental.

---

## Recommended Fixes (Code Snippets)

### Fix 1: BreedingPanel — Use `GeneticEngine` with `SanityConfig`

```cpp
void BreedingPanel::breedSnapshots()
{
    auto& bank = proc_.getSnapshotBank();
    std::array<int, SnapshotBank::NUM_SLOTS> occupied{};
    const int occupiedCount = bank.getOccupiedSlots(occupied);
    if (occupiedCount < 2) { /* ... */ return; }

    const int idxA = occupied[static_cast<size_t>(random_.nextInt(occupiedCount))];
    int idxB = idxA;
    while (idxB == idxA)
        idxB = occupied[static_cast<size_t>(random_.nextInt(occupiedCount))];

    std::vector<float> vecA, vecB;
    if (!bank.getSlotValuesCopy(idxA, vecA) || !bank.getSlotValuesCopy(idxB, vecB))
        return;

    const int count = juce::jmin(static_cast<int>(vecA.size()),
                                 static_cast<int>(vecB.size()),
                                 MAX_PARAMETERS);
    if (count <= 0) return;

    ParameterState parentA, parentB;
    parentA.parameterCount = count;
    parentB.parameterCount = count;
    for (int i = 0; i < count; ++i)
    {
        parentA.values[static_cast<size_t>(i)] = vecA[static_cast<size_t>(i)];
        parentB.values[static_cast<size_t>(i)] = vecB[static_cast<size_t>(i)];
    }

    const float crossover = 0.2f + random_.nextFloat() * 0.6f;
    const float mutation  = 0.05f;  // small UI mutation
    auto sanity = proc_.getSanityConfig();
    auto offspring = GeneticEngine::breed(parentA, parentB, crossover, mutation, random_, sanity);

    std::vector<float> blended(count);
    for (int i = 0; i < count; ++i)
        blended[static_cast<size_t>(i)] = offspring.values[static_cast<size_t>(i)];

    const int queued = proc_.enqueueParameterState(blended);
    // ...
}
```

### Fix 2: ParameterClassifier — Correct `stepCount` for Discrete/Enumeration

```cpp
// In ParameterClassifier::analyzeParameters()
if (meta.type == ParameterType::Binary)
    meta.stepCount = 2;
else if (meta.type == ParameterType::Discrete || meta.type == ParameterType::Enumeration)
{
    // Query actual steps from host; fall back to 10
    const int hostSteps = host.getParameterNumSteps(static_cast<int>(i));
    meta.stepCount = (hostSteps > 1) ? static_cast<uint16_t>(hostSteps) : 10;
}
```

### Fix 3: MIDIRouter — Pass Note-Offs Through

```cpp
if (note >= trigBase && note < trigBase + SnapshotBank::NUM_SLOTS)
{
    if (msg.isNoteOn() && msg.getVelocity() > 0)
    {
        const int slot = note - trigBase;
        auto cb = snapshotCb_.load(std::memory_order_acquire);
        if (cb) cb(slot, snapshotCtx_.load(std::memory_order_acquire));
        consumed = true;  // Only consume the triggering note-on
    }
    // Note-offs are NOT consumed — they must reach the hosted synth
}
```

### Fix 4: GeneticEngine — Remove Audio-Thread-Unsafe Operations

```cpp
ParameterState GeneticEngine::breed(...)
{
    ParameterState offspring;
    const int count = juce::jmin(parentA.parameterCount, parentB.parameterCount);
    // Remove the log — callers can log on the message thread if needed
    for (int i = 0; i < count; ++i)
    {
        if (sanity.enabled && sanity.protectedIndices.count(i) > 0)
        {
            offspring.values[static_cast<size_t>(i)] = parentA.values[static_cast<size_t>(i)];
            continue;
        }
        float blended = parentA.values[i] * (1.0f - crossoverRatio)
                      + parentB.values[i] * crossoverRatio;
        float mutation = (rng.nextFloat() * 2.0f - 1.0f) * mutationStrength;
        offspring.values[static_cast<size_t>(i)] = juce::jlimit(0.0f, 1.0f, blended + mutation);
    }
    offspring.occupied = true;
    offspring.parameterCount = count;
    // offspring.setName("Offspring");  // REMOVED — avoid allocation
    return offspring;
}
```

---

## Verification Checklist

| Area | Checked | Critical Finding |
|------|---------|------------------|
| MIDI Note C3–B3 trigger | ✅ | Note-offs swallowed — stuck notes (Critical #3) |
| MIDI CC1 morph position | ✅ | Correctly routed to `setFaderPos` (PluginProcessor.cpp:120–125) |
| MIDI channel filter | ✅ | Omni (0) + 1–16 filtering works correctly |
| `juce::MidiBuffer` iteration | ✅ | Should use `const auto&` (High #7) |
| Audio I/O bus layout | ✅ | Symmetric I/O enforced; sidechain mono/stereo OK |
| `isBusesLayoutSupported` | ✅ | Up to 8 channels accepted |
| Sample rate / block size changes | ✅ | `prepareToPlay` handles reallocation; `reset()` re-inits state |
| Latency reporting | ✅ | `updateReportedLatency()` queries hosted plugin + oversampling + FFT |
| `processBlock` bypass | ✅ | Passes through audio but still runs analyzer (Medium #14) |
| Genetic `breed()` crossover | ✅ | Exists in `GeneticEngine` but **unused by UI** (Critical #1) |
| `SanityConfig` guarding | ✅ | `GeneticEngine` respects it, but `BreedingPanel` does not |
| `smartRandomize()` | ✅ | Exists in `GeneticEngine` but **unused by UI** (High #9) |
| Random generator quality | ✅ | `juce::Random` is used; adequate for this use case |
| Discrete parameter snapping | ✅ | **Broken for stepCount=1** (Critical #2) |
| `ParameterClassifier` categories | ✅ | 8 categories detected; heuristics can misclassify (Medium #11) |
| Macro knob mapping | ✅ | Hardcoded 0–7; no user assignment (High #5) |
| Touch detection cooldown | ✅ | Dynamic ~200ms cooldown computed in `prepareToPlay` (M-6 FIX) |
| Tempo sync / playhead | ✅ | BPM forwarded to modulation engine; time signature unused (Medium #12) |
| `AudioPlayHead` usage | ✅ | `getPosition()` optional handled correctly for JUCE 8 |

---

*Report generated by Sub-Agent 5: Audio I/O, MIDI & Features Validator.*
