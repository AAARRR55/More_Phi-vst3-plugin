@echo off
REM _buildonly.bat — build the test target only (configure already done).
setlocal
set "VS=C:\Program Files\Microsoft Visual Studio\18\Enterprise"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
cmake --build build-ninja --target MorePhiTests -- -j 2
exit /b %errorlevel%
