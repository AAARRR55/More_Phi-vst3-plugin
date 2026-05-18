$ErrorActionPreference = 'Stop'

$fl = Get-Process FL64 -ErrorAction Stop | Select-Object -First 1
$script = Join-Path (Split-Path -Parent $PSScriptRoot) 'tools\trace_izotope_ipc_frida.js'

if (-not (Get-Command frida -ErrorAction SilentlyContinue)) {
    Write-Host 'Frida CLI is not installed or not on PATH.'
    Write-Host 'Install with: python -m pip install frida-tools'
    Write-Host "Then run: frida -p $($fl.Id) -l `"$script`""
    exit 1
}

Write-Host "Attaching Frida to FL64 PID $($fl.Id)"
frida -p $fl.Id -l $script
