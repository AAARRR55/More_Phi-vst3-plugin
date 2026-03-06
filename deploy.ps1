# MorphSnap — Deploy VST3 to system folder (requires admin)
# Usage: Right-click → Run as Administrator, or run from elevated PowerShell

$repoRoot = Split-Path -Parent $PSCommandPath
$src = Join-Path $repoRoot "build\windows-msvc-safe\MorphSnap_artefacts\Release\VST3\MorphSnap.vst3"
$dst = "C:\Program Files\Common Files\VST3\MorphSnap.vst3"

# Check admin
if (-NOT ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "Elevating to admin..." -ForegroundColor Yellow
    Start-Process powershell.exe -ArgumentList "-ExecutionPolicy Bypass -File `"$PSCommandPath`"" -Verb RunAs
    exit
}

if (-not (Test-Path $src)) {
    Write-Host "ERROR: Build not found at $src" -ForegroundColor Red
    Write-Host "Run: .\scripts\build-windows-safe.ps1" -ForegroundColor Yellow
    pause; exit 1
}

Write-Host "Deploying MorphSnap VST3..." -ForegroundColor Cyan
Copy-Item -Path $src -Destination $dst -Recurse -Force
$size = [Math]::Round((Get-Item $dst).Length / 1MB, 2)
Write-Host "OK: Deployed $size MB -> $dst" -ForegroundColor Green
pause
