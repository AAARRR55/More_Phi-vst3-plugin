# MorphSnap v2 — Audio Engine & Development Workflow Specification

Version: 2.0.0-draft
Date: 2026-02-26
Status: Implementation-ready

---

## Part A — Audio Engine Implementation

### Area 6.1: Sample Rate & Bit Depth Support

#### Supported Sample Rate Matrix

| Rate (kHz) | Status    | Notes                                          |
|------------|-----------|------------------------------------------------|
| 44.1       | Required  | CD/standard DAW default                        |
| 48         | Required  | Broadcast/video standard, primary test rate    |
| 88.2       | Required  | Double-rate for high-res delivery              |
| 96         | Required  | Pro-audio standard                             |
| 176.4      | Required  | Quad 44.1, rare in practice                   |
| 192        | Required  | High-end mastering, tests at 192 kHz           |

All sample rates use the same code paths. The physics engine uses
time-delta (dt) values derived from the actual block duration rather
than hard-coded timing constants, so all rates are automatically correct.

#### Internal Processing Precision: float32

float32 is the correct choice for MorphSnap's processing domain:

1. **Parameter morphing operates in [0, 1] normalized space.** The LSB
   of a 32-bit float at 1.0 is ~1.19e-7, giving 24-bit effective
   dynamic range — more than sufficient for parameter interpolation.
2. **JUCE's AudioBuffer and APVTS use float.** Promoting to double
   would require conversions at every boundary.
3. **SIMD throughput.** AVX2 processes 8 floats vs 4 doubles per cycle.
   The smoothing hot loop (MorphProcessor::applySmoothing) is 2x faster
   with float.
4. **double for time accumulation only.** driftTime_ and physics
   integration accumulators use double to prevent clock drift over
   long sessions (> 30 min).

```cpp
// Correct pattern: accumulate time in double, convert to float for physics
class MorphProcessor {
    double driftTimeAccum_  { 0.0 };   // seconds — double to avoid drift
    float  processedX_      { 0.0f };  // position — float is sufficient
};
```

#### Sample Rate Change Handling (prepareToPlay Lifecycle)

```
prepareToPlay(sampleRate, maxBlockSize) called on message thread
│
├── 1. Store sampleRate, maxBlockSize
├── 2. OversamplingWrapper::prepare(maxBlockSize, numChannels, sampleRate)
│       → Reallocates FIR filter buffers at new rate
│       → OversamplingWrapper::reset() clears filter state
├── 3. LatencyManager::setOversamplingLatency(os.getLatencyInSamples())
├── 4. LatencyManager::setHostedPluginLatency(hostedPlugin->getLatencySamples())
├── 5. setLatencySamples(latencyManager_.getTotal())  ← notify DAW
├── 6. PhysicsEngine: no state change needed (uses dt, not absolute time)
├── 7. GeneticEngine: stateless, no action
└── 8. MorphProcessor::prepare(paramCount): resize smoothedValues_
```

**Key constraint**: `prepareToPlay` is called on the message thread.
The audio thread must not be running during this call. JUCE guarantees
this ordering.

#### Bit Depth Handling

MorphSnap processes audio as float internally regardless of the DAW's
bit depth setting. The DAW's audio engine (not the plugin) handles
conversion between the session bit depth and float.

For **dithering** when the session runs at 16-bit (e.g., for legacy
hardware): apply TPDF (Triangular Probability Density Function) dither
before writing to output buffers. This is relevant only if MorphSnap
ever writes audio samples — currently it does not (it is a FX/meta
plugin). If audio pass-through is added in v2, add:

```cpp
// In processBlock, after all processing:
if (bitDepth_ <= 16) {
    juce::ScopedNoDenormals noDenormals;
    // TPDF dither: add two uniform random values, scale to 1 LSB
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch) {
        auto* data = buffer.getWritePointer(ch);
        for (int i = 0; i < buffer.getNumSamples(); ++i) {
            float dither = (rng_.nextFloat() - rng_.nextFloat()) / 32768.0f;
            data[i] += dither;
        }
    }
}
```

---

### Area 6.2: Buffer Size Management

#### Supported Range: 32 to 8192 samples

The design is explicitly block-size independent. No algorithm in
MorphSnap assumes a specific block size:

- **Physics engine**: uses `dt = numSamples / sampleRate` — correct at
  any block size.
- **Smoothing**: coefficient derived from per-block time, not per-sample.
- **LockFreeQueue**: 8192-slot capacity is the absolute ceiling, not
  correlated to block size.
- **OversamplingWrapper**: `initProcessing(maxSamplesPerBlock)` pre-allocates
  the worst-case buffer; smaller blocks run without reallocation.

#### Ring Buffer Sizing Relative to Block Size

The `LockFreeQueue<ParamCommand, 8192>` is sized for the worst case:
a UI or MCP thread enqueuing all 2048 parameters before the audio thread
drains it. At 32-sample blocks at 192 kHz, a block completes in 166
microseconds — the UI thread can generate at most ~12 commands in that
window. The 8192 capacity provides a safety margin of 4x.

For the planned spectral grain buffer in v2 (grain-based morphing):

```
grainBufferSize = nextPowerOf2(4 * maxBlockSize + fftSize)
```

Example: maxBlock=1024, fftSize=2048 → 8192-sample grain buffer.

#### FFT Processing with Variable Buffer Sizes

Spectral morphing (planned for v2) uses overlap-add FFT processing:

```
FFT size: fixed at 2048 (configurable: 512, 1024, 2048, 4096)
Hop size: fftSize / 4 = 512 (75% overlap)
Analysis latency: fftSize / 2 samples

Variable-block handling:
  Input ring buffer accumulates samples until >= hopSize available.
  Multiple FFT frames computed if block > hopSize.
  Partial frames held in ring buffer for next processBlock call.
```

This means processBlock() is not called once per FFT frame but rather
once per host block, with the ring buffer bridging the size mismatch.

#### Latency Reporting Strategy

```
Processing mode     | setLatencySamples() value
────────────────────|────────────────────────────────────────────────
Direct (no OS)      | hostedPluginLatency
Direct + x4 OS      | x4FIRLatency + hostedPluginLatency
Spectral morph      | fftSize/2 + hostedPluginLatency
Spectral + x4 OS    | fftSize/2 + x4FIRLatency + hostedPluginLatency
```

`LatencyManager` (see `src/Core/LatencyManager.h`) centralizes this.
Call `setLatencySamples(latencyManager_.getTotal())` at the end of
`prepareToPlay()` and whenever the hosted plugin changes.

---

### Area 6.3: Oversampling Strategy

The production default is **x4 FIR** for all nonlinear processing
stages. The implementation is in `src/Core/OversamplingWrapper.h`.

#### When to Oversample

| Processing Stage                    | Oversample? | Reason                              |
|-------------------------------------|-------------|-------------------------------------|
| Parameter interpolation (linear)    | No          | Linear — no aliasing possible       |
| Physics spring/damper (linear ODE)  | No          | No nonlinearity                     |
| Spectral waveshaping (saturation)   | Yes (x4+)   | Nonlinear → alias products above Ny  |
| Grain envelope (amplitude mod)      | Optional x2 | Mild AM aliasing near Nyquist        |
| Audio pass-through (identity)       | No          | Pointless CPU cost                  |

#### Factor Selection Guide

```
x1  (bypass):  Direct mode, no audio processing. Zero latency.
x2:            Light saturation at low frequencies. Marginal quality gain.
               +~130% CPU. Stopband ~80 dB (sufficient for 16-bit delivery).
x4:  DEFAULT   Production quality. +~380% CPU vs x1.
               Stopband ~100 dB. Suitable for 24-bit output.
x8:            Mastering-grade, extremely CPU intensive. +~850% vs x1.
               Stopband ~120 dB. Use only on dedicated render machines.
```

#### FIR vs IIR Anti-Aliasing Filter Selection

FIR (default for MorphSnap):
- Linear phase — no pre-ringing that could corrupt snapshot transitions
- Higher CPU (polyphase Kaiser, ~128 taps per stage)
- Latency: ~(filterOrder / 2) / sampleRate per stage

IIR:
- Minimum phase — lower CPU, ~40% cheaper than FIR
- Non-linear phase causes pre-ringing on transients
- Acceptable for sustained tonal content

Recommendation: Expose this as an expert setting in the AI Status panel.
Default to FIR. Allow IIR for users who need lower CPU.

#### JUCE Built-in vs Custom Oversampling

Use JUCE's `juce::dsp::Oversampling<float>` (wrapped in
`OversamplingWrapper`). Reasons:
- Battle-tested in production plugins
- Handles buffer management internally
- Integrates cleanly with AudioBlock
- `filterHalfBandFIREquiripple` is equiripple (maximally flat passband)

Custom oversampling would only be justified if:
- Non-standard filter responses are needed (e.g., Chebyshev)
- Per-grain oversampling in the grain engine (JUCE's API expects full-block)

---

### Area 6.4: Latency Compensation

See `src/Core/LatencyManager.h` for the implementation.

#### Total Latency Formula

```
totalLatency = oversamplingLatency       // OversamplingWrapper::getLatencyInSamples()
             + fftWindowLatency          // fftSize / 2 (spectral mode only, else 0)
             + hostedPluginLatency       // AudioPluginInstance::getLatencySamples()
```

#### Latency When Switching Processing Modes

Switching from Direct to Spectral mode changes the latency. The correct
procedure:

```cpp
void MorphSnapProcessor::setProcessingMode(ProcessingMode mode) {
    // Called on message thread
    processBlock must be paused (JUCE ensures this in prepareToPlay)

    if (mode == ProcessingMode::Spectral) {
        latencyManager_.setFFTWindowLatency(fftSize_ / 2);
    } else {
        latencyManager_.setFFTWindowLatency(0);
    }

    setLatencySamples(latencyManager_.getTotal());
    // DAW will reconfigure its delay compensation graph
    updateHostDisplay(ChangeDetails::withLatencyChanged());
}
```

#### Plugin Hosting Latency Passthrough

After successfully loading a hosted plugin:

```cpp
void MorphSnapProcessor::onHostedPluginLoaded() {
    if (auto* instance = hostManager.getLoadedPlugin()) {
        int pluginLatency = instance->getLatencySamples();
        latencyManager_.setHostedPluginLatency(pluginLatency);
        setLatencySamples(latencyManager_.getTotal());
        updateHostDisplay(ChangeDetails::withLatencyChanged());
    }
}
```

---

### Area 6.5: CPU Architecture Optimizations

#### SIMD Strategy: Runtime Dispatch (Chosen over Compile-Time)

MorphSnap distributes a single binary to users with varied CPUs. Runtime
dispatch is mandatory:

```cpp
// Pattern already established in MorphProcessor.cpp — extend this:
#if defined(MORPHSNAP_USE_AVX)
    // AVX2 path: 8 floats/cycle
#elif defined(MORPHSNAP_USE_SSE)
    // SSE2 path: 4 floats/cycle
#else
    // Scalar fallback
#endif
```

For v2, add ARM NEON detection for Apple Silicon:

```cpp
// Add to MorphProcessor.cpp SIMD detection block:
#if defined(__ARM_NEON) || defined(__aarch64__)
    #include <arm_neon.h>
    #define MORPHSNAP_USE_NEON 1
#endif
```

#### Hot Loops to Vectorize (Priority Order)

1. `MorphProcessor::applySmoothing()` — already SIMD in v3.3.0. ✓
2. `InterpolationEngine::interpolateBatch_SIMD()` — already SIMD. ✓
3. **Grain mixing** (v2): Sum N grain buffers into output. AVX2 can process
   8 grains per cycle using FMA instructions.
4. **FFT windowing**: Apply Hann window to 2048-sample block. Pure
   multiply-accumulate, ideal for AVX2.
5. **Spectral interpolation**: Per-bin complex-number lerp. 4 floats
   per complex pair → AVX2 processes 4 bins per cycle.

#### NEON Implementation (Apple Silicon)

```cpp
#elif defined(MORPHSNAP_USE_NEON)
// ARM NEON path — 4 floats at once (128-bit registers)
// On Apple Silicon, use NEON via arm_neon.h
float32x4_t rateVec         = vdupq_n_f32(rate);
float32x4_t oneMinusRateVec = vdupq_n_f32(oneMinusRate);

const size_t simdCount = maxSmoothable - (maxSmoothable % 4);
for (size_t i = 0; i < simdCount; i += 4) {
    float32x4_t smoothed = vld1q_f32(smoothedValues_.data() + i);
    float32x4_t out      = vld1q_f32(output.data() + i);

    // NEON fused multiply-add: a*b + c (available on ARMv8.1+)
    float32x4_t newSmoothed = vmlaq_f32(
        vmulq_f32(out, oneMinusRateVec),
        smoothed, rateVec);

    vst1q_f32(smoothedValues_.data() + i, newSmoothed);
    vst1q_f32(output.data() + i, newSmoothed);
}
for (size_t i = simdCount; i < maxSmoothable; ++i) {
    smoothedValues_[i] = smoothedValues_[i] * rate + output[i] * oneMinusRate;
    output[i] = smoothedValues_[i];
}
```

#### Cache-Line Alignment for Hot Structures

```cpp
// Align hot arrays to 64-byte cache lines to prevent false sharing.
// Apply to: smoothedValues_, LockFreeQueue indices

// In MorphProcessor.h:
alignas(64) std::vector<float> smoothedValues_;

// In LockFreeQueue.h (already cache-line separated for indices):
alignas(64) std::atomic<size_t> head_ { 0 };
alignas(64) std::atomic<size_t> tail_ { 0 };
```

#### Memory Pool for Grain Allocation (v2)

```cpp
// Pre-allocate all grain buffers in prepare(), never in processBlock().
// Pool: fixed-size array of grain frames.
struct GrainPool {
    static constexpr int MAX_GRAINS    = 64;
    static constexpr int MAX_GRAIN_LEN = 4096; // samples

    // Heap-allocated in prepare() — not on stack (avoids FL Studio overflow)
    std::unique_ptr<float[]> buffer;  // MAX_GRAINS * MAX_GRAIN_LEN floats
    std::array<bool, MAX_GRAINS> active {};

    void prepare() {
        buffer = std::make_unique<float[]>(MAX_GRAINS * MAX_GRAIN_LEN);
        active.fill(false);
    }

    float* grain(int index) noexcept {
        return buffer.get() + index * MAX_GRAIN_LEN;
    }
};
```

#### Thread Affinity and Priority

On Windows, JUCE's audio thread is created with `THREAD_PRIORITY_TIME_CRITICAL`
by default. No additional management is needed.

For the background MCP thread (`MCPServer`), it uses JUCE's `juce::Thread`
which defaults to normal priority. Explicitly set it to below-normal to
ensure the audio thread always wins CPU:

```cpp
// In MCPServer::run():
juce::Thread::setCurrentThreadPriority(3); // 0=lowest, 10=highest, 5=normal
```

---

### Area 6.6: Audio Quality Assurance

#### TPDF Dithering

If audio pass-through is added in v2 and the session bit depth is
configured below 24-bit, apply TPDF dither (two rectangular PDF values).
See the bit depth section above for the implementation pattern.

#### DC Offset Prevention

Two mechanisms are already in place:
1. `std::clamp` in `GeneticEngine::breed()` constrains parameters to [0,1],
   preventing DC in parameter space.
2. The smoothing filter `smoothed = smoothed * rate + target * (1 - rate)`
   is a first-order IIR low-pass that converges to the target without bias.

For audio signal path (v2), add a high-pass DC blocker:

```cpp
// First-order DC blocker — add to audio processing chain if MorphSnap
// ever passes audio (currently it only controls parameters).
// R = 0.995 for 48 kHz; adjustable via sampleRate.
class DCBlocker {
    float x1 { 0.0f }, y1 { 0.0f };
    float R { 0.995f };
public:
    void prepare(double sr) noexcept {
        R = 1.0f - (juce::MathConstants<float>::pi * 2.0f * 10.0f / static_cast<float>(sr));
    }
    float process(float x) noexcept {
        float y = x - x1 + R * y1;
        x1 = x; y1 = y;
        return y;
    }
};
```

#### Denormal Suppression

JUCE's `ScopedNoDenormals` sets the CPU's FTZ (Flush-To-Zero) and DAZ
(Denormals-Are-Zero) MXCSR bits for x86, and the equivalent FPCR bits
on ARM. Use it at the top of processBlock:

```cpp
void MorphSnapProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                       juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;  // Must be first line
    // ... rest of processBlock
}
```

The physics engine spring-damper state approaches zero when at rest,
creating denormal candidates. With FTZ enabled these flush to zero with
no CPU overhead.

#### Signal Level Monitoring

```cpp
// Existing: rmsLevel_ atomic updated each block in processBlock.
// For v2, add peak hold with configurable release time:
class LevelMeter {
    std::atomic<float> rms_    { 0.0f };
    std::atomic<float> peak_   { 0.0f };
    float peakHoldSamples_ = 0.0f;
    static constexpr float HOLD_TIME_SECONDS = 3.0f;
    static constexpr float RELEASE_DB_PER_BLOCK = 0.3f;

public:
    void process(const float* data, int numSamples, float sampleRate) noexcept {
        float rms = 0.0f;
        float peak = 0.0f;
        for (int i = 0; i < numSamples; ++i) {
            float abs = std::abs(data[i]);
            rms += data[i] * data[i];
            peak = std::max(peak, abs);
        }
        rms = std::sqrt(rms / numSamples);
        rms_.store(rms, std::memory_order_relaxed);

        float prevPeak = peak_.load(std::memory_order_relaxed);
        if (peak > prevPeak) {
            peak_.store(peak, std::memory_order_relaxed);
            peakHoldSamples_ = HOLD_TIME_SECONDS * sampleRate;
        } else if (peakHoldSamples_ > 0) {
            peakHoldSamples_ -= numSamples;
        } else {
            float released = prevPeak * std::pow(10.0f, -RELEASE_DB_PER_BLOCK / 20.0f);
            peak_.store(released, std::memory_order_relaxed);
        }
    }
};
```

#### Bypass Behavior

MorphSnap implements **latency-compensated bypass**, not true bypass:

- **True bypass**: Removes the plugin from the signal path entirely. Not
  possible from within a plugin — the DAW controls this.
- **Latency-compensated bypass**: When bypass is active, the plugin still
  processes the audio buffer but copies input to output without morphing.
  It continues to report the same latency so the DAW's delay compensation
  remains stable.

```cpp
// In processBlock:
if (apvts.getRawParameterValue("bypass")->load() > 0.5f) {
    // Latency-compensated bypass: copy input to output, apply no morph.
    // Latency reporting stays unchanged — DAW compensation stays valid.
    return; // buffer already contains input from host
}
```

---

## Part B — Development Workflow

### Area 8.1: Build System

See `CMakePresets.json` for the complete preset definitions.

#### CMake Modernization for v2

New dependencies for v2 via FetchContent:

```cmake
# FFTW3 alternative: pffft (public domain, no GPL, header-friendly)
FetchContent_Declare(pffft
    GIT_REPOSITORY https://bitbucket.org/jpommier/pffft.git
    GIT_TAG        main
    GIT_SHALLOW    TRUE
)
set(PFFFT_BUILD_TESTS OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(pffft)

# Eigen (header-only linear algebra — for spectral bin manipulation)
FetchContent_Declare(eigen
    GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
    GIT_TAG        3.4.0
    GIT_SHALLOW    TRUE
)
set(EIGEN_BUILD_DOC         OFF CACHE INTERNAL "")
set(BUILD_TESTING           OFF CACHE INTERNAL "")
set(EIGEN_BUILD_PKGCONFIG   OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(eigen)

# ONNX Runtime (pre-built binaries — DO NOT build from source in CI)
# Download platform-specific release tarball and extract:
#   Windows: onnxruntime-win-x64-1.17.0.zip
#   macOS:   onnxruntime-osx-universal2-1.17.0.tgz
# Set ONNXRUNTIME_ROOT to extracted directory.
find_library(ONNXRUNTIME_LIB
    NAMES onnxruntime
    HINTS ${ONNXRUNTIME_ROOT}/lib
    REQUIRED
)
target_link_libraries(MorphSnap PRIVATE ${ONNXRUNTIME_LIB})
target_include_directories(MorphSnap PRIVATE ${ONNXRUNTIME_ROOT}/include)
```

Package management rationale:
- **FetchContent** (current): Best for header-only and simple CMake projects.
  Avoids version lock-in. Suitable for JUCE, json, Catch2, Eigen, pffft.
- **Pre-built binaries** (ONNX Runtime): ONNX Runtime takes 45+ minutes
  to build from source. Always use pre-built official releases.
- **vcpkg**: Not recommended — JUCE does not ship a vcpkg port and the
  custom JUCE CMake integration conflicts with vcpkg's toolchain injection.

#### Cross-Compilation

Windows ARM64 (for Windows on ARM devices):

```cmake
# cmake --preset windows-msvc-release -A ARM64
# Requires Visual Studio ARM64 toolchain component installed.
# JUCE supports Windows ARM64 from JUCE 7+.
# NOTE: OversamplingWrapper must use NEON path on ARM64 Windows.
```

macOS Universal Binary: handled by `cmake/cmake-presets.json`
`macos-universal-release` preset with `CMAKE_OSX_ARCHITECTURES=x86_64;arm64`.

---

### Area 8.2: CI/CD Pipeline

See `.github/workflows/ci.yml` for the full implementation.

Key design decisions:
- **`macos-14` runner** (Apple Silicon): Can build and run arm64 natively
  and cross-compile x64. Faster than `macos-13` for universal builds.
- **`windows-2022`**: MSVC 17.x (VS2022), matching the production build tool.
- **FetchContent cache** keyed on `CMakeLists.txt` hash: avoids re-downloading
  JUCE (380 MB) on every run.
- **pluginval strictness 5**: The minimum for production release. Strictness
  10 would be ideal but currently fails on some JUCE-generated metadata.
- **Concurrency group with `cancel-in-progress`**: Feature branch pushes
  cancel in-flight CI runs, saving CI minutes.

---

### Area 8.3: Testing Methodology

#### Unit Tests (Catch2 v3)

Files:
- `tests/Unit/test_morphsnap_unit.cpp` — LFQ, ParameterState, InterpolationEngine, SnapshotBank
- `tests/Unit/TestPhysicsAndGenetic.cpp` — PhysicsEngine, GeneticEngine, SanityMode
- `tests/Unit/TestAudioEngine.cpp` — OversamplingWrapper, buffer size invariants, SNR
- `tests/Unit/TestDSPQuality.cpp` — LatencyManager, aliasing, monotonicity, smoothing
- `tests/Unit/TestSIMDOperations.cpp` — SIMD correctness at boundary sizes
- `tests/Unit/TestSidechainTrigger.cpp` — sidechain threshold detection
- `tests/Unit/TestAIStatusPanel.cpp` — AIStatusPanel UI state

**DSP correctness requirements**:
- Interpolation output must match scalar reference within 1e-5f (float rounding)
- Smoothing convergence: within 1% of target after 200 steps at rate=0.95
- Genetic engine output: clamped to [0.0, 1.0] at any mutation strength
- Physics: elastic state converges within 0.15 of target after 200 steps at 60 fps

**Thread safety testing with TSan**:

```bash
cmake --preset linux-clang-asan -DCMAKE_CXX_FLAGS="-fsanitize=thread"
cmake --build build/linux-clang-asan
cd build/linux-clang-asan && ctest
```

The SPSC LockFreeQueue test (`TestCase "SPSC producer-consumer"`) exercises
the actual concurrent producer-consumer pattern and will catch data races
under TSan.

#### Integration Tests

`tests/Integration/` — Plugin lifecycle:
- Load MorphSnap as a standalone AudioProcessor
- Call prepareToPlay(48000, 512) → processBlock(silence) → releaseResources()
- Verify no assertion failures, no memory leaks (run under ASAN)
- State save/restore round-trip: getStateInformation → setStateInformation
  → verify all parameters match

#### Audio Quality Tests

These are implemented in `tests/Unit/TestDSPQuality.cpp` and
`tests/Unit/TestAudioEngine.cpp`:

- **SNR measurement**: x4 FIR round-trip must achieve > 80 dB SNR
- **Aliasing detection**: No significant energy above 22 kHz after processing
  a 20 kHz sine with x4 oversampling
- **Latency verification**: LatencyManager total must equal sum of components
- **Monotonicity**: 1D interpolation output must be monotonic for linear snapshots

#### Fuzzing

For v2, add fuzzing with libFuzzer (Clang) targeting:

```cpp
// tests/Fuzz/FuzzPresetParser.cpp
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    juce::MemoryBlock block(data, size);
    morphsnap::MorphSnapProcessor processor;
    // setStateInformation must not crash on arbitrary input
    processor.setStateInformation(data, static_cast<int>(size));
    return 0;
}

// tests/Fuzz/FuzzMCPHandler.cpp
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Feed arbitrary bytes as a JSON-RPC message
    std::string input(reinterpret_cast<const char*>(data), size);
    morphsnap::MCPToolHandler handler(/* mock processor */);
    // Must not crash, throw, or access violate
    handler.processMessage(input);
    return 0;
}
```

Build with:
```bash
clang++ -fsanitize=fuzzer,address -std=c++20 \
    tests/Fuzz/FuzzPresetParser.cpp -o fuzz_preset \
    -I src [link flags]
./fuzz_preset -max_total_time=3600 corpus/preset/
```

---

### Area 8.4: Code Quality

See `.clang-tidy` in the repository root for the complete configuration.

#### Audio-Specific Static Analysis Rules

The most critical rules for audio-thread safety:

| Check | What it catches | Action |
|-------|----------------|--------|
| `cppcoreguidelines-pro-type-reinterpret-cast` | Unsafe casts | Error |
| `misc-throw-by-value-catch-by-reference` | Exception in noexcept context | Error |
| `clang-analyzer-cplusplus.NewDelete` | Dynamic allocation on audio thread | Error |
| `performance-unnecessary-copy-initialization` | Accidental copies of large objects | Error |
| `concurrency-mt-unsafe` | Non-thread-safe C functions | Error |

**Custom check for audio thread (manual code review rule)**:
Functions marked `noexcept` in the audio path must not call:
- `new`, `delete`, `malloc`, `free`
- `std::vector::push_back` (may reallocate)
- `std::string` construction (heap allocates)
- `juce::String` construction
- Any `juce::Logger` or `DBG()` calls
- `std::mutex::lock()` (blocks)

The `AllocationTracker.h` already provides a runtime assertion for debug builds.

#### Code Coverage

```bash
# Configure with coverage (GCC/Clang)
cmake -B build-cov \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="--coverage -fprofile-arcs -ftest-coverage"

cmake --build build-cov --target MorphSnapTests

cd build-cov && ctest

# Generate HTML report
lcov --capture --directory . --output-file coverage.info
lcov --remove coverage.info '*/_deps/*' '*/tests/*' --output-file coverage-filtered.info
genhtml coverage-filtered.info --output-directory coverage-html
```

Coverage targets:
- Core DSP (MorphProcessor, InterpolationEngine, PhysicsEngine): > 90%
- GeneticEngine: > 95% (already has comprehensive tests)
- MCPServer/Handler: > 70% (integration tested separately)
- UI components: > 50% (UI testing is expensive; cover logic, not paint)

#### Documentation (Doxygen)

```ini
# docs/Doxyfile (key settings)
PROJECT_NAME           = "MorphSnap Audio Engine"
PROJECT_NUMBER         = 3.3.0
OUTPUT_DIRECTORY       = docs/api
INPUT                  = src/
RECURSIVE              = YES
EXTRACT_ALL            = NO
EXTRACT_PRIVATE        = NO
GENERATE_HTML          = YES
GENERATE_LATEX         = NO
ENABLE_PREPROCESSING   = YES
MACRO_EXPANSION        = YES
PREDEFINED             = DOXYGEN_SHOULD_SKIP_THIS \
                         MORPHSNAP_TEST_MODE=0

# Only document public API — not internal implementation details
EXTRACT_STATIC         = NO
HIDE_UNDOC_MEMBERS     = YES
```

Run: `doxygen docs/Doxyfile` — output in `docs/api/html/index.html`.

---

### Area 8.5: Version Control Strategy

#### Branch Strategy

```
main (or mvp)    Production-ready releases only. Protected.
develop          Integration branch. All features merge here first.
feature/*        One feature per branch. Rebased onto develop before PR.
release/v3.4.0   Short-lived. Stabilization only — no new features.
hotfix/*         Critical production fixes. Branch from main, PR to both.
```

#### Semantic Versioning Rules

```
MAJOR.MINOR.PATCH  (e.g., 3.3.0)

MAJOR: Breaking change to preset format, MCP API contract, or
       minimum supported OS/DAW version.

MINOR: New feature (new morph mode, new AI tool, new UI panel)
       that is backward compatible. Old presets still load.

PATCH: Bug fix only. No new API surface. No behavior changes
       observable by the user except fixing the reported bug.
```

Pre-release suffixes: `-alpha.1`, `-beta.2`, `-rc.1`

#### Changelog Generation

Use `git cliff` (https://git-cliff.org) with conventional commits:

```toml
# cliff.toml
[git]
conventional_commits = true
commit_parsers = [
  { message = "^feat",     group = "Features" },
  { message = "^fix",      group = "Bug Fixes" },
  { message = "^perf",     group = "Performance" },
  { message = "^refactor", group = "Refactoring" },
  { message = "^test",     group = "Tests",   skip = true },
  { message = "^chore",    group = "Chores",  skip = true },
  { message = "^doc",      group = "Documentation" },
]
```

Run: `git cliff --tag v3.4.0 --output CHANGELOG.md`

#### Release Tagging and Artifact Naming

```bash
# Tag a release:
git tag -a v3.4.0 -m "Release v3.4.0 — Spectral morphing beta"
git push origin v3.4.0

# CI automatically triggers publish-release job.
# Artifacts named:
#   MorphSnap-3.4.0-Windows-x64.zip      (VST3)
#   MorphSnap-3.4.0-macOS-Universal.zip  (VST3 + AU)
```

For pre-releases:
```bash
git tag -a v3.4.0-rc.1 -m "Release candidate 1"
# CI sets prerelease=true when tag contains -rc or -beta
```

---

## Implementation Checklist for v2

### Audio Engine

- [ ] Integrate OversamplingWrapper into MorphSnapProcessor::prepareToPlay
- [ ] Integrate LatencyManager and call setLatencySamples() correctly
- [ ] Add NEON SIMD path to applySmoothing for Apple Silicon
- [ ] Implement DCBlocker for audio pass-through path
- [ ] Add GrainPool for spectral grain engine
- [ ] Add FFT ring buffer for variable block size handling
- [ ] Wire LevelMeter peak hold to AIStatusPanel
- [ ] Add TPDF dither for sub-24-bit delivery contexts

### Testing

- [ ] Add TestAudioEngine.cpp to tests/CMakeLists.txt
- [ ] Add TestDSPQuality.cpp to tests/CMakeLists.txt
- [ ] Set up lcov coverage report in CI
- [ ] Add fuzzing targets (FuzzPresetParser, FuzzMCPHandler)
- [ ] Configure TSan run in CI (Linux Clang, separate job)

### Build System

- [ ] Commit CMakePresets.json
- [ ] Update .github/workflows/ci.yml (replace old workflow)
- [ ] Commit .clang-tidy
- [ ] Add ONNX Runtime download step to CI
- [ ] Add pffft and Eigen to CMakeLists.txt FetchContent
- [ ] Add Doxyfile and `docs/api/` to .gitignore

### Code Quality

- [ ] Run clang-tidy on all src/ files, fix errors
- [ ] Audit processBlock for any remaining allocations
- [ ] Add Doxygen comments to all public API headers
- [ ] Set up git cliff for changelog generation
