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
REM CRASH-DIAG (2026-06-29): copy the PDB next to the installed .vst3 so FL
REM crash dumps (%LOCALAPPDATA%\CrashDumps\FL64.exe.*.dmp) resolve to source
REM lines when opened in a debugger. The PDB is matched by GUID + age, so it
REM must be the exact one produced with this binary — re-deploy after every
REM rebuild or the dump won't symbolicate. Optional: delete the .pdb here for a
REM stripped install, at the cost of un-diagnosable crashes.
if exist "%SRC%\..\..\MorePhi.pdb" (
    copy /Y "%SRC%\..\..\MorePhi.pdb" "%DST%\MorePhi.pdb" || echo [warn] PDB copy failed (non-fatal — crashes just won't symbolicate)
)
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
