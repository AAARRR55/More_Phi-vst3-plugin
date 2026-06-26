@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
"C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" -C "G:\More_Phi-vst3-plugin\build-ninja" -j1 CMakeFiles/MorePhi.dir/src/AI/MCPToolHandler.cpp.obj > "G:\More_Phi-vst3-plugin\mcptool_build.log" 2>&1
echo [build] ninja exit code: %ERRORLEVEL%
