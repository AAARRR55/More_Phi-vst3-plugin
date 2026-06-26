$ErrorActionPreference = 'Stop'
$src = 'G:\More_Phi-vst3-plugin\build-ninja\MorePhi_artefacts\Release\VST3\MorePhi.vst3'
$dst = 'C:\Program Files\Common Files\VST3\MorePhi.vst3'
$log = 'G:\More_Phi-vst3-plugin\reinstall-result.log'

"=== Reinstall start: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') ===" | Out-File $log -Force
if (-not (Test-Path $src)) {
    "ERROR: source bundle not found at $src" | Out-File $log -Append
    exit 2
}
try {
    if (Test-Path $dst) {
        Remove-Item -Recurse -Force $dst
        "Removed stale: $dst" | Out-File $log -Append
    }
} catch {
    "REMOVE FAILED: $($_.Exception.Message)" | Out-File $log -Append
    exit 3
}
try {
    Copy-Item -Recurse -Force $src $dst
    "Copied fresh bundle -> $dst" | Out-File $log -Append
} catch {
    "COPY FAILED: $($_.Exception.Message)" | Out-File $log -Append
    exit 4
}
$dll = Join-Path $dst 'Contents\x86_64-win\MorePhi.vst3'
if (Test-Path $dll) {
    $mtime = (Get-Item $dll).LastWriteTime
    "Installed DLL mtime: $($mtime.ToString('yyyy-MM-dd HH:mm:ss'))" | Out-File $log -Append
    "SUCCESS" | Out-File $log -Append
    exit 0
} else {
    "ERROR: DLL missing after copy" | Out-File $log -Append
    exit 5
}
