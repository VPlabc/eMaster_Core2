[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Archive,
    [Parameter(Mandatory = $true)]
    [string]$Version
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$fixture = Join-Path ([System.IO.Path]::GetTempPath()) ("emaster-update-fixture-" + [guid]::NewGuid().ToString('N'))
$release = Join-Path $fixture ("releases\" + $Version)
$data = Join-Path $fixture 'data'

try {
    New-Item -ItemType Directory -Force -Path $release, $data | Out-Null
    $archivePath = (Resolve-Path -LiteralPath $Archive).Path
    $unpacked = Join-Path $fixture 'unpacked'
    Expand-Archive -LiteralPath $archivePath -DestinationPath $unpacked
    $source = Get-ChildItem -LiteralPath $unpacked -Directory | Select-Object -First 1
    if (-not $source) { throw 'Archive has no release directory' }
    Copy-Item -Path (Join-Path $source.FullName '*') -Destination $release -Recurse -Force

    $preserved = @{
        'config.db' = 'configuration-marker'
        'clients.db' = 'credential-marker'
        'applications\demo\main.lua' = 'lua-marker'
    }
    foreach ($relative in $preserved.Keys) {
        $path = Join-Path $data $relative
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
        Set-Content -LiteralPath $path -Value $preserved[$relative] -NoNewline
    }

    foreach ($relative in $preserved.Keys) {
        $path = Join-Path $data $relative
        if ((Get-Content -Raw -LiteralPath $path) -ne $preserved[$relative]) {
            throw "Preserved data changed: $relative"
        }
    }
    if (Get-ChildItem -LiteralPath $release -Recurse -File | Where-Object { $_.Name -like '*.db' -or $_.Name -eq 'config.json' }) {
        throw 'Release directory contains persistent data that belongs in data/'
    }
    Write-Host "[update-fixture] release $Version installed and persistent data preserved"
}
finally {
    if (Test-Path -LiteralPath $fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force }
}
