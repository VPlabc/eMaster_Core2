[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Archive,
    [Parameter(Mandatory = $true)]
    [string]$Version
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$script = Join-Path $PSScriptRoot 'verify-package.ps1'

$expectedHash = (Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script -Archive $Archive -ExpectedVersion $Version -ExpectedSha256 $expectedHash
if ($LASTEXITCODE -ne 0) { throw 'Valid package was rejected by the verifier' }

$tampered = Join-Path ([System.IO.Path]::GetTempPath()) ("emaster-tampered-" + [guid]::NewGuid().ToString('N') + '.zip')
try {
    [System.IO.File]::Copy((Resolve-Path -LiteralPath $Archive).Path, $tampered)
    $bytes = [System.IO.File]::ReadAllBytes($tampered)
    $bytes[$bytes.Length - 1] = $bytes[$bytes.Length - 1] -bxor 1
    [System.IO.File]::WriteAllBytes($tampered, $bytes)
    $tamperRejected = $false
    try {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script -Archive $tampered -ExpectedVersion $Version -ExpectedSha256 $expectedHash 2>$null
        $tamperRejected = ($LASTEXITCODE -ne 0)
    } catch {
        $tamperRejected = $true
    }
    if (-not $tamperRejected) { throw 'Verifier accepted a tampered archive' }
} finally {
    if (Test-Path -LiteralPath $tampered) { Remove-Item -LiteralPath $tampered -Force }
}

$wrongVersion = if ($Version -eq '0.0.0') { '9.9.9' } else { '0.0.0' }
$rejected = $false
try {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script -Archive $Archive -ExpectedVersion $wrongVersion 2>$null
    $rejected = ($LASTEXITCODE -ne 0)
} catch {
    $rejected = $true
}
if (-not $rejected) { throw 'Verifier accepted a package with the wrong expected version' }

Write-Host "[verify-test] valid and wrong-version package paths behaved correctly"
