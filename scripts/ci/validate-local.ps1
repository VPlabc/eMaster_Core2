[CmdletBinding()]
param([string]$BuildDir = 'build-win', [string]$Actionlint)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $repo

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products '*' -property installationPath
$ctest = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
& $ctest --test-dir $BuildDir -C Release --output-on-failure --no-tests=error --timeout 120
if ($LASTEXITCODE -ne 0) { throw 'CTest failed' }

python -m compileall -q scripts/ci
if ($LASTEXITCODE -ne 0) { throw 'Python compilation failed' }
python -m unittest discover -s scripts/ci -p 'test_*.py' -v
if ($LASTEXITCODE -ne 0) { throw 'CI tooling tests failed' }

$tokens = $null
$errors = $null
[System.Management.Automation.Language.Parser]::ParseFile((Join-Path $repo 'scripts/package.ps1'), [ref]$tokens, [ref]$errors) | Out-Null
if ($errors) { throw ($errors | Out-String) }
& ./scripts/validate-update-safety.ps1

$bash = Join-Path $env:ProgramFiles 'Git\bin\bash.exe'
if (-not (Test-Path $bash)) { throw 'Git Bash is required for shell syntax validation' }
& $bash -n scripts/package.sh scripts/build-ubuntu.sh scripts/make-manifest.sh scripts/ci/linux-package.sh
if ($LASTEXITCODE -ne 0) { throw 'Shell syntax validation failed' }
if ($Actionlint) {
    & $Actionlint -shellcheck= -pyflakes= .github/workflows/build.yml .github/workflows/firmware-build.yml .github/workflows/release.yml .github/workflows/hardware-test.yml
    if ($LASTEXITCODE -ne 0) { throw 'Workflow validation failed' }
} else {
    Write-Warning 'actionlint was not supplied; workflow validation was not run.'
}
