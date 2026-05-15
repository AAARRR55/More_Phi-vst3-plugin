# morphy — Deploy VST3 to standard DAW plugin folders
# Usage:
#   .\deploy.ps1                  # Current user install (no admin)
#   .\deploy.ps1 -Scope Machine   # All users install (elevates)

param(
    [ValidateSet("User", "Machine")]
    [string]$Scope = "User",

    [switch]$NoPause
)

$repoRoot = Split-Path -Parent $PSCommandPath

$artifactRoots = @(
    (Join-Path $repoRoot "build\morphy_artefacts\Release\VST3"),
    (Join-Path $repoRoot "build\MorePhi_artefacts\Release\VST3"),
    (Join-Path $repoRoot "build\windows-ninja-vs2026-safe\morphy_artefacts\Release\VST3"),
    (Join-Path $repoRoot "build\windows-ninja-vs2026-safe\MorePhi_artefacts\Release\VST3"),
    (Join-Path $repoRoot "build\windows-msvc-2026-safe\morphy_artefacts\Release\VST3"),
    (Join-Path $repoRoot "build\windows-msvc-2026-safe\MorePhi_artefacts\Release\VST3"),
    (Join-Path $repoRoot "build\windows-msvc-2026-release\morphy_artefacts\Release\VST3"),
    (Join-Path $repoRoot "build\windows-msvc-2026-release\MorePhi_artefacts\Release\VST3"),
    (Join-Path $repoRoot "build\windows-msvc-safe\morphy_artefacts\Release\VST3"),
    (Join-Path $repoRoot "build\windows-msvc-safe\MorePhi_artefacts\Release\VST3"),
    (Join-Path $repoRoot "build\windows-msvc-release\morphy_artefacts\Release\VST3"),
    (Join-Path $repoRoot "build\windows-msvc-release\MorePhi_artefacts\Release\VST3")
)

$bundleCandidates = @("MorePhi.vst3")
$sourceCandidates = foreach ($root in $artifactRoots) {
    foreach ($bundle in $bundleCandidates) {
        Join-Path $root $bundle
    }
}

$src = $sourceCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $src) {
    Write-Host "ERROR: Could not find plugin .vst3 bundle in expected build paths." -ForegroundColor Red
    Write-Host "Tried:" -ForegroundColor Yellow
    $sourceCandidates | ForEach-Object { Write-Host "  - $_" }
    Write-Host "Build first: cmake --build G:/morphy/build --config Release --target MorePhi_VST3" -ForegroundColor Yellow
    if (-not $NoPause) {
        pause
    }
    exit 1
}

$targetBundleName = Split-Path -Leaf $src

if ($Scope -eq "Machine") {
    $dst = Join-Path $env:ProgramFiles ("Common Files\VST3\" + $targetBundleName)

    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent())
        .IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

    if (-not $isAdmin) {
        Write-Host "Elevating to admin for machine-wide install..." -ForegroundColor Yellow
        Start-Process powershell.exe -ArgumentList "-ExecutionPolicy Bypass -File `"$PSCommandPath`" -Scope Machine" -Verb RunAs
        exit
    }
}
else {
    $dst = Join-Path $env:USERPROFILE ("Documents\VST3\" + $targetBundleName)
}

$dstParent = Split-Path -Parent $dst
if (-not (Test-Path $dstParent)) {
    New-Item -ItemType Directory -Path $dstParent -Force | Out-Null
}

Write-Host "Deploying morphy VST3..." -ForegroundColor Cyan
Write-Host "  Source:      $src"
Write-Host "  Destination: $dst"
Write-Host "  Scope:       $Scope"

if (Test-Path $dst) {
    $resolvedDst = (Resolve-Path -LiteralPath $dst).Path
    $resolvedParent = (Resolve-Path -LiteralPath $dstParent).Path
    if ($resolvedDst.StartsWith($resolvedParent + [IO.Path]::DirectorySeparatorChar) -and
        (Split-Path -Leaf $resolvedDst) -like "*.vst3") {
        Remove-Item -LiteralPath $resolvedDst -Recurse -Force
    }
    else {
        throw "Refusing to replace unexpected destination path: $resolvedDst"
    }
}

Copy-Item -LiteralPath $src -Destination $dstParent -Recurse -Force

$sizeBytes = (Get-ChildItem -LiteralPath $dst -Recurse -File | Measure-Object -Property Length -Sum).Sum
$size = [Math]::Round($sizeBytes / 1MB, 2)
Write-Host "OK: Deployed $size MB -> $dst" -ForegroundColor Green
Write-Host "Next: rescan plugins in your DAW." -ForegroundColor Green
if (-not $NoPause) {
    pause
}
