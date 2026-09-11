[CmdletBinding()]
param(
    [string]$BuildDir = 'build-win',
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$releaseDir = Join-Path $BuildDir $Configuration
if (-not (Test-Path -LiteralPath $releaseDir -PathType Container)) {
    throw "Build output directory does not exist: $releaseDir"
}

$gateway = Join-Path $releaseDir 'hsf_gateway.exe'
if (-not (Test-Path -LiteralPath $gateway -PathType Leaf)) {
    throw "Gateway executable does not exist: $gateway"
}

Write-Host '[tests] gateway version:' -ForegroundColor Cyan
& $gateway --version
if ($LASTEXITCODE -ne 0) { throw 'gateway --version failed' }

$tests = @(Get-ChildItem -LiteralPath $releaseDir -Filter '*_tests.exe' -File | Sort-Object Name)
if ($tests.Count -eq 0) { throw "No Release test executables found in $releaseDir" }

$passed = 0
foreach ($test in $tests) {
    Write-Host "[tests] $($test.Name)" -ForegroundColor Cyan
    & $test.FullName
    if ($LASTEXITCODE -ne 0) { throw "Test failed: $($test.Name)" }
    $passed++
}

Write-Host "[tests] passed $passed Windows $Configuration test executables" -ForegroundColor Green
