[CmdletBinding()]
param(
    [string]$BuildPreset = "windows-safe",
    [int[]]$ParallelJobs = @(1, 2, 4),
    [int]$SampleSeconds = 2,
    [string]$LogDir = "build/diagnostics",
    [switch]$SkipBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-SystemSnapshot {
    Add-Type -AssemblyName Microsoft.VisualBasic
    $computerInfo = New-Object Microsoft.VisualBasic.Devices.ComputerInfo
    $osReg = Get-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion"
    $baselineCounters = Get-Counter @(
        "\Paging File(_Total)\% Usage",
        "\Processor Information(_Total)\Processor Frequency"
    )
    $pagefileUsagePct = [double]0
    $processorFrequencyMHz = [double]0
    foreach ($sample in $baselineCounters.CounterSamples) {
        $counterPath = $sample.Path.ToLowerInvariant()
        if ($counterPath -like "*paging file(_total)*") {
            $pagefileUsagePct = [double]$sample.CookedValue
        } elseif ($counterPath -like "*processor information(_total)*processor frequency") {
            $processorFrequencyMHz = [double]$sample.CookedValue
        }
    }
    $cDrive = Get-PSDrive -PSProvider FileSystem | Where-Object { $_.Name -eq "C" }

    [pscustomobject]@{
        Timestamp       = (Get-Date).ToString("s")
        OS              = $osReg.ProductName
        OSVersion       = if ($osReg.DisplayVersion) { $osReg.DisplayVersion } else { $osReg.CurrentVersion }
        BuildNumber     = $osReg.CurrentBuild
        CPU             = $env:PROCESSOR_IDENTIFIER
        LogicalProcessors = [Environment]::ProcessorCount
        MaxClockMHz     = [int][math]::Round($processorFrequencyMHz, 0)
        TotalRamGB      = [math]::Round(($computerInfo.TotalPhysicalMemory / 1GB), 1)
        FreeRamGB       = [math]::Round(($computerInfo.AvailablePhysicalMemory / 1GB), 1)
        PagefileUsagePct = [math]::Round($pagefileUsagePct, 1)
        CDriveFreeGB    = if ($null -ne $cDrive) { [math]::Round(($cDrive.Free / 1GB), 1) } else { 0.0 }
    }
}

function Get-RecentCriticalEvents {
    try {
        $events = Get-WinEvent -FilterHashtable @{
            LogName   = "System"
            Id        = 41, 18
            StartTime = (Get-Date).AddDays(-7)
        } -MaxEvents 100 -ErrorAction Stop

        $events |
            Where-Object {
                $_.ProviderName -eq "Microsoft-Windows-Kernel-Power" -or
                $_.ProviderName -eq "Microsoft-Windows-WHEA-Logger"
            } |
            Select-Object -First 25 Id, TimeCreated, ProviderName, LevelDisplayName
    } catch {
        @()
    }
}

function Start-ResourceMonitor {
    param(
        [Parameter(Mandatory = $true)][string]$CsvPath,
        [Parameter(Mandatory = $true)][int]$IntervalSeconds,
        [Parameter(Mandatory = $true)][int]$MaxClockMHz
    )

    "timestamp,cpu_pct,available_ram_mb,pagefile_usage_pct,disk_queue,current_clock_mhz,clock_ratio_pct" | Set-Content -Path $CsvPath

    Start-Job -ScriptBlock {
        param($Path, $Interval, $CpuMaxClock)

        while ($true) {
            $timestamp = Get-Date -Format o
            $cpuPct = [double]0
            $availableMb = [double]0
            $pagefilePct = [double]0
            $diskQueue = [double]0
            $clockMHz = [double]0

            try {
                $counter = Get-Counter @(
                    "\Processor(_Total)\% Processor Time",
                    "\Memory\Available MBytes",
                    "\Paging File(_Total)\% Usage",
                    "\PhysicalDisk(_Total)\Avg. Disk Queue Length",
                    "\Processor Information(_Total)\Processor Frequency"
                )

                foreach ($sample in $counter.CounterSamples) {
                    $path = $sample.Path.ToLowerInvariant()
                    if ($path -like "*processor(_total)*") { $cpuPct = [double]$sample.CookedValue }
                    elseif ($path -like "*memory\\available mbytes") { $availableMb = [double]$sample.CookedValue }
                    elseif ($path -like "*paging file(_total)*") { $pagefilePct = [double]$sample.CookedValue }
                    elseif ($path -like "*physicaldisk(_total)*") { $diskQueue = [double]$sample.CookedValue }
                    elseif ($path -like "*processor information(_total)*processor frequency") { $clockMHz = [double]$sample.CookedValue }
                }
            } catch {
                # Keep sampling even if one counter read fails.
            }

            $clockRatioPct = if ($CpuMaxClock -gt 0) {
                [math]::Round(($clockMHz / $CpuMaxClock) * 100, 1)
            } else {
                0
            }

            Add-Content -Path $Path -Value ("{0},{1:N2},{2:N2},{3:N2},{4:N2},{5:N0},{6:N1}" -f $timestamp, $cpuPct, $availableMb, $pagefilePct, $diskQueue, $clockMHz, $clockRatioPct)
            Start-Sleep -Seconds $Interval
        }
    } -ArgumentList $CsvPath, $IntervalSeconds, $MaxClockMHz
}

function Stop-ResourceMonitor {
    param([Parameter(Mandatory = $true)]$Job)
    if ($null -ne $Job) {
        Stop-Job -Job $Job -ErrorAction SilentlyContinue | Out-Null
        Receive-Job -Job $Job -ErrorAction SilentlyContinue | Out-Null
        Remove-Job -Job $Job -ErrorAction SilentlyContinue | Out-Null
    }
}

function Measure-RunMetrics {
    param([Parameter(Mandatory = $true)][string]$CsvPath)

    $rows = Import-Csv -Path $CsvPath -ErrorAction SilentlyContinue
    if ($null -eq $rows -or $rows.Count -eq 0) {
        return [pscustomobject]@{
            AvgCpuPct           = 0.0
            MinAvailableRamMB   = 0.0
            MaxPagefileUsagePct = 0.0
            MaxDiskQueue        = 0.0
            MinClockRatioPct    = 0.0
            SampleCount         = 0
        }
    }

    $cpu = @($rows | ForEach-Object { [double]$_.cpu_pct })
    $ram = @($rows | ForEach-Object { [double]$_.available_ram_mb })
    $page = @($rows | ForEach-Object { [double]$_.pagefile_usage_pct })
    $disk = @($rows | ForEach-Object { [double]$_.disk_queue })
    $clock = @($rows | ForEach-Object { [double]$_.clock_ratio_pct })

    [pscustomobject]@{
        AvgCpuPct           = [math]::Round((($cpu | Measure-Object -Average).Average), 1)
        MinAvailableRamMB   = [math]::Round((($ram | Measure-Object -Minimum).Minimum), 1)
        MaxPagefileUsagePct = [math]::Round((($page | Measure-Object -Maximum).Maximum), 1)
        MaxDiskQueue        = [math]::Round((($disk | Measure-Object -Maximum).Maximum), 2)
        MinClockRatioPct    = [math]::Round((($clock | Measure-Object -Minimum).Minimum), 1)
        SampleCount         = $rows.Count
    }
}

function Classify-Issues {
    param(
        [Parameter(Mandatory = $true)]$Run,
        [Parameter(Mandatory = $true)]$Snapshot,
        [Parameter(Mandatory = $true)]$RecentCriticalEvents
    )

    $causes = @()
    $logText = Get-Content -Path $Run.BuildLogPath -Raw -ErrorAction SilentlyContinue

    if ($Run.MinAvailableRamMB -lt 1024 -or ($Run.MinAvailableRamMB -lt 2048 -and $Run.MaxPagefileUsagePct -gt 65)) {
        $causes += "resource-exhaustion"
    }

    if ($Snapshot.CDriveFreeGB -lt 25 -or $Run.MaxDiskQueue -gt 2) {
        $causes += "swap-or-disk-pressure"
    }

    if ($Run.AvgCpuPct -ge 90 -and $Run.MinClockRatioPct -gt 0 -and $Run.MinClockRatioPct -lt 70) {
        $causes += "possible-thermal-throttling"
    }

    if ($Run.ParallelJobs -ge 4 -and ($Run.ExitCode -ne 0 -or $Run.MinAvailableRamMB -lt 1024)) {
        $causes += "excessive-parallel-jobs"
    }

    if ($logText -match "(?i)LTCG|/GL|fatal error C1060|fatal error C1076|LNK1257|out of heap|ran out of memory") {
        $causes += "cmake-or-toolchain-configuration-pressure"
    }

    if ($RecentCriticalEvents.Count -gt 0) {
        $causes += "system-level-instability-signals"
    }

    if ($causes.Count -eq 0) {
        $causes += "no-obvious-bottleneck"
    }

    $causes | Sort-Object -Unique
}

function Write-RecommendationBlock {
    param([Parameter(Mandatory = $true)][string[]]$CauseUnion)

    Write-Host ""
    Write-Host "Recommended actions:" -ForegroundColor Cyan

    if ($CauseUnion -contains "resource-exhaustion" -or $CauseUnion -contains "excessive-parallel-jobs") {
        Write-Host " - Keep --parallel 2 as default on this machine; use --parallel 1 when stability is more important than speed."
        Write-Host " - Keep MORPHSNAP_MSVC_MP at 2 (or reduce to 1 if --parallel 4 is required)."
    }
    if ($CauseUnion -contains "swap-or-disk-pressure") {
        Write-Host " - Free C: drive to at least 25 GB before builds."
        Write-Host " - Keep pagefile enabled and sized to 24-32 GB for this 16 GB RAM system."
        Write-Host " - Consider building from D: with: cmake -S . -B D:/morphy-build -DMORPHSNAP_SAFE_BUILD_MODE=ON"
    }
    if ($CauseUnion -contains "possible-thermal-throttling") {
        Write-Host " - Improve cooling (fan profile, cooling pad, clean vents, hard surface)."
        Write-Host " - Re-run with --parallel 1 or 2 and compare clock ratio stability."
    }
    if ($CauseUnion -contains "cmake-or-toolchain-configuration-pressure") {
        Write-Host " - Keep MORPHSNAP_ENABLE_LTO=OFF for local development."
        Write-Host " - Use safe preset for local work and reserve aggressive flags for CI/release."
    }
    if ($CauseUnion -contains "system-level-instability-signals") {
        Write-Host " - Inspect Event Viewer entries for Kernel-Power (41) and WHEA (18)."
        Write-Host " - If freezes occur outside builds, run: sfc /scannow and Windows Memory Diagnostic."
    }
    if ($CauseUnion -contains "no-obvious-bottleneck") {
        Write-Host " - Metrics look stable. Next step is to collect a longer run and correlate with Event Viewer."
    }
}

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$resolvedLogDir = if ([System.IO.Path]::IsPathRooted($LogDir)) {
    $LogDir
} else {
    Join-Path $repoRoot $LogDir
}
New-Item -ItemType Directory -Force -Path $resolvedLogDir | Out-Null

$snapshot = Get-SystemSnapshot
$events = @(Get-RecentCriticalEvents)

Write-Host "=== MorphSnap Build Freeze Diagnostics ===" -ForegroundColor Cyan
Write-Host ("Host: {0}" -f $snapshot.OS)
Write-Host ("OS  : version {0}, build {1}" -f $snapshot.OSVersion, $snapshot.BuildNumber)
Write-Host ("CPU : {0} ({1} logical processors, max {2} MHz)" -f $snapshot.CPU, $snapshot.LogicalProcessors, $snapshot.MaxClockMHz)
Write-Host ("RAM : total {0} GB, free {1} GB" -f $snapshot.TotalRamGB, $snapshot.FreeRamGB)
Write-Host ("Page: current usage {0}%" -f $snapshot.PagefileUsagePct)
Write-Host ("C:  : free {0} GB" -f $snapshot.CDriveFreeGB)

if ($snapshot.CDriveFreeGB -lt 25) {
    Write-Warning "C: free space is below 25 GB. Disk/page pressure is likely during heavy builds."
}
if ($snapshot.PagefileUsagePct -gt 70) {
    Write-Warning "Current pagefile usage is high. Increase pagefile size and reduce parallelism."
}

if ($events.Count -gt 0) {
    Write-Warning ("Detected {0} recent critical system events (Kernel-Power/WHEA) in the last 7 days." -f $events.Count)
}

$runResults = @()

if (-not $SkipBuild) {
    foreach ($jobs in $ParallelJobs) {
        Write-Host ""
        Write-Host ("--- Build run: preset={0}, parallel={1} ---" -f $BuildPreset, $jobs) -ForegroundColor Yellow

        $runDir = Join-Path $resolvedLogDir ("parallel-{0}" -f $jobs)
        New-Item -ItemType Directory -Force -Path $runDir | Out-Null

        $metricsPath = Join-Path $runDir "resource-metrics.csv"
        $buildLogPath = Join-Path $runDir "build.log"
        $monitor = Start-ResourceMonitor -CsvPath $metricsPath -IntervalSeconds $SampleSeconds -MaxClockMHz $snapshot.MaxClockMHz

        $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
        & cmake --build --preset $BuildPreset --parallel $jobs *> $buildLogPath
        $exitCode = $LASTEXITCODE
        $stopwatch.Stop()

        Stop-ResourceMonitor -Job $monitor
        $metrics = Measure-RunMetrics -CsvPath $metricsPath

        $run = [pscustomobject]@{
            BuildPreset         = $BuildPreset
            ParallelJobs        = $jobs
            ExitCode            = $exitCode
            DurationSec         = [math]::Round($stopwatch.Elapsed.TotalSeconds, 1)
            AvgCpuPct           = $metrics.AvgCpuPct
            MinAvailableRamMB   = $metrics.MinAvailableRamMB
            MaxPagefileUsagePct = $metrics.MaxPagefileUsagePct
            MaxDiskQueue        = $metrics.MaxDiskQueue
            MinClockRatioPct    = $metrics.MinClockRatioPct
            SampleCount         = $metrics.SampleCount
            BuildLogPath        = $buildLogPath
        }

        $causes = Classify-Issues -Run $run -Snapshot $snapshot -RecentCriticalEvents $events
        $run | Add-Member -NotePropertyName Causes -NotePropertyValue ($causes -join ";")
        $runResults += $run

        Write-Host ("Exit={0}, Duration={1}s, MinRAM={2}MB, MaxPage={3}%, MaxDiskQ={4}, MinClock={5}%" -f $run.ExitCode, $run.DurationSec, $run.MinAvailableRamMB, $run.MaxPagefileUsagePct, $run.MaxDiskQueue, $run.MinClockRatioPct)
        Write-Host ("Likely causes: {0}" -f $run.Causes)
    }
} else {
    Write-Host "SkipBuild was set: only preflight diagnostics were collected."
}

$summaryPath = Join-Path $resolvedLogDir "diagnostics-summary.json"
$payload = [pscustomobject]@{
    Snapshot     = $snapshot
    RecentEvents = $events
    Runs         = $runResults
}
$payload | ConvertTo-Json -Depth 6 | Set-Content -Path $summaryPath

if ($runResults.Count -gt 0) {
    Write-Host ""
    Write-Host "=== Run summary ===" -ForegroundColor Cyan
    $runResults | Select-Object ParallelJobs, ExitCode, DurationSec, MinAvailableRamMB, MaxPagefileUsagePct, MaxDiskQueue, MinClockRatioPct, Causes | Format-Table -AutoSize
    $allCauses = $runResults.Causes -split ";" | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Sort-Object -Unique
    Write-RecommendationBlock -CauseUnion $allCauses
}

Write-Host ""
Write-Host ("Logs written to: {0}" -f $resolvedLogDir)
Write-Host ("Summary file   : {0}" -f $summaryPath)
