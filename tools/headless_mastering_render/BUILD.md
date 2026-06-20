## Build Plan — Compile the Harness on the Lightning Linux Box

The Lightning studio has only the flat Python control package (`morephi-control/`), NOT the C++ repo. So the harness must be built on a box with the full repo (clone to Lightning for Linux builds), then the resulting `.so` copied next to the Python teacher.

### 0. Preconditions (on the Lightning build box)
- Ubuntu 22.04+ / any glibc>=2.31 Linux (the .so will be `dlopen`ed by Python).
- CMake >= 3.22, a C++20 compiler (gcc>=11 or clang>=14), git, `pkg-config`.
- CMA-Scale: ~2 GB RAM, ~5 GB disk for the JUCE/nlohmann FetchContent cache.

### 1. Clone the repo (full source tree required)
```bash
git clone <more-phi-repo-url> ~/more-phi
cd ~/more-phi
git checkout feature/release-validation   # or the branch carrying the harness
```

### 2. Configure with the headless-render option ON, tests ON (for local validation), ONNX OFF
```bash
cmake -B build -S . \
    -DMORE_PHI_BUILD_TESTS=ON \
    -DMORE_PHI_BUILD_HEADLESS_RENDER=ON \
    -DMORE_PHI_ENABLE_ONNX=OFF \
    -DCMAKE_BUILD_TYPE=Release
```
Key flags:
- `MORE_PHI_BUILD_HEADLESS_RENDER=ON` — adds `tools/headless_mastering_render/` (the new target).
- `MORE_PHI_ENABLE_ONNX=OFF` — **MUST stay OFF.** The harness reuses `buildPlanCandidate`/`sanitizePlanCandidate`, which are pure ONNX-free transforms (`MORE_PHI_HAS_ONNX=0`, `OnnxNeuralMasteringRunner.cpp:19-21`). Flipping ONNX ON would download a 200 MB prebuilt binary via `cmake/FetchOnnxRuntime.cmake` and couple the .so to `libonnxruntime.so` at load time — unnecessary.
- Tests ON so the existing `TestAudioEngine` suite (which proves the offline pattern at `:381-411` and `:442-471`) compiles and can be run as a pre-flight sanity check.

### 3. Build ONLY the headless render target (fastest path)
```bash
cmake --build build --config Release --target more_phi_headless_render --parallel 4
```
The output is `build/libmore_phi_headless_render.so` (MODULE library, `extern "C"` + default visibility exports `render` / `morephi_headless_init` / `morephi_headless_shutdown` / `morephi_headless_chain_latency` undecorated).

First configure will FetchContent JUCE 8.0.4 + nlohmann/json 3.11.3 (~3-5 min one-time). Subsequent builds are incremental.

### 4. Pre-flight: run the source-side tests that prove the offline pattern
```bash
ctest --test-dir build --build-config Release -R "AutoMasteringEngine" --output-on-failure
```
Expected PASS: `AutoMasteringEngine publishes live analyzer snapshots` (the `:381-411` offline `processBlock` loop) and `AutoMasteringEngine applies only validated neural mastering plans` (the `:442-471` offline `applyValidatedPlan`). If these fail, the harness cannot work — fix the source first.

### 5. Verify the .so exports + runs headless
```bash
# Confirm the render symbol is exported undecorated:
nm -D build/libmore_phi_headless_render.so | grep render
# Expect: T morephi_headless_chain_latency / morephi_headless_init / morephi_headless_shutdown / render

# Smoke test (the harness self-test, no Python needed):
cd tools/headless_mastering_render
python3 morephi_render.py --lib ../../build/libmore_phi_headless_render.so --sr 48000
# Expect a printed chain_latency, rendered shape (n, 2), peak, and meters dict.

# Full parity test (determinism + re-entrancy + admissibility):
python3 test_render_parity.py --lib ../../build/libmore_phi_headless_render.so --sr 48000
# Expect: [PASS] determinism / re-entrancy / admissibility / zero-delta, then "ALL PASS".
```

### 6. Deploy next to the Python teacher
```bash
# Copy the .so to the Lightning control package directory (where the CMA-ES teacher runs):
cp build/libmore_phi_headless_render.so /path/to/morephi-control/lib/

# Also copy the Python binding + parity test:
cp tools/headless_mastering_render/morephi_render.py /path/to/morephi-control/
cp tools/headless_mastering_render/test_render_parity.py /path/to/morephi-control/
```

### 7. Use from the CMA-ES teacher
```python
from morephi_render import HeadlessRenderer

r = HeadlessRenderer("/path/to/libmore_phi_headless_render.so",
                     sample_rate=48000, block_size=512, normalizer_mode=0)

# In the CMA-ES objective:
def objective(delta72: np.ndarray) -> float:
    rendered, meters = r.render_candidate(unmastered_pcm, delta72)
    # ... compute perceptual target (e.g. spectral distance to reference, LUFS error) ...
    return loss

# IMPORTANT: pin one sample_rate across the whole search (determinism across arch).
# Wire only the 43 live slots in delta72; fix the 29 dead slots at 0
# (dynamics[4..7], stereo[4..7], harmonic[1..7], limiter[1..7], loudness[1..7]).
```

### Notes
- **Architecture pinning for reproducibility:** the SIMD tuning (`/arch:AVX2` on x86_64, scoped to `SIMDAudio.cpp` via `set_source_files_properties` at `CMakeLists.txt:734-744`) propagates automatically because it is bound to the absolute source path, not the target. Pin the build arch (e.g. build on the same x86_64 box that runs the teacher) for bit-reproducible CMA-ES runs. Cross-arch (x86_64 vs arm64) renders are NOT bit-identical.
- **No MessageManager crash:** the harness constructs `juce::ScopedJuceInitialiser_GUI` inside `morephi_headless_init` on the caller thread. Do NOT call `prepare(...,true)` — the harness always uses `prepare(sr, block, false)`.
- **stack:** Linux MODULE library has no /STACK concern (Windows-only). The Windows path adds `/STACK:4194304` in the CMake entry for local dev.
