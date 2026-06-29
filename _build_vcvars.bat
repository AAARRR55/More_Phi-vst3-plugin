@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d G:\More_Phi-vst3-plugin
cmake --build build-ninja --config Release --target MorePhiTests > build-ninja\_build2.log 2>&1
exit /b %errorlevel%
