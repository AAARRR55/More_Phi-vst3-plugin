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

If CMake reports a stale Visual Studio generator instance, refresh the build
directory before retrying:

```powershell
cmake --preset windows-msvc-safe --fresh
cmake --build --preset windows-single --parallel 1
```

If `vswhere` does not list Build Tools but `cl.exe` and `nmake.exe` exist on
disk, use the Developer Command Prompt environment directly as a diagnostic
fallback:

```powershell
cmd /d /s /c "`"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat`" && cmake --preset windows-nmake-release --fresh && cmake --build --preset windows-nmake-single"
```

If that fails with `cl : Command line error D8037 : cannot create temporary il
file`, first point `TMP` and `TEMP` at a clean workspace temp folder. If the
error persists, the compiler installation or Windows provider stack needs repair
before More-Phi can be validated locally. Microsoft documents D8037 as a
temporary-file creation failure:
<https://learn.microsoft.com/en-us/cpp/error-messages/tool-errors/command-line-error-d8037>.

## 2) Preflight checks before heavy builds

1. Keep `C:` free space at `>= 25 GB`.
2. Keep pagefile enabled and sized to roughly `24-32 GB` for a 16 GB RAM system.
3. Verify Visual Studio Build Tools are registered: `vswhere` should list the 2022 C++ build tools installation, and `MSBuild` should be available from a Developer PowerShell.
4. Prefer building on a drive with more headroom (for example `D:`):

```powershell
cmake -S . -B D:/morphy-build -G "Visual Studio 17 2022" -A x64 `
  -DMORE_PHI_SAFE_BUILD_MODE=ON -DMORE_PHI_MSVC_MP=2 -DMORE_PHI_ENABLE_LTO=OFF
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

- `MORE_PHI_SAFE_BUILD_MODE` (default `ON`)
- `MORE_PHI_MSVC_MP` (default `2`, set `0` to disable `/MP`)
- `MORE_PHI_ENABLE_LTO` (default `OFF` for local stability)

Recommended local values:

```powershell
-DMORE_PHI_SAFE_BUILD_MODE=ON -DMORE_PHI_MSVC_MP=2 -DMORE_PHI_ENABLE_LTO=OFF
```

For aggressive CI/release only:

```powershell
-DMORE_PHI_SAFE_BUILD_MODE=OFF -DMORE_PHI_ENABLE_LTO=ON
```

## 6) Acceptance checks

1. Three consecutive `windows-safe` release builds complete without freezing.
2. Available RAM stays above `~1 GB` during builds.
3. Pagefile and disk queue do not climb continuously across runs.
4. CPU clock ratio stays stable at sustained load.
5. `--parallel 4` behavior is documented; `--parallel 2` remains the default safe point.
