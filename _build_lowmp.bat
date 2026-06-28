@echo off
REM _build_lowmp.bat — configure + build with MORE_PHI_MSVC_MP=0 and low ninja
REM parallelism, to dodge the MSVC heap-exhaustion (C1060) that /MP8 + parallel
REM ninja triggers on this box. Internal helper, safe to delete.
setlocal
set "VS=C:\Program Files\Microsoft Visual Studio\18\Enterprise"
set "VCVARS=%VS%\VC\Auxiliary\Build\vcvars64.bat"
set "PATH=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
call "%VCVARS%" >nul
if errorlevel 1 ( echo [err] vcvars failed & exit /b 1 )
cmake -B build-ninja -S . -G Ninja -DCMAKE_BUILD_TYPE=Release -DMORE_PHI_BUILD_TESTS=ON -DMORE_PHI_ENABLE_ONNX=ON -DMORE_PHI_MSVC_MP=0
if errorlevel 1 ( echo [err] configure failed & exit /b 1 )
cmake --build build-ninja --target MorePhiTests -- -j 2
exit /b %errorlevel%
