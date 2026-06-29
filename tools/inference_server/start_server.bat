@echo off
REM ==========================================================================
REM SonicMaster inference server launcher.
REM Double-click this file, or run from a terminal:
REM     tools\inference_server\start_server.bat
REM
REM Defaults the model package to the known-good location. Override by
REM passing a path as the first argument:
REM     start_server.bat "D:\path\to\sonicmaster-v3-decision-engine-XXXX"
REM
REM Binds 127.0.0.1:8765 (the address the More-Phi plugin probes). Leave this
REM window open while you use the plugin; closing it stops the server and the
REM plugin's "Neural Master" toggle flips back to "unavailable (no model)".
REM ==========================================================================

setlocal

REM Repo root = parent of this script's directory's parent.
set "SCRIPT_DIR=%~dp0"
set "SERVER=%SCRIPT_DIR%server.py"

REM Default package location (override with %1).
set "PACKAGE=%~1"
if "%PACKAGE%"=="" set "PACKAGE=C:\Users\HP\Downloads\sonicmaster-v3-decision-engine-20260530T121536Z"

REM Sanity-check the checkpoint exists so we fail fast with a clear message
REM instead of a Python traceback.
set "CKPT=%PACKAGE%\models\v3\mastering-brain-v2-fullchain-best\checkpoints\best.ckpt"
if not exist "%CKPT%" (
    echo [start_server] FATAL: checkpoint not found at:
    echo   %CKPT%
    echo [start_server] Pass the package root as the first argument, e.g.
    echo   start_server.bat "C:\path\to\sonicmaster-v3-decision-engine-XXXX"
    pause
    exit /b 1
)

echo [start_server] package : %PACKAGE%
echo [start_server] server  : %SERVER%
echo [start_server] starting SonicMaster inference server on 127.0.0.1:8765 ...
echo.

python "%SERVER%" --package "%PACKAGE%" --host 127.0.0.1 --port 8765
set "RC=%ERRORLEVEL%"

echo.
if not "%RC%"=="0" (
    echo [start_server] server exited with code %RC%.
    pause
)
endlocal & exit /b %RC%
