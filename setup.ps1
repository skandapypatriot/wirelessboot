<#
.SYNOPSIS
    One-command setup and build for ESP32-C3 BLE OTA Recovery using PlatformIO

.DESCRIPTION
    Installs PlatformIO, ESP32 platform, builds the project, and optionally flashes.

.USAGE
    .\setup.ps1                    # Install deps and build
    .\setup.ps1 -Flash             # Build and flash to device
    .\setup.ps1 -Monitor           # Build, flash, and open serial monitor
    .\setup.ps1 -Clean             # Clean build
    .\setup.ps1 -Size              # Show size report
#>

param(
    [switch]$Flash,
    [switch]$Monitor,
    [switch]$Clean,
    [switch]$Size,
    [switch]$NoCache
)

$ErrorActionPreference = "Stop"
$ProjectDir = $PSScriptRoot

function Write-Step($msg) {
    Write-Host "`n=== $msg ===" -ForegroundColor Cyan
}

function Write-OK($msg) {
    Write-Host "[OK] $msg" -ForegroundColor Green
}

function Write-Err($msg) {
    Write-Host "[ERR] $msg" -ForegroundColor Red
}

function Check-Command($cmd) {
    try {
        $null = & $cmd --version 2>$null
        return $true
    } catch {
        return $false
    }
}

# Check Python
Write-Step "Checking Python"
if (-not (Check-Command python)) {
    Write-Err "Python not found in PATH. Install from python.org or 'winget install Python.Python.3.12'"
    exit 1
}
Write-OK "Python found: $(python --version)"

# Check/Install PlatformIO
Write-Step "Checking PlatformIO"
if (-not (Check-Command pio)) {
    Write-Host "Installing PlatformIO via pip..." -ForegroundColor Yellow
    python -m pip install --upgrade platformio
    if (-not (Check-Command pio)) {
        Write-Err "PlatformIO install failed. Try: python -m pip install platformio"
        exit 1
    }
}
Write-OK "PlatformIO found: $(pio --version)"

# Install ESP32 platform
Write-Step "Installing ESP32 platform (espressif32)"
pio platform install espressif32 @~6.4.0
Write-OK "ESP32 platform installed"

# Clean if requested
if ($Clean) {
    Write-Step "Cleaning build"
    pio run --target clean
    Write-OK "Clean done"
}

# Build
Write-Step "Building project"
$buildArgs = "run"
if ($NoCache) { $buildArgs += " --target clean" }
pio @($buildArgs.Split(' ', [StringSplitOptions]::RemoveEmptyEntries))

$binPath = "$ProjectDir\.pio\build\esp32c3\firmware.bin"
if (-not (Test-Path $binPath)) {
    Write-Err "Build failed - firmware.bin not found at $binPath"
    exit 1
}
$size = (Get-Item $binPath).Length
Write-OK "Build successful: $([math]::Round($size/1024, 1)) KB ($size bytes)"

# Size report
if ($Size) {
    Write-Step "Size report"
    pio run --target size
}

# Flash
if ($Flash -or $Monitor) {
    Write-Step "Flashing to device"
    pio run --target upload
    Write-OK "Flash complete"
}

# Monitor
if ($Monitor) {
    Write-Step "Opening serial monitor (Ctrl+C to exit)"
    pio device monitor
}

Write-Host "`nDone!" -ForegroundColor Green