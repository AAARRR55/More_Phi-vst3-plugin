@echo off
REM ===========================================================================
REM  release-validate.bat — local B3-gate validation for More-Phi (Windows)
REM ===========================================================================
REM  Purpose:
REM    Reproduces the Windows slice of .github/workflows/ci.yml locally, for use
REM    when CI hasn't run yet (pre-push) or when iterating without round-tripping
REM    through GitHub. Closes the build / ctest / pluginval gates on this host.
REM
REM  Usage (from repo root, Git-Bash or cmd):
REM    cmd.exe //c "G:\More_Phi-vst3-plugin\tools\release-validate.bat" [stage]
REM
REM    NOTE the double-slash: MSYS/Git-Bash mangles a single "/c" and silently
REM    drops the .bat invocation. This quirk is the reason this script exists
REM    as a .bat rather than inline shell.
REM
REM    stage (optional, default = all):
REM      build    configure + Release build only
REM      test     build + full ctest suite
REM      pluginval build + test + pluginval strictness-5
REM      all      (default) build + test + pluginval
REM
REM  Exit codes:
REM    0 = all requested stages passed
REM    2 = VS dev environment init failed
REM    3 = configure failed
REM    4 = build failed
REM    5 = VST3 artefact missing after build
REM    6 = ctest failures
REM    7 = pluginval failures
REM ===========================================================================
setlocal enabledelayedexpansion

set STAGE=%1
if "%STAGE%"=="" set STAGE=all

set VSDIR=C:\Program Files\Microsoft Visual Studio\18\Enterprise
set VST3=build\MorePhi_artefacts\Release\VST3\MorePhi.vst3
set PLUGINVAL=tools\pluginval.exe
set PV_REPORT=validation\pluginval_strictness5.txt

REM ── VS dev environment ───────────────────────────────────────────────────
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul 2>&1
if errorlevel 1 ( echo [FAIL] VsDevCmd init & exit /b 2 )
cd /d "%~dp0\.."

REM ── Stage: build ─────────────────────────────────────────────────────────
echo === CONFIGURE ===
cmake -B build -S . -DMORE_PHI_BUILD_TESTS=ON
if errorlevel 1 ( echo [FAIL] configure & exit /b 3 )

echo === BUILD Release ===
cmake --build build --config Release --parallel
if errorlevel 1 ( echo [FAIL] build & exit /b 4 )

if not exist "%VST3%" ( echo [FAIL] %VST3% missing & exit /b 5 )
echo [OK] %VST3% built

if /i "%STAGE%"=="build" goto done

REM ── Stage: test ──────────────────────────────────────────────────────────
echo === CTEST (Release, parallel 4) ===
ctest --test-dir build --build-config Release --output-on-failure --parallel 4
if errorlevel 1 ( echo [FAIL] ctest & exit /b 6 )
echo [OK] ctest suite passed

if /i "%STAGE%"=="test" goto done

REM ── Stage: pluginval ─────────────────────────────────────────────────────
if not exist "%PLUGINVAL%" (
    echo [SKIP] %PLUGINVAL% not found — install from
    echo        https://github.com/Tracktion/pluginval/releases (expected at tools\)
    goto done
)
echo === PLUGINVAL strictness 5 ===
"%PLUGINVAL%" --strictness-level 5 --validate "%VST3%" --output-dir validation\ > "%PV_REPORT%" 2>&1
if errorlevel 1 ( echo [FAIL] pluginval — see %PV_REPORT% & exit /b 7 )
findstr /B /C:"SUCCESS" "%PV_REPORT%" >nul
if errorlevel 1 ( echo [FAIL] pluginval no SUCCESS line & exit /b 7 )
echo [OK] pluginval strictness-5 passed

:done
echo.
echo === release-validate [%STAGE%] PASSED ===
exit /b 0
