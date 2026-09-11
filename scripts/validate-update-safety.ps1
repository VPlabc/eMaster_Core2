[CmdletBinding()]
param([string]$Root = '.')

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Read-RepoFile([string]$relative) { Get-Content -Raw -LiteralPath (Join-Path $Root $relative) }
function Require-Text([string]$text, [string]$pattern, [string]$message) { if ($text -notmatch $pattern) { throw $message } }

$windows = Read-RepoFile 'scripts/package.ps1'
$posix = Read-RepoFile 'scripts/package.sh'
$installer = Read-RepoFile 'scripts/install-ota.sh'
$manifest = Read-RepoFile 'scripts/make-manifest.sh'

Require-Text $windows 'config\.db|clients\.db|credential' 'Windows package must document credential/database exclusion'
Require-Text $posix '\*\.db|config\.json|credential' 'POSIX package must exclude credential/database files'
Require-Text $windows 'validate-release\.ps1' 'Windows package must run release validation'
Require-Text $windows 'verify-package\.ps1' 'Windows package must verify the final archive'
Require-Text $posix 'validate-release\.sh' 'POSIX package must run release validation'
Require-Text $posix 'verify-package\.sh' 'POSIX package must verify the final archive'
Require-Text $installer 'current|rollback|releases' 'OTA installer must use versioned release/rollback layout'
Require-Text $manifest 'sha256|signature|platforms' 'Release manifest must bind checksums, signatures, and platforms'
Write-Host '[update] packaging and OTA safety contract validated'
