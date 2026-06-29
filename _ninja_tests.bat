@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d G:\More_Phi-vst3-plugin\build-ninja
"C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" MorePhiTests %*
if %ERRORLEVEL% NEQ 0 exit /b %ERRORLEVEL%
ctest --build-config Release --output-on-failure --parallel 4 2>&1
