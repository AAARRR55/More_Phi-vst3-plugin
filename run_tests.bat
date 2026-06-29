@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
"C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" -C "G:\More_Phi-vst3-plugin\build-ninja" -j2 MorePhiTests
if errorlevel 1 exit /b 1
cd /d "G:\More_Phi-vst3-plugin\build-ninja\tests"
MorePhiTests.exe "[rulebased]"
