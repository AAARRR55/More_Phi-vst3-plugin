@echo off
REM Deploy the delay-load-fixed MorePhi plugin to the system VST3 path.
REM Requires elevation because C:\Program Files is admin-owned, and FL must
REM be closed so the binary isn't locked.
setlocal
set "SRC=G:\More_Phi-vst3-plugin\build-ninja\MorePhi_artefacts\Release\VST3\MorePhi.vst3\Contents\x86_64-win"
set "DST=C:\Program Files\Common Files\VST3\MorePhi.vst3\Contents\x86_64-win"

echo Checking FL Studio is not running...
tasklist 2>NUL | findstr /I "FL64.exe FL32.exe" >NUL
if %ERRORLEVEL% == 0 (
    echo.
    echo [ERROR] FL Studio is still running and holding the plugin locked.
    echo Please fully close FL Studio, then re-run this script.
    pause
    exit /b 1
)

echo Deploying fixed binary to "%DST%"
copy /Y "%SRC%\MorePhi.vst3" "%DST%\MorePhi.vst3" || goto :needadmin
copy /Y "%SRC%\onnxruntime.dll" "%DST%\onnxruntime.dll" || goto :needadmin
copy /Y "%SRC%\onnxruntime_providers_shared.dll" "%DST%\onnxruntime_providers_shared.dll" || goto :needadmin
echo.
echo [OK] Deployed. You can now open FL Studio and load MorePhi.
dir "%DST%"
pause
exit /b 0

:needadmin
echo.
echo [Need admin] Copy denied. Re-launching elevated...
powershell -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
exit /b %ERRORLEVEL%
