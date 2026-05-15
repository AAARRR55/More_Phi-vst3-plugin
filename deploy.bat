@echo off
:: morphy — Deploy VST3 to standard DAW plugin folders
:: Usage:
::   deploy.bat          -> current user install (no admin)
::   deploy.bat machine  -> all users install (auto-elevates)

setlocal EnableExtensions

set REPO_ROOT=%~dp0
set SCOPE=%~1
if "%SCOPE%"=="" set SCOPE=user
set TARGET_BUNDLE=

set SRC=
if exist "%REPO_ROOT%build\morphy_artefacts\Release\VST3\MorePhi.vst3" set SRC=%REPO_ROOT%build\morphy_artefacts\Release\VST3\MorePhi.vst3
if not defined SRC if exist "%REPO_ROOT%build\MorePhi_artefacts\Release\VST3\MorePhi.vst3" set SRC=%REPO_ROOT%build\MorePhi_artefacts\Release\VST3\MorePhi.vst3
if not defined SRC if exist "%REPO_ROOT%build\windows-ninja-vs2026-safe\morphy_artefacts\Release\VST3\MorePhi.vst3" set SRC=%REPO_ROOT%build\windows-ninja-vs2026-safe\morphy_artefacts\Release\VST3\MorePhi.vst3
if not defined SRC if exist "%REPO_ROOT%build\windows-ninja-vs2026-safe\MorePhi_artefacts\Release\VST3\MorePhi.vst3" set SRC=%REPO_ROOT%build\windows-ninja-vs2026-safe\MorePhi_artefacts\Release\VST3\MorePhi.vst3
if not defined SRC if exist "%REPO_ROOT%build\windows-msvc-safe\morphy_artefacts\Release\VST3\MorePhi.vst3" set SRC=%REPO_ROOT%build\windows-msvc-safe\morphy_artefacts\Release\VST3\MorePhi.vst3
if not defined SRC if exist "%REPO_ROOT%build\windows-msvc-release\morphy_artefacts\Release\VST3\MorePhi.vst3" set SRC=%REPO_ROOT%build\windows-msvc-release\morphy_artefacts\Release\VST3\MorePhi.vst3
if not defined SRC if exist "%REPO_ROOT%build\MorePhi_artefacts\Release\VST3\MorePhi.vst3" set SRC=%REPO_ROOT%build\MorePhi_artefacts\Release\VST3\MorePhi.vst3
if not defined SRC if exist "%REPO_ROOT%build\windows-msvc-safe\MorePhi_artefacts\Release\VST3\MorePhi.vst3" set SRC=%REPO_ROOT%build\windows-msvc-safe\MorePhi_artefacts\Release\VST3\MorePhi.vst3
if not defined SRC if exist "%REPO_ROOT%build\windows-msvc-release\MorePhi_artefacts\Release\VST3\MorePhi.vst3" set SRC=%REPO_ROOT%build\windows-msvc-release\MorePhi_artefacts\Release\VST3\MorePhi.vst3

if defined SRC for %%I in ("%SRC%") do set TARGET_BUNDLE=%%~nxI

if /I "%SCOPE%"=="machine" (
    net session >nul 2>&1
    if %errorlevel% neq 0 (
        echo Elevating to admin for machine-wide install...
        powershell -Command "Start-Process cmd -ArgumentList '/c \"%~f0\" machine' -Verb RunAs"
        exit /b
    )
    set DST=%ProgramFiles%\Common Files\VST3\%TARGET_BUNDLE%
) else (
    set DST=%USERPROFILE%\Documents\VST3\%TARGET_BUNDLE%
)

if not defined SRC (
    echo ERROR: Could not find a plugin .vst3 bundle in expected build paths.
    echo Tried:
    echo   %REPO_ROOT%build\morphy_artefacts\Release\VST3\MorePhi.vst3
    echo   %REPO_ROOT%build\MorePhi_artefacts\Release\VST3\MorePhi.vst3
    echo   %REPO_ROOT%build\windows-ninja-vs2026-safe\morphy_artefacts\Release\VST3\MorePhi.vst3
    echo   %REPO_ROOT%build\windows-ninja-vs2026-safe\MorePhi_artefacts\Release\VST3\MorePhi.vst3
    echo   %REPO_ROOT%build\windows-msvc-safe\morphy_artefacts\Release\VST3\MorePhi.vst3
    echo   %REPO_ROOT%build\windows-msvc-release\morphy_artefacts\Release\VST3\MorePhi.vst3
    echo   %REPO_ROOT%build\MorePhi_artefacts\Release\VST3\MorePhi.vst3
    echo   %REPO_ROOT%build\windows-msvc-safe\MorePhi_artefacts\Release\VST3\MorePhi.vst3
    echo   %REPO_ROOT%build\windows-msvc-release\MorePhi_artefacts\Release\VST3\MorePhi.vst3
    echo Build first: cmake --build G:/morphy/build --config Release --target MorePhi_VST3
    pause
    exit /b 1
)

if not exist "%USERPROFILE%\Documents\VST3" mkdir "%USERPROFILE%\Documents\VST3" >nul 2>&1

echo Deploying morphy VST3...
echo   Source:      %SRC%
echo   Destination: %DST%
echo   Scope:       %SCOPE%
if exist "%DST%" rmdir /S /Q "%DST%"
xcopy "%SRC%" "%DST%" /E /Y /I /Q
echo OK: Deployed to %DST%
echo Next: rescan plugins in your DAW.
pause
