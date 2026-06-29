@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d G:\More_Phi-vst3-plugin
cmake --build build-validate --target MorePhi --config Debug
exit /b %errorlevel%
