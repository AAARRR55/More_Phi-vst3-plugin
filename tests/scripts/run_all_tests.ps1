# Comprehensive Test Runner for Morphy Plugin (Windows PowerShell)
# Runs all test suites and generates combined report

# Configuration
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
$BuildDir = Join-Path $ProjectRoot "build"
$PluginPath = Join-Path $BuildDir "Morphy_artefacts\Release\VST3\Morphy.vst3"
$ResultsDir = Join-Path $ProjectRoot "test-results"
$Timestamp = Get-Date -Format "yyyyMMdd_HHmmss"

# Create results directory
New-Item -ItemType Directory -Force -Path $ResultsDir | Out-Null

Write-Host "=========================================="  -ForegroundColor Cyan
Write-Host "Morphy Plugin Test Suite"  -ForegroundColor Cyan
Write-Host "=========================================="  -ForegroundColor Cyan
Write-Host "Build Directory: $BuildDir"
Write-Host "Plugin Path: $PluginPath"
Write-Host "Results Directory: $ResultsDir"
Write-Host "=========================================="  -ForegroundColor Cyan
Write-Host ""

# Overall status
$OverallStatus = 0

# Function to print test header
function Print-TestHeader {
    param([string]$Title)
    Write-Host ""
    Write-Host "=========================================="  -ForegroundColor Cyan
    Write-Host $Title  -ForegroundColor Cyan
    Write-Host "=========================================="  -ForegroundColor Cyan
}

# Function to check if plugin exists
function Test-PluginExists {
    if (-not (Test-Path $PluginPath)) {
        Write-Host "ERROR: Plugin not found at $PluginPath" -ForegroundColor Red
        Write-Host "Please build the plugin first"
        exit 1
    }
}

# Test 1: VST3 Validation
function Test-VST3Validation {
    Print-TestHeader "Test 1: VST3 SDK Validation"

    $ScriptPath = Join-Path $ScriptDir "run_vst3_validator.py"
    if (-not (Test-Path $ScriptPath)) {
        Write-Host "WARNING: VST3 validator script not found" -ForegroundColor Yellow
        return
    }

    $OutputPath = Join-Path $ResultsDir "vst3_validation_$Timestamp.json"
    $JunitPath = Join-Path $ResultsDir "vst3_validation_$Timestamp.xml"

    & python $ScriptPath `
        --plugin $PluginPath `
        --output $OutputPath `
        --junit $JunitPath

    if ($LASTEXITCODE -ne 0) {
        $script:OverallStatus = 1
    }

    Write-Host "VST3 validation complete"
}

# Test 2: Real-Time Safety
function Test-RealtimeSafety {
    Print-TestHeader "Test 2: Real-Time Safety Testing"

    $ScriptPath = Join-Path $ScriptDir "realtime_safety_test.py"
    if (-not (Test-Path $ScriptPath)) {
        Write-Host "WARNING: Real-time safety test script not found" -ForegroundColor Yellow
        return
    }

    $OutputPath = Join-Path $ResultsDir "realtime_safety_$Timestamp.json"

    & python $ScriptPath `
        --plugin $PluginPath `
        --duration 5 `
        --output $OutputPath

    if ($LASTEXITCODE -ne 0) {
        $script:OverallStatus = 1
    }

    Write-Host "Real-time safety testing complete"
}

# Test 3: Audio Quality
function Test-AudioQuality {
    Print-TestHeader "Test 3: Audio Quality Testing"

    $ScriptPath = Join-Path $ScriptDir "audio_quality_test.py"
    if (-not (Test-Path $ScriptPath)) {
        Write-Host "WARNING: Audio quality test script not found" -ForegroundColor Yellow
        return
    }

    $OutputDir = Join-Path $ResultsDir "audio_quality_$Timestamp"

    & python $ScriptPath `
        --plugin $PluginPath `
        --output-dir $OutputDir

    if ($LASTEXITCODE -ne 0) {
        $script:OverallStatus = 1
    }

    Write-Host "Audio quality testing complete"
}

# Test 4: Unit Tests
function Test-UnitTests {
    Print-TestHeader "Test 4: Unit Tests"

    $TestExe = Join-Path $BuildDir "Release\MorphyTests.exe"
    if (-not (Test-Path $TestExe)) {
        Write-Host "WARNING: Unit test executable not found at $TestExe" -ForegroundColor Yellow
        return
    }

    $OutputPath = Join-Path $ResultsDir "unit_tests_$Timestamp.json"

    & $TestExe `
        --use-colour yes `
        --reporter json `
        --out $OutputPath

    if ($LASTEXITCODE -ne 0) {
        $script:OverallStatus = 1
    }

    Write-Host "Unit tests complete"
}

# Test 5: Memory Analysis (Windows-specific)
function Test-MemoryAnalysis {
    Print-TestHeader "Test 5: Memory Analysis"

    # Use Application Verifier if available
    $AppVerifier = "C:\Program Files (x86)\Windows Kits\10\bin\x64\appverif.exe"
    if (Test-Path $AppVerifier) {
        Write-Host "Running with Application Verifier..."

        & $AppVerifier -for $TestExe -with Handles
        & $TestExe
        & $AppVerifier -disable $TestExe

        Write-Host "Memory analysis complete"
    } else {
        Write-Host "Application Verifier not available, skipping memory analysis" -ForegroundColor Yellow
    }
}

# Generate combined report
function Generate-CombinedReport {
    Print-TestHeader "Generating Combined Report"

    $ReportFile = Join-Path $ResultsDir "combined_report_$Timestamp.json"

    $Report = @{
        timestamp = (Get-Date -Format "o")
        plugin_path = $PluginPath
        test_results = @{}
        overall_status = if ($OverallStatus -eq 0) { "PASSED" } else { "FAILED" }
    }

    # Add individual test results if they exist
    $TestTypes = @("vst3_validation", "realtime_safety", "audio_quality", "unit_tests")
    foreach ($TestType in $TestTypes) {
        $JsonFile = Get-ChildItem $ResultsDir -Filter "${TestType}_*.json" | Select-Object -First 1
        if ($JsonFile) {
            $Report.test_results[$TestType] = Get-Content $JsonFile.FullName | ConvertFrom-Json
        }
    }

    $Report | ConvertTo-Json -Depth 10 | Out-File $ReportFile

    Write-Host "Combined report saved to: $ReportFile"
}

# Print summary
function Print-Summary {
    Print-TestHeader "Test Suite Summary"

    Write-Host ""
    Write-Host "Results Directory: $ResultsDir"
    Write-Host ""
    Write-Host "Individual Test Results:"
    Write-Host "  - VST3 Validation: $ResultsDir\vst3_validation_$Timestamp.*"
    Write-Host "  - Real-Time Safety: $ResultsDir\realtime_safety_$Timestamp.json"
    Write-Host "  - Audio Quality: $ResultsDir\audio_quality_$Timestamp\"
    Write-Host "  - Unit Tests: $ResultsDir\unit_tests_$Timestamp.json"
    Write-Host "  - Combined Report: $ResultsDir\combined_report_$Timestamp.json"
    Write-Host ""

    if ($OverallStatus -eq 0) {
        Write-Host "Overall Status: PASSED" -ForegroundColor Green
    } else {
        Write-Host "Overall Status: FAILED" -ForegroundColor Red
    }
    Write-Host ""
}

# Main execution
function Main {
    # Check prerequisites
    Test-PluginExists

    # Run all tests
    Test-VST3Validation
    Test-RealtimeSafety
    Test-AudioQuality
    Test-UnitTests
    Test-MemoryAnalysis

    # Generate report
    Generate-CombinedReport

    # Print summary
    Print-Summary

    # Exit with appropriate code
    exit $OverallStatus
}

# Parse command line arguments
for ($i = 0; $i -lt $args.Count; $i++) {
    switch ($args[$i]) {
        "--plugin-path" {
            $PluginPath = $args[++$i]
        }
        "--build-dir" {
            $BuildDir = $args[++$i]
        }
        "--results-dir" {
            $ResultsDir = $args[++$i]
        }
        "--help" {
            Write-Host "Usage: .\run_all_tests.ps1 [options]"
            Write-Host "Options:"
            Write-Host "  --plugin-path PATH   Path to plugin bundle"
            Write-Host "  --build-dir PATH     Build directory"
            Write-Host "  --results-dir PATH   Results directory"
            exit 0
        }
        default {
            Write-Host "Unknown option: $($args[$i])"
            exit 1
        }
    }
}

# Run main
Main
