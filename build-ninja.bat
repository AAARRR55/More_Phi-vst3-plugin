@echo off
REM build-ninja.bat — configure + build More-Phi with the Ninja generator under MSVC.
REM Usage: build-ninja.bat [configure|build|tests|clean]
REM
REM Ninja gives faster, more parallel builds than the Visual Studio generator and
REM avoids the .tlog file-lock thrash that hangs parallel VS builds.
REM Uses the VS-bundled ninja.exe so no separate install is needed.

setlocal

set "VS=C:\Program Files\Microsoft Visual Studio\18\Enterprise"
set "VCVARS=%VS%\VC\Auxiliary\Build\vcvars64.bat"
set "VSCMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "PATH=%VSCMAKE%;%PATH%"
REM %~dp0 ends with a trailing backslash that escapes the next quote char in
REM "-S "%SRC%"" — strip it.
set "SRC=%~dp0"
if "%SRC:~-1%"=="\" set "SRC=%SRC:~0,-1%"
set "BUILD=%SRC%\build-ninja"

if not exist "%VCVARS%" (
    echo [build-ninja] ERROR: vcvars64.bat not found at %VCVARS%
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 ( echo [build-ninja] ERROR: vcvars64 failed & exit /b 1 )

set "ACTION=%1"
if "%ACTION%"=="" set "ACTION=build"

if /i "%ACTION%"=="configure" (
    cmake -B "%BUILD%" -S "%SRC%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DMORE_PHI_BUILD_TESTS=ON
    goto :done
)
if /i "%ACTION%"=="clean" (
    if exist "%BUILD%" rmdir /s /q "%BUILD%"
    goto :done
)
if /i "%ACTION%"=="tests" (
    if not exist "%BUILD%\build.ninja" cmake -B "%BUILD%" -S "%SRC%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DMORE_PHI_BUILD_TESTS=ON || exit /b 1
    cmake --build "%BUILD%" --target MorePhiTests || exit /b 1
    cd /d "%BUILD%" && ctest --output-on-failure
    goto :done
)
if /i "%ACTION%"=="testonly" (
    cd /d "%BUILD%" && ctest %2 %3 %4 %5 %6
    goto :done
)

REM default: build the DAW-loadable VST3 plugin (not just SharedCode).
REM SharedCode is an intermediate lib; the VST3 target links it into the .vst3
REM bundle you actually load into a DAW. Use "target MorePhi" if you only want
REM the lib (faster, e.g. for a quick compile check).
if not exist "%BUILD%\build.ninja" cmake -B "%BUILD%" -S "%SRC%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DMORE_PHI_BUILD_TESTS=ON || exit /b 1
if /i "%ACTION%"=="target" (
    cmake --build "%BUILD%" --target %2
    goto :done
)
cmake --build "%BUILD%" --target MorePhi_VST3

:done
endlocal