# Build Stability Guide (Windows 11, 16 GB RAM)

This guide is focused on preventing system freezes during CMake builds on mid-range laptops.

## 1) Default safe workflow

Use the safe preset and keep top-level build parallelism at `2`.

```powershell
cmake --preset windows-msvc-safe
cmake --build --preset windows-safe --parallel 2
```

If the machine feels unstable, switch to single-job build:

```powershell
cmake --build --preset windows-single --parallel 1
```

## 2) Preflight checks before heavy builds

1. Keep `C:` free space at `>= 25 GB`.
2. Keep pagefile enabled and sized to roughly `24-32 GB` for a 16 GB RAM system.
3. Prefer building on a drive with more headroom (for example `D:`):

```powershell
cmake -S . -B D:/morphy-build -G "Visual Studio 17 2022" -A x64 `
  -DMORPHSNAP_SAFE_BUILD_MODE=ON -DMORPHSNAP_MSVC_MP=2 -DMORPHSNAP_ENABLE_LTO=OFF
cmake --build D:/morphy-build --config Release --parallel 2
```

## 3) Run diagnostic matrix with resource monitoring

The script below captures CPU, available RAM, pagefile pressure, disk queue, and effective CPU clock ratio during each build run.

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\scripts\diagnose-build-freeze.ps1 -BuildPreset windows-safe -ParallelJobs 1,2,4 -SampleSeconds 2
```

Output:
- Per-run logs in `build/diagnostics/parallel-*/`
- `build/diagnostics/diagnostics-summary.json` with classified causes

## 4) How causes are classified

- `resource-exhaustion`: low available RAM and high pagefile activity
- `excessive-parallel-jobs`: failures or severe pressure at high `--parallel`
- `swap-or-disk-pressure`: low free disk and/or sustained disk queue growth
- `possible-thermal-throttling`: high CPU load with low clock ratio
- `cmake-or-toolchain-configuration-pressure`: link/LTO or compiler memory signatures in logs
- `system-level-instability-signals`: recent Kernel-Power (41) / WHEA (18)

## 5) CMake knobs for controlled tuning

Configured in `CMakeLists.txt`:

- `MORPHSNAP_SAFE_BUILD_MODE` (default `ON`)
- `MORPHSNAP_MSVC_MP` (default `2`, set `0` to disable `/MP`)
- `MORPHSNAP_ENABLE_LTO` (default `OFF` for local stability)

Recommended local values:

```powershell
-DMORPHSNAP_SAFE_BUILD_MODE=ON -DMORPHSNAP_MSVC_MP=2 -DMORPHSNAP_ENABLE_LTO=OFF
```

For aggressive CI/release only:

```powershell
-DMORPHSNAP_SAFE_BUILD_MODE=OFF -DMORPHSNAP_ENABLE_LTO=ON
```

## 6) Acceptance checks

1. Three consecutive `windows-safe` release builds complete without freezing.
2. Available RAM stays above `~1 GB` during builds.
3. Pagefile and disk queue do not climb continuously across runs.
4. CPU clock ratio stays stable at sustained load.
5. `--parallel 4` behavior is documented; `--parallel 2` remains the default safe point.
