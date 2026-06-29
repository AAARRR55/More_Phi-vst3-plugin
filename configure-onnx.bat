@echo off
set "VS=C:\Program Files\Microsoft Visual Studio\18\Enterprise"
set "VCVARS=%VS%\VC\Auxiliary\Build\vcvars64.bat"
set "VSCMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "PATH=%VSCMAKE%;%PATH%"
call "%VCVARS%" >nul
cmake -B "G:\More_Phi-vst3-plugin\build-ninja" -S "G:\More_Phi-vst3-plugin" -G Ninja -DCMAKE_BUILD_TYPE=Release -DMORE_PHI_BUILD_TESTS=ON -DMORE_PHI_ENABLE_ONNX=ON
exit /b %errorlevel%
