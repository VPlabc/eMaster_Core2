<#
.SYNOPSIS
    Starts the eMaster Gateway from an extracted release package.

.DESCRIPTION
    Works from either layout: <pkg>\scripts\run.ps1 in a package, or
    scripts\run.ps1 in the source tree (where it falls back to build-win\).
#>
[CmdletBinding()]
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$GatewayArgs)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root = Split-Path -Parent $PSScriptRoot

$candidates = @(
    (Join-Path $Root 'bin\hsf_gateway.exe'),
    (Join-Path $Root 'build-release-x86\Release\hsf_gateway.exe'),
    (Join-Path $Root 'build-win\Release\hsf_gateway.exe')
)
$exe = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $exe) {
    throw "hsf_gateway.exe not found. Build it first (scripts\package.ps1) or run this from an extracted package."
}

$configDir = Join-Path $Root 'config'

# First run: seed config.json from the template so ConfigManager imports it
# into a fresh config.db. Never overwrite an existing one -- that would
# discard live settings, including the REST API key.
if ((Test-Path $configDir) -and
    -not (Test-Path (Join-Path $configDir 'config.db')) -and
    -not (Test-Path (Join-Path $configDir 'config.json'))) {
    $template = Join-Path $configDir 'config.example.json'
    if (Test-Path $template) {
        Copy-Item $template (Join-Path $configDir 'config.json')
        Write-Host "[run] Seeded config\config.json from the template."
        Write-Host "[run] Edit it (or the Configuration page) and replace the YOUR_* placeholders."
    }
}

Write-Host "[run] $((& $exe --version | Select-Object -First 2) -join ' ')"
& $exe @GatewayArgs
