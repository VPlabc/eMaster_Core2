[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Archive,
    [string]$ExpectedVersion,
    [string]$ExpectedSha256
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$archivePath = (Resolve-Path -LiteralPath $Archive).Path
$actualSha256 = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($ExpectedSha256 -and $actualSha256 -ne $ExpectedSha256.ToLowerInvariant()) {
    throw "Archive checksum '$actualSha256' does not match expected '$ExpectedSha256'"
}
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("emaster-package-" + [guid]::NewGuid().ToString('N'))

try {
    Expand-Archive -LiteralPath $archivePath -DestinationPath $temp -Force
    $directories = @(Get-ChildItem -LiteralPath $temp -Directory)
    if ($directories.Count -ne 1) { throw 'Archive must contain exactly one release directory' }
    $stage = $directories[0].FullName

    $manifestPath = Join-Path $stage 'manifest.json'
    $versionPath = Join-Path $stage 'VERSION'
    $binary = Join-Path $stage 'bin\hsf_gateway.exe'
    foreach ($required in $manifestPath, $versionPath, $binary) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "Package is missing required file: $required"
        }
    }

    $manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    $version = (Get-Content -LiteralPath $versionPath -First 1).Trim()
    foreach ($field in 'product', 'platform', 'architecture', 'build_type', 'git_commit') {
        if ([string]::IsNullOrWhiteSpace([string]$manifest.$field)) {
            throw "Manifest is missing required field: $field"
        }
    }
    if ([string]$manifest.product -ne 'eMaster') { throw "Unsupported manifest product: $($manifest.product)" }
    if ([string]$manifest.build_type -ne 'Release') { throw "Package build type must be Release" }
    if ([string]$manifest.version -ne $version) {
        throw "Manifest version '$($manifest.version)' does not match VERSION '$version'"
    }
    if ($ExpectedVersion -and $version -ne $ExpectedVersion) {
        throw "Package version '$version' does not match expected '$ExpectedVersion'"
    }

    $leaked = @(Get-ChildItem -LiteralPath $stage -Recurse -File |
        Where-Object { $_.Name -like '*.db' -or $_.Name -eq 'config.json' -or $_.Name -like '*.key' })
    if ($leaked.Count -gt 0) {
        throw "Package contains excluded credential/database files: $($leaked.FullName -join ', ')"
    }

    & (Join-Path $root 'scripts\validate-release.ps1') -StageDir $stage
    Write-Host "[verify] package is valid: version=$version sha256=$actualSha256 archive=$archivePath"
}
finally {
    if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Recurse -Force }
}
