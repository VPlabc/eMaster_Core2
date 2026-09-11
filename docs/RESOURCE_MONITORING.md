# Resource monitoring

Windows soak tests can sample a running gateway with:

```powershell
./scripts/monitor-gateway.ps1 -ProcessId <pid> -DurationSeconds 3600 `
  -IntervalSeconds 10 -Output gateway-resource-monitor.csv
```

The CSV records UTC timestamps, working-set/private memory, CPU seconds,
thread count, handle count, and TCP socket count. Compare the first and last
samples, and inspect the series for monotonic growth during reconnect,
application reload, and configuration reload scenarios.

Linux runs should use equivalent `/proc/<pid>/status`, `/proc/<pid>/fd`, and
socket-count sampling until a common cross-platform monitor is added.
