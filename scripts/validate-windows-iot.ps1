[CmdletBinding()]
param(
    [string]$PresetFile = 'CMakePresets.json',
    [string]$TargetFile = 'config\targets\windows11-iot.json'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$presets = Get-Content -Raw -LiteralPath $PresetFile | ConvertFrom-Json
$preset = @($presets.configurePresets | Where-Object name -eq 'windows11-iot')
if ($preset.Count -ne 1) { throw 'Windows 11 IoT configure preset is missing or duplicated' }
$target = Get-Content -Raw -LiteralPath $TargetFile | ConvertFrom-Json

if ($preset[0].architecture -ne 'x64') { throw 'Windows 11 IoT must target x64' }
if ($preset[0].cacheVariables.HSF_ENABLE_ZK -ne 'OFF') { throw 'Windows 11 IoT cannot enable ZK' }
if ($preset[0].cacheVariables.HSF_BUILD_PLUGIN_SDK -ne 'OFF') { throw 'Windows 11 IoT must disable the native plugin SDK' }
if ($target.platform -ne 'windows' -or $target.architecture -ne 'x64') { throw 'IoT metadata has an invalid platform or architecture' }
if ($target.plugin_sdk -ne $false -or $target.zk -ne $false) { throw 'IoT metadata enables an unsupported feature' }
if ($target.runtime_verification -ne 'pending-target-environment') {
    throw 'IoT runtime verification must remain explicitly pending until an IoT target is available'
}
Write-Host '[windows-iot] profile contract validated (runtime verification pending target hardware)'
