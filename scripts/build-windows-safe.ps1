param(
    [switch]$SingleJob,
    [switch]$Deploy
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$cmakeHelp = (& cmake --help 2>&1) -join "`n"
$vs2026DevCmd = "C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\Tools\VsDevCmd.bat"
$hasVs2026 = ($cmakeHelp -match "Visual Studio 18 2026") -and (Test-Path $vs2026DevCmd)

if ($hasVs2026) {
    $configurePreset = "windows-ninja-vs2026-safe"
    $buildPreset = if ($SingleJob) { "windows-vs2026-single" } else { "windows-vs2026-safe" }
    $artifactPath = Join-Path $repoRoot "build/windows-ninja-vs2026-safe/MorePhi_artefacts/Release/VST3/MorePhi.vst3"
}
else {
    $configurePreset = "windows-msvc-safe"
    $buildPreset = if ($SingleJob) { "windows-single" } else { "windows-safe" }
    $artifactPath = Join-Path $repoRoot "build/windows-msvc-safe/MorePhi_artefacts/Release/VST3/MorePhi.vst3"
}

function Invoke-CMakeBuildCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Command
    )

    if ($hasVs2026) {
        & cmd.exe /c "call `"$vs2026DevCmd`" -arch=x64 -host_arch=x64 >nul && $Command"
    }
    else {
        Invoke-Expression $Command
    }
}

Push-Location $repoRoot
try {
    Write-Host "Configuring preset $configurePreset" -ForegroundColor Cyan
    Invoke-CMakeBuildCommand "cmake --preset $configurePreset"
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed with exit code $LASTEXITCODE"
    }

    Write-Host "Building preset $buildPreset" -ForegroundColor Cyan
    Invoke-CMakeBuildCommand "cmake --build --preset $buildPreset --target MorePhi_VST3"
    if ($LASTEXITCODE -ne 0) {
        throw "CMake build failed with exit code $LASTEXITCODE"
    }

    if (-not (Test-Path $artifactPath)) {
        throw "Expected VST3 bundle was not produced at $artifactPath"
    }

    Write-Host "Build complete: $artifactPath" -ForegroundColor Green

    if ($Deploy) {
        & powershell -ExecutionPolicy Bypass -File (Join-Path $repoRoot "deploy.ps1") -NoPause
        if ($LASTEXITCODE -ne 0) {
            throw "Deploy failed with exit code $LASTEXITCODE"
        }
    }
}
finally {
    Pop-Location
}
