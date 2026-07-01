@echo off
REM build-tests-lowpar.bat — build MorePhiTests with reduced parallelism to avoid
REM MSVC C1060 (out of heap space) on memory-constrained machines. Invoked by
REM the audit fix workflow because the default ninja parallelism exhausts MSVC's
REM heap when compiling the large SonicMaster / JUCE TUs concurrently.
REM Usage: build-tests-lowpar.bat [target] [ninja-args...]
setlocal
set "VS=C:\Program Files\Microsoft Visual Studio\18\Enterprise"
set "VCVARS=%VS%\VC\Auxiliary\Build\vcvars64.bat"
set "VSCMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "PATH=%VSCMAKE%;%PATH%"
set "SRC=%~dp0"
if "%SRC:~-1%"=="\" set "SRC=%SRC:~0,-1%"
set "BUILD=%SRC%\build-ninja"
set "TARGET=%1"
if "%TARGET%"=="" set "TARGET=MorePhiTests"
call "%VCVARS%" >nul
if errorlevel 1 ( echo [lowpar] ERROR: vcvars64 failed & exit /b 1 )
cmake --build "%BUILD%" --target %TARGET% -- -j 2 %2 %3 %4 %5
endlocal
