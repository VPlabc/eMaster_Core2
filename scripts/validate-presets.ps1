[CmdletBinding()]
param([string]$PresetFile = 'CMakePresets.json')

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$presets = Get-Content -Raw -LiteralPath $PresetFile | ConvertFrom-Json
$expected = @('windows-x86', 'windows-x64', 'windows11-iot', 'ubuntu-18', 'ubuntu-22', 'linux-arm64')
$names = @($presets.configurePresets | ForEach-Object name)
foreach ($name in $expected) { if ($name -notin $names) { throw "Missing configure preset: $name" } }
$x86 = $presets.configurePresets | Where-Object name -eq 'windows-x86'
$x64 = $presets.configurePresets | Where-Object name -eq 'windows-x64'
if ($x86.cacheVariables.HSF_ENABLE_ZK -ne 'ON' -or $x64.cacheVariables.HSF_ENABLE_ZK -ne 'OFF') { throw 'Windows ZK architecture constraint is not represented by the presets' }
$arm = $presets.configurePresets | Where-Object name -eq 'linux-arm64'
if (-not $arm.toolchainFile -or $arm.cacheVariables.HSF_ENABLE_ZK -ne 'OFF') { throw 'ARM64 preset must select a toolchain and disable ZK' }
$root = Split-Path -Parent $PresetFile
if ([string]::IsNullOrWhiteSpace($root)) { $root = (Get-Location).Path }
foreach ($name in $expected) {
    $target = Join-Path $root ("config\targets\$name.json")
    if (-not (Test-Path -LiteralPath $target)) { throw "Missing target metadata: $target" }
    $metadata = Get-Content -Raw -LiteralPath $target | ConvertFrom-Json
    foreach ($field in 'platform', 'architecture', 'zk', 'compiler', 'dependencies', 'install_layout', 'package_format') {
        if ($null -eq $metadata.$field) { throw "Target metadata '$name' is missing '$field'" }
    }
}
$iot = $presets.configurePresets | Where-Object name -eq 'windows11-iot'
if ($iot.cacheVariables.HSF_ENABLE_ZK -ne 'OFF' -or $iot.cacheVariables.HSF_BUILD_PLUGIN_SDK -ne 'OFF') {
    throw 'Windows 11 IoT must disable ZK and the native plugin SDK by default'
}
Write-Host "[profiles] validated $($expected.Count) configure presets and target metadata"
