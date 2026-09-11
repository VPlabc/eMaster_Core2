[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$StageDir)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not (Test-Path -LiteralPath $StageDir -PathType Container)) { throw "Release stage does not exist: $StageDir" }
foreach ($required in 'bin', 'web', 'config') {
    if (-not (Test-Path -LiteralPath (Join-Path $StageDir $required) -PathType Container)) { throw "Release stage is missing required directory: $required" }
}
if (-not (Test-Path -LiteralPath (Join-Path $StageDir 'config\config.example.json') -PathType Leaf)) { throw 'Release stage is missing config/config.example.json' }

$stageRoot = (Resolve-Path -LiteralPath $StageDir).Path.TrimEnd('\')
$forbidden = Get-ChildItem -LiteralPath $StageDir -Recurse -Force -File | Where-Object {
    $relative = $_.FullName.Substring($stageRoot.Length).TrimStart('\')
    $topLevel = $relative.Split('\')[0]
    $_.Name -match '^(config|clients|logs|smartlocker)\.db(-.*)?$' -or
    $_.Name -eq 'config.json' -or $topLevel -in @('.git', 'target', 'build', 'tests', 'source')
}
if ($forbidden) { throw ("Release contains forbidden files:`n" + (($forbidden | ForEach-Object FullName) -join "`n")) }
Write-Host "[package] release validation passed: $StageDir" -ForegroundColor Green
