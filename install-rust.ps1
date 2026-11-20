<#
.SYNOPSIS
    Bootstrap the LOCAL Rust toolchain inside .rust/ (no PATH pollution).

    Downloads rustup-init.exe, installs stable toolchain with minimal profile,
    adds riscv32imc-unknown-none-elf target, then removes the installer.

.USAGE
    .\install-rust.ps1
#>

param()

$ErrorActionPreference = "Stop"
$ProjectRoot = $PSScriptRoot
$RustupDir   = Join-Path $ProjectRoot ".rust\rustup"
$CargoDir    = Join-Path $ProjectRoot ".rust\cargo"
$Installer   = Join-Path $ProjectRoot ".rust\rustup-init.exe"

# 1. Create directories
New-Item -ItemType Directory -Force -Path $RustupDir | Out-Null
New-Item -ItemType Directory -Force -Path $CargoDir | Out-Null

# 2. Download rustup-init.exe (direct static URL, not rustup.rs HTML)
$url = "https://static.rust-lang.org/rustup/dist/x86_64-pc-windows-gnu/rustup-init.exe"
Write-Host "Downloading rustup-init.exe..." -ForegroundColor Cyan
Invoke-WebRequest -Uri $url -OutFile $Installer

# 3. Install silently (minimal profile, no PATH modification)
Write-Host "Installing Rust toolchain (this may take a minute)..." -ForegroundColor Cyan
$env:RUSTUP_HOME = $RustupDir
$env:CARGO_HOME  = $CargoDir
& $Installer -y --no-modify-path --profile minimal --default-host x86_64-pc-windows-gnu --default-toolchain stable

# 4. Add riscv32imc-unknown-none-elf target
Write-Host "Adding riscv32imc-unknown-none-elf target..." -ForegroundColor Cyan
& Join-Path $CargoDir "bin\rustup.exe" target add riscv32imc-unknown-none-elf

# 5. Add llvm-tools (provides dlltool.exe needed by windows-sys on Windows)
Write-Host "Adding llvm-tools component..." -ForegroundColor Cyan
& Join-Path $CargoDir "bin\rustup.exe" component add llvm-tools-preview

# 6. Remove installer
Remove-Item $Installer -Force

Write-Host "`nRust toolchain installed locally in .rust/" -ForegroundColor Green
Write-Host "Use .\cargo_local.ps1 <cargo-args> to build" -ForegroundColor Green