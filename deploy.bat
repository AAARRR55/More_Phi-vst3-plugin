@echo off
:: MorphSnap — Deploy VST3 to system folder
:: Double-click to run. Auto-elevates to admin.

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Elevating to admin...
    powershell -Command "Start-Process cmd -ArgumentList '/c \"%~f0\"' -Verb RunAs"
    exit /b
)

set SRC=d:\morphy\build\MorphSnap_artefacts\Release\VST3\MorphSnap.vst3
set DST=C:\Program Files\Common Files\VST3\MorphSnap.vst3

if not exist "%SRC%" (
    echo ERROR: Build not found at %SRC%
    echo Run: cmake --build build --config Release
    pause
    exit /b 1
)

echo Deploying MorphSnap VST3...
xcopy "%SRC%" "%DST%" /E /Y /I /Q
echo OK: Deployed to %DST%
pause
