<#
.SYNOPSIS
    Runs cargo with the LOCAL Rust toolchain (zero PATH pollution).

    All Rust tooling lives in .rust/ inside this project directory.
    RUSTUP_HOME and CARGO_HOME are scoped strictly to this shell invocation.

.USAGE
    .\cargo_local.ps1 build --release --target riscv32imc-unknown-none-elf
    .\cargo_local.ps1 check --target riscv32imc-unknown-none-elf
    .\cargo_local.ps1 clippy --target riscv32imc-unknown-none-elf

.NOTES
    On Windows, cargo may need dlltool.exe from llvm-tools.
    If you get a "dlltool" error, add the llvm-tools bin directory to PATH:

      $env:PATH = "C:\path\to\project\.rust\rustup\toolchains\<toolchain>\lib\rustlib\x86_64-pc-windows-gnu\bin\self-contained;" + $env:PATH
#>

param(
    [Parameter(Mandatory = $true, Position = 0, RemainingArguments = $true)]
    [string[]]$Args
)

$ErrorActionPreference = "Stop"

$ProjectRoot = $PSScriptRoot
$RustupHome  = Join-Path $ProjectRoot ".rust\rustup"
$CargoHome   = Join-Path $ProjectRoot ".rust\cargo"
$CargoBin    = Join-Path $CargoHome "bin\cargo.exe"

if (-not (Test-Path $CargoBin)) {
    Write-Error "Local cargo not found at $CargoBin"
    Write-Host "Run install-rust.ps1 first to bootstrap the local toolchain." -ForegroundColor Yellow
    exit 1
}

# Scope Rust environment strictly to this process
$env:RUSTUP_HOME = $RustupHome
$env:CARGO_HOME  = $CargoHome

# On Windows, help cargo find dlltool.exe if needed
$dlltoolDir = Join-Path $RustupHome "toolchains\stable-x86_64-pc-windows-gnu\lib\rustlib\x86_64-pc-windows-gnu\bin\self-contained"
if (Test-Path $dlltoolDir) {
    $env:PATH = "$dlltoolDir;" + $env:PATH
}

& $CargoBin @Args