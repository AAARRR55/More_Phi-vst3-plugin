param(
    [switch]$SingleJob,
    [switch]$Deploy
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$configurePreset = "windows-msvc-safe"
$buildPreset = if ($SingleJob) { "windows-single" } else { "windows-safe" }
$artifactPath = Join-Path $repoRoot "build/windows-msvc-safe/MorphSnap_artefacts/Release/VST3/MorphSnap.vst3"

Push-Location $repoRoot
try {
    Write-Host "Configuring preset $configurePreset" -ForegroundColor Cyan
    & cmake --preset $configurePreset
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed with exit code $LASTEXITCODE"
    }

    Write-Host "Building preset $buildPreset" -ForegroundColor Cyan
    & cmake --build --preset $buildPreset
    if ($LASTEXITCODE -ne 0) {
        throw "CMake build failed with exit code $LASTEXITCODE"
    }

    if (-not (Test-Path $artifactPath)) {
        throw "Expected VST3 bundle was not produced at $artifactPath"
    }

    Write-Host "Build complete: $artifactPath" -ForegroundColor Green

    if ($Deploy) {
        & powershell -ExecutionPolicy Bypass -File (Join-Path $repoRoot "deploy.ps1")
        if ($LASTEXITCODE -ne 0) {
            throw "Deploy failed with exit code $LASTEXITCODE"
        }
    }
}
finally {
    Pop-Location
}