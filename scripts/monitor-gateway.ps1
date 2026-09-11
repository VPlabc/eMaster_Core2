[CmdletBinding(DefaultParameterSetName = 'ById')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'ById')]
    [int]$ProcessId,
    [Parameter(Mandatory = $true, ParameterSetName = 'ByName')]
    [string]$ProcessName,
    [int]$DurationSeconds = 60,
    [int]$IntervalSeconds = 5,
    [string]$Output = 'gateway-resource-monitor.csv'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($DurationSeconds -le 0 -or $IntervalSeconds -le 0) { throw 'DurationSeconds and IntervalSeconds must be positive' }

$rows = [System.Collections.Generic.List[object]]::new()
$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
while ($stopwatch.Elapsed.TotalSeconds -lt $DurationSeconds) {
    try {
        $process = if ($PSCmdlet.ParameterSetName -eq 'ById') {
            Get-Process -Id $ProcessId -ErrorAction Stop
        } else {
            Get-Process -Name $ProcessName -ErrorAction Stop | Select-Object -First 1
        }
        $socketCount = 0
        if (Get-Command Get-NetTCPConnection -ErrorAction SilentlyContinue) {
            $socketCount = @(Get-NetTCPConnection -OwningProcess $process.Id -ErrorAction SilentlyContinue).Count
        }
        $rows.Add([pscustomobject]@{
            timestamp_utc = [DateTime]::UtcNow.ToString('o')
            pid = $process.Id
            working_set_bytes = $process.WorkingSet64
            private_memory_bytes = $process.PrivateMemorySize64
            cpu_seconds = $process.CPU
            threads = @($process.Threads).Count
            handles = $process.HandleCount
            tcp_sockets = $socketCount
        })
    } catch [System.Management.Automation.PSArgumentException] {
        throw "Gateway process is no longer available"
    }
    Start-Sleep -Seconds $IntervalSeconds
}

$rows | Export-Csv -LiteralPath $Output -NoTypeInformation -Encoding UTF8
Write-Host "[monitor] wrote $($rows.Count) samples to $Output"
