<#
.SYNOPSIS
    Builds and packages the eMaster Gateway for Windows (request/release.md
    sections 9, 15, 16, 30).

.DESCRIPTION
    Always configures into a CLEAN build directory. A release must be
    reproducible from the tagged commit, and an incremental build can carry
    stale objects or a stale configure-time Git hash baked in by CMake.

    Produces dist/HSF-Gateway-v<version>-windows-<arch>.zip plus a SHA-256
    checksum file.

.PARAMETER Arch
    x86 (default) includes ZKTeco PullSDK support; the SDK is a 32-bit
    Windows DLL, so x64 packages are built with -DHSF_ENABLE_ZK=OFF and
    cannot do ZK card reading.

.PARAMETER SkipBuild
    Package whatever is already in the build directory. For iterating on the
    packaging step itself -- never for producing a real release.
#>
[CmdletBinding()]
param(
    [ValidateSet('x86', 'x64')]
    [string]$Arch = 'x86',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $RepoRoot

$Version = (Get-Content (Join-Path $RepoRoot 'VERSION') -First 1).Trim()
if ($Version -notmatch '^\d+\.\d+\.\d+') {
    throw "VERSION must contain a semantic version like 1.0.0, got '$Version'"
}

$Platform  = if ($Arch -eq 'x86') { 'Win32' } else { 'x64' }
$Triplet   = if ($Arch -eq 'x86') { 'x86-windows' } else { 'x64-windows' }
$EnableZk  = if ($Arch -eq 'x86') { 'ON' } else { 'OFF' }
$BuildDir  = Join-Path $RepoRoot "build-release-$Arch"
$StageName = "HSF-Gateway-v$Version-windows-$Arch"
$DistDir   = Join-Path $RepoRoot 'dist'
$StageDir  = Join-Path $DistDir $StageName

Write-Host "[package] eMaster Gateway $Version -> windows-$Arch (ZK=$EnableZk)" -ForegroundColor Cyan

# --- locate tools ---------------------------------------------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found. Install Visual Studio with the C++ workload." }
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "No Visual Studio installation with the C++ workload was found." }

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
$cmakeExe = if ($cmake) { $cmake.Source } else {
    Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
if (-not (Test-Path $cmakeExe)) { throw "cmake.exe not found (looked for it on PATH and under $vsPath)." }

$vcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { Join-Path $RepoRoot 'build\vcpkg' }
$toolchain = Join-Path $vcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
if (-not (Test-Path $toolchain)) {
    throw "vcpkg toolchain not found at $toolchain. Set VCPKG_ROOT or bootstrap vcpkg into build\vcpkg."
}

# --- build ----------------------------------------------------------------
if (-not $SkipBuild) {
    if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
    & $cmakeExe -S $RepoRoot -B $BuildDir -A $Platform `
        "-DVCPKG_TARGET_TRIPLET=$Triplet" `
        "-DCMAKE_TOOLCHAIN_FILE=$toolchain" `
        "-DHSF_ENABLE_ZK=$EnableZk"
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

    & $cmakeExe --build $BuildDir --config Release
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }
} else {
    Write-Warning "SkipBuild: packaging an existing build directory. Not valid for a real release."
}

$exe = Join-Path $BuildDir 'Release\hsf_gateway.exe'
if (-not (Test-Path $exe)) { throw "Built executable not found at $exe" }

# --- stage ----------------------------------------------------------------
# Layout mirrors release.md section 15, flattened so the binary sits beside
# web/ and config/ -- which is exactly what ResolveResourceDir() in main.cpp
# looks for first, so the package runs from wherever it is extracted.
if (Test-Path $StageDir) { Remove-Item -Recurse -Force $StageDir }
New-Item -ItemType Directory -Force -Path $StageDir | Out-Null

New-Item -ItemType Directory -Force -Path (Join-Path $StageDir 'bin') | Out-Null
Copy-Item $exe (Join-Path $StageDir 'bin')
# Runtime DLLs vcpkg placed next to the exe, plus the PullSDK ones CMake
# copied there post-build when ZK is enabled.
Get-ChildItem (Join-Path $BuildDir 'Release') -Filter *.dll -ErrorAction SilentlyContinue |
    ForEach-Object { Copy-Item $_.FullName (Join-Path $StageDir 'bin') }

Copy-Item -Recurse (Join-Path $RepoRoot 'web') (Join-Path $StageDir 'web')

# config/: scripts and the TEMPLATE only. config.db, config.json and
# clients.db carry the REST API key and generated client keys
# (release.md section 2) and must never be packaged.
New-Item -ItemType Directory -Force -Path (Join-Path $StageDir 'config') | Out-Null
Copy-Item (Join-Path $RepoRoot 'config\config.example.json') (Join-Path $StageDir 'config')
Copy-Item -Recurse (Join-Path $RepoRoot 'config\scripts') (Join-Path $StageDir 'config\scripts')

Copy-Item -Recurse (Join-Path $RepoRoot 'docs') (Join-Path $StageDir 'docs') -ErrorAction SilentlyContinue
foreach ($f in 'README.md', 'CHANGELOG.md', 'LICENSE', 'VERSION') {
    Copy-Item (Join-Path $RepoRoot $f) $StageDir -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force -Path (Join-Path $StageDir 'scripts') | Out-Null
Copy-Item (Join-Path $PSScriptRoot 'run.ps1') (Join-Path $StageDir 'scripts') -ErrorAction SilentlyContinue

# Belt and braces: fail loudly rather than ship a credential if any of the
# excluded files ever reaches the staging directory.
$leaked = Get-ChildItem $StageDir -Recurse -Include '*.db', 'config.json' -ErrorAction SilentlyContinue
if ($leaked) {
    $leaked | ForEach-Object { Write-Error "Refusing to package credential file: $($_.FullName)" }
    throw "Staging directory contains files excluded by release.md section 2."
}

# --- build metadata -------------------------------------------------------
$buildInfo = & (Join-Path $StageDir 'bin\hsf_gateway.exe') --version
$buildInfo | Set-Content (Join-Path $StageDir 'BUILD_INFO.txt') -Encoding UTF8
Write-Host $buildInfo

# --- archive + checksum ---------------------------------------------------
$zip = Join-Path $DistDir "$StageName.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $StageDir -DestinationPath $zip -CompressionLevel Optimal

$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
"$hash  $StageName.zip" | Out-File (Join-Path $DistDir 'SHA256SUMS') -Append -Encoding ascii

Write-Host "[package] $zip" -ForegroundColor Green
Write-Host "[package] sha256 $hash" -ForegroundColor Green
Write-Host "[package] Extract it somewhere clean and run scripts\run.ps1 before publishing (release.md section 17)."
