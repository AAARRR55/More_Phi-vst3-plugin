# Full CTest Validation - windows-msvc-release - 2026-06-15

## Command

```powershell
ctest --build-config Release --output-on-failure --parallel 4 -O "G:\More_Phi-vst3-plugin\validation\ctest_full_windows-msvc-release_20260615.log"
```

## Environment

| Field | Value |
|-------|-------|
| Build tree | `G:\More_Phi-vst3-plugin\build\windows-msvc-release` |
| Build config | `Release` |
| CTest registry | 458 tests |
| Local raw log | `validation/ctest_full_windows-msvc-release_20260615.log` |

The raw `.log` file is intentionally not tracked because repository ignore rules exclude `*.log`.

## Result

```text
100% tests passed, 0 tests failed out of 458

Total Test time (real) =  60.83 sec
```

## Scope Boundary

This validates the current compiled CTest target in `build/windows-msvc-release`.
It does not validate external Steinberg `vst3_validator`, `pluginval`, DAW-host behavior, or 10K dataset generation/runtime memory behavior.
