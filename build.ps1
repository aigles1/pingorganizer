#requires -Version 7.0
<#
.SYNOPSIS
    Configures and builds PSPingGui.

.DESCRIPTION
    First run downloads wxWidgets, the WebView2 SDK and xterm.js, then builds
    wxWidgets from source; expect roughly five to ten minutes. Later runs only
    recompile the app itself and take a few seconds.

.EXAMPLE
    .\build.ps1 -Run
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',

    # Throw away the build directory and start over.
    [switch]$Clean,

    # Launch the app once it has built.
    [switch]$Run
)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$buildDir = Join-Path $PSScriptRoot 'build'

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host 'Removing build directory...' -ForegroundColor Yellow
    Remove-Item $buildDir -Recurse -Force
}

Write-Host "Configuring ($Config)..." -ForegroundColor Cyan
cmake -S . -B $buildDir -G 'Visual Studio 18 2026' -A x64
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)." }

Write-Host "Building ($Config)..." -ForegroundColor Cyan
cmake --build $buildDir --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)." }

$exe = Join-Path $buildDir "$Config\PSPingGui.exe"
if (-not (Test-Path $exe)) { throw "Built, but $exe is missing." }

Write-Host "`nBuilt: $exe" -ForegroundColor Green

if ($Run) {
    Write-Host 'Launching...' -ForegroundColor Cyan
    Start-Process $exe
}
