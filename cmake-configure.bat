@echo off
REM cmake-configure.bat — bare cmake configure for the Ninja build dir, used when
REM build-ninja.bat configure's ninja restat step hits a transient permission
REM error on Windows file locks.
setlocal
set "VS=C:\Program Files\Microsoft Visual Studio\18\Enterprise"
set "VCVARS=%VS%\VC\Auxiliary\Build\vcvars64.bat"
set "VSCMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "PATH=%VSCMAKE%;%PATH%"
set "SRC=%~dp0"
if "%SRC:~-1%"=="\" set "SRC=%SRC:~0,-1%"
set "BUILD=%SRC%\build-ninja"
call "%VCVARS%" >nul
if errorlevel 1 ( echo [configure] ERROR: vcvars64 failed & exit /b 1 )
cmake -B "%BUILD%" -S "%SRC%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DMORE_PHI_BUILD_TESTS=ON
endlocal
