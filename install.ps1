<#
.SYNOPSIS
    One-line installer for ESP32-C3 Unbrickable BLE Recovery.

    Run this inside ANY existing PlatformIO ESP32-C3 Arduino project:

        irm https://raw.githubusercontent.com/<OWNER>/wirelessboot/main/install.ps1 | iex

    What it does:
    1. Verifies platformio.ini exists in the current directory
    2. Downloads recovery assets into .wirelessboot/ subfolder
    3. Patches platformio.ini to use the custom partition table
    4. Flashes bootloader (0x0), partitions (0x8000), recovery (0x190000)

.PARAMETER Port
    Serial port for flashing (e.g., COM3). Auto-detected if omitted.

.PARAMETER NoFlash
    Download assets and patch platformio.ini but do NOT flash.

.EXAMPLE
    .\install.ps1
    .\install.ps1 -Port COM3
    .\install.ps1 -NoFlash
#>

[CmdletBinding()]
param(
    [string]$Port,
    [switch]$NoFlash
)

$ErrorActionPreference = "Stop"
$ProjectDir = Get-Location
$WirelessBootDir = Join-Path $ProjectDir ".wirelessboot"

# GitHub raw URLs (update <OWNER> and <REPO> as needed)
$RAW_BASE = "https://raw.githubusercontent.com/casmm/wirelessboot/main"
$RELEASE_BASE = "$RAW_BASE/release"

Write-Host "=== ESP32-C3 Unbrickable BLE Recovery Installer ===" -ForegroundColor Cyan
Write-Host "Project directory: $ProjectDir" -ForegroundColor Gray

# 1. Verify platformio.ini exists
$platformioIni = Join-Path $ProjectDir "platformio.ini"
if (-not (Test-Path $platformioIni)) {
    Write-Error "platformio.ini not found in current directory. Run this inside a PlatformIO project folder."
    exit 1
}
Write-Host "[1/5] platformio.ini found" -ForegroundColor Green

# 2. Create .wirelessboot/ and download assets
Write-Host "[2/5] Downloading recovery assets to .wirelessboot/..." -ForegroundColor Cyan
New-Item -ItemType Directory -Force -Path $WirelessBootDir | Out-Null

$files = @(
    @{ url = "$RELEASE_BASE/partitions.csv"; out = Join-Path $WirelessBootDir "partitions.csv" },
    @{ url = "$RELEASE_BASE/bootloader.bin";   out = Join-Path $WirelessBootDir "bootloader.bin" },
    @{ url = "$RELEASE_BASE/recovery.bin";      out = Join-Path $WirelessBootDir "recovery.bin" },
    @{ url = "$RAW_BASE/release/ota_client.py"; out = Join-Path $WirelessBootDir "ota_client.py" },
)

foreach ($file in $files) {
    Write-Host "  Downloading $(Split-Path $file.out -Leaf)..." -ForegroundColor Gray
    try {
        Invoke-WebRequest -Uri $file.url -OutFile $file.out -TimeoutSec 60
    } catch {
        Write-Error "Failed to download $($file.url): $_"
        exit 1
    }
}
Write-Host "  All assets downloaded" -ForegroundColor Green

# 3. Backup and patch platformio.ini
Write-Host "[3/5] Patching platformio.ini..." -ForegroundColor Cyan
$bakPath = "$platformioIni.bak"
if (-not (Test-Path $bakPath)) {
    Copy-Item $platformioIni $bakPath
    Write-Host "  Backed up to platformio.ini.bak" -ForegroundColor Gray
}

$iniContent = Get-Content $platformioIni -Raw

# Check if already patched
if ($iniContent -notmatch "wirelessboot/partitions.csv") {
    # Add partition table line at the end of the [env] section or at the end of file
    $patchLine = "`n; --- wirelessboot recovery partition ---`nboard_build.partitions = .wirelessboot/partitions.csv"
    $iniContent = $iniContent.TrimEnd() + $patchLine
    Set-Content -Path $platformioIni -Value $iniContent
    Write-Host "  Added board_build.partitions = .wirelessboot/partitions.csv" -ForegroundColor Green
} else {
    Write-Host "  Already patched, skipping" -ForegroundColor Gray
}

# 4. Locate esptool.py
Write-Host "[4/5] Locating esptool.py..." -ForegroundColor Cyan
$esptoolCandidates = @(
    Join-Path $env:USERPROFILE ".platformio\packages\tool-esptool\esptool.py",
    Join-Path $env:LOCALAPPDATA ".platformio\packages\tool-esptool\esptool.py",
    "C:\Users\$env:USERNAME\.platformio\packages\tool-esptool\esptool.py",
)

$esptool = $null
foreach ($candidate in $esptoolCandidates) {
    if (Test-Path $candidate) {
        $esptool = $candidate
        break
    }
}

if (-not $esptool) {
    Write-Error "esptool.py not found. Install PlatformIO and run 'pio platform install espressif32' first."
    exit 1
}
Write-Host "  Found: $esptool" -ForegroundColor Green

# Find python
$pioPython = Join-Path (Split-Path $esptool -Parent) "..\..\python_env\Scripts\python.exe"
if (-not (Test-Path $pioPython)) {
    $pioPython = "python"
}

# 5. Auto-detect port if not specified
if (-not $Port) {
    Write-Host "[5/5] Auto-detecting ESP32-C3 serial port..." -ForegroundColor Cyan
    # Try to detect from platformio.ini or use common ports
    $commonPorts = "COM1","COM2","COM3","COM4","COM5","COM6","COM7","COM8","COM9","COM10"
    foreach ($p in $commonPorts) {
        try {
            $port = New-Object System.IO.Ports.SerialPort $p, 115200
            $port.Open()
            $port.Close()
            $Port = $p
            Write-Host "  Detected: $p" -ForegroundColor Green
            break
        } catch {
            # Port in use or doesn't exist, try next
        }
    }
    if (-not $Port) {
        Write-Error "No serial port detected. Specify with -Port COMx"
        exit 1
    }
} else {
    Write-Host "[5/5] Using port: $Port" -ForegroundColor Cyan
}

# 6. Flash
if (-not $NoFlash) {
    Write-Host "`n=== Flashing recovery partition ===" -ForegroundColor Cyan
    Write-Host "  bootloader.bin -> 0x0" -ForegroundColor Gray
    Write-Host "  partitions.csv -> 0x8000" -ForegroundColor Gray
    Write-Host "  recovery.bin   -> 0x190000" -ForegroundColor Gray

    & $pioPython $esptool --chip esp32c3 --port $Port --baud 921600 write_flash -z 0x0 "$(Join-Path $WirelessBootDir "bootloader.bin")" 0x8000 "$(Join-Path $WirelessBootDir "partitions.csv")" 0x190000 "$(Join-Path $WirelessBootDir "recovery.bin")"

    if ($LASTEXITCODE -eq 0) {
        Write-Host "`n=== Flash complete! ===" -ForegroundColor Green
        Write-Host "Recovery firmware installed at factory partition (0x190000)" -ForegroundColor Green
        Write-Host "Hold BOOT (GPIO9) on power-up to enter recovery mode." -ForegroundColor Yellow
    } else {
        Write-Error "Flashing failed with exit code $LASTEXITCODE"
        exit 1
    }
} else {
    Write-Host "`n=== Install complete (no flash) ===" -ForegroundColor Green
    Write-Host "Assets in: $WirelessBootDir" -ForegroundColor Green
    Write-Host "Run: .\install.ps1 -Port <COMx> to flash" -ForegroundColor Yellow
}

Write-Host "`nNext steps:" -ForegroundColor Cyan
Write-Host "  1. Build your Arduino app normally (pio run -t upload)"
Write-Host "  2. App goes to ota_0 (0x10000), recovery stays at factory (0x190000)"
Write-Host "  3. Hold BOOT button on power-up to enter BLE recovery"
Write-Host "  4. Use .wirelessboot/ota_client.py to upload firmware over BLE"