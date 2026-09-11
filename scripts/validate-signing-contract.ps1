[CmdletBinding()]
param([string]$Root = '.')

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Read-Text([string]$relative) {
    Get-Content -Raw -LiteralPath (Join-Path $Root $relative)
}

$header = Read-Text 'include\hsf\update\SignatureVerifier.h'
$source = Read-Text 'src\update\SignatureVerifier.cpp'
$docs = Read-Text 'docs\RELEASE_SIGNING.md'

foreach ($pattern in 'BuildSignedMessage', 'Verify') {
    if ($header -notmatch $pattern -or $source -notmatch $pattern) {
        throw "Signature verifier is missing required API: $pattern"
    }
}
if ($source -notmatch 'version \+ "\\n" \+ platform \+ "\\n" \+ sha256Hex \+ "\\n"') {
    throw 'Signature verifier canonical message format has changed unexpectedly'
}
if ($docs -notmatch 'private keys must') { throw 'Signing documentation must prohibit repository private keys' }
if ($docs -notmatch 'version>.*platform>.*archive-sha256') {
    throw 'Signing documentation must describe the canonical signed message'
}

$forbidden = @(Get-ChildItem -LiteralPath $Root -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object {
        $_.FullName -notmatch '\\(\.git|build|target|dist)\\' -and
        $_.FullName -notmatch '\\config\\lua_(package|signing)\.key$' -and
        $_.Name -match '(release|private|ota|signing).*\.(pem|key|p12|pfx)$'
    })
if ($forbidden.Count -gt 0) {
    throw "Private-key-like files found in repository inputs: $($forbidden.FullName -join ', ')"
}
Write-Warning 'config/lua_package.key and config/lua_signing.key are legacy Lua runtime keys; they must remain outside release archives.'
Write-Host '[signing] canonical message, verifier API, and key hygiene validated'
