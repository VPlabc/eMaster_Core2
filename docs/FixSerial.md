# Serial Communication Performance Optimization Plan

## 1. Objective

Investigate and fix the performance issue where opening a Serial Port from the C++ backend on Windows takes approximately **10–15 seconds**, causing the entire HSF Gateway application to become blocked or delayed.

The current Lua workflow repeatedly performs:

```text
Serial.Open()
    ↓
Serial.Read()
    ↓
Serial.Close()
```

This causes the application to repeatedly pay the Windows Serial Port initialization/cleanup cost.

The goal is to redesign the Serial subsystem so that:

* Opening a serial port is fast and non-blocking.
* Serial ports remain open while the Lua runtime is active.
* Lua scripts do not repeatedly open/close the physical port.
* Serial communication is asynchronous where possible.
* Serial reads do not block the main application.
* Serial writes are responsive.
* Multiple Lua scripts cannot interfere with the same physical serial port.
* Serial errors can be recovered without blocking the whole gateway.
* The same API behavior remains available to Lua.
* The implementation works consistently on Windows, Linux, and macOS.

---

# 2. Current Problem

## 2.1 Current Architecture

The current flow is approximately:

```text
Lua Script
    │
    ▼
Serial.Open()
    │
    ▼
C++ Serial Implementation
    │
    ▼
Windows COM Port
    │
    ▼
Serial.Read()
    │
    ▼
Serial.Close()
```

The Lua script may execute this cycle repeatedly:

```lua
Serial.Open()

local data = Serial.Read()

Serial.Close()
```

This is inefficient for a continuously connected device.

---

# 3. Observed Problem

On Windows, the C++ implementation may take approximately:

```text
10–15 seconds
```

during:

```text
Serial.Open()
```

This causes:

```text
Lua Script
    │
    ▼
Serial.Open()
    │
    │ 10–15 seconds
    ▼
Application blocked
    │
    ├── Web UI delayed
    ├── Lua scripts delayed
    ├── Modbus delayed
    ├── REST API delayed
    └── WebSocket delayed
```

The serial implementation must therefore be investigated as a potential blocking operation.

---

# 4. Root Cause Investigation

Before changing the architecture, inspect the existing C++ Serial implementation.

Check the following areas.

## 4.1 Windows COM Port Opening

Inspect:

```cpp
CreateFile()
```

for the COM port.

Example:

```cpp
CreateFile(
    "\\\\.\\COM3",
    GENERIC_READ | GENERIC_WRITE,
    0,
    nullptr,
    OPEN_EXISTING,
    0,
    nullptr
);
```

Check whether the implementation is using inappropriate flags or causing synchronous blocking.

---

# 5. Serial Initialization

Inspect all operations executed inside:

```text
Serial.Open()
```

including:

```text
CreateFile
GetCommState
SetCommState
SetCommTimeouts
SetupComm
PurgeComm
EscapeCommFunction
ClearCommError
Device configuration
Thread creation
Event creation
Read thread initialization
Write thread initialization
```

Measure the execution time of every operation.

Example:

```text
Serial.Open()
    ├── CreateFile()       12 ms
    ├── GetCommState()      2 ms
    ├── SetCommState()      1 ms
    ├── SetCommTimeouts()   1 ms
    ├── SetupComm()         1 ms
    └── Worker startup      3 ms

Total: 20 ms
```

If one operation takes several seconds, isolate and fix that operation instead of optimizing Lua first.

---

# 6. Add Serial Performance Instrumentation

Add high-resolution timing to the C++ implementation.

Use:

```cpp
std::chrono::steady_clock
```

Example:

```cpp
auto start = std::chrono::steady_clock::now();

// Serial initialization

auto end = std::chrono::steady_clock::now();

auto elapsed =
    std::chrono::duration_cast<
        std::chrono::milliseconds
    >(end - start).count();
```

Log:

```text
Serial.Open BEGIN
CreateFile BEGIN
CreateFile END: 12 ms
ConfigurePort BEGIN
ConfigurePort END: 4 ms
WorkerThread BEGIN
WorkerThread END: 2 ms
Serial.Open END: 20 ms
```

This must be implemented before optimizing the code.

---

# 7. Main Optimization Principle

## Do Not Open and Close the Physical Port for Every Lua Operation

The physical serial port should have a lifecycle independent of individual Lua calls.

Recommended architecture:

```text
Application Start
       │
       ▼
Serial Manager
       │
       ▼
Open COM Port
       │
       ▼
Keep Port Open
       │
       ├───────────────┐
       │               │
       ▼               ▼
Read Worker        Write Queue
       │               │
       ▼               ▼
Receive Buffer      COM Port
       │
       ▼
Lua API
```

The Lua API becomes an interface to the Serial Manager instead of directly controlling the physical COM port.

---

# 8. Recommended Serial Lifecycle

The preferred lifecycle is:

```text
Application Start
        │
        ▼
Load Serial Configuration
        │
        ▼
Serial Manager Initialize
        │
        ▼
Open COM Port
        │
        ▼
Start RX Worker
        │
        ▼
Start TX Worker / Queue
        │
        ▼
Port Ready
        │
        ├── Lua Read
        ├── Lua Write
        ├── Lua Script 1
        └── Lua Script 2
```

The port should remain open until:

```text
Application Shutdown
```

or:

```text
Port Configuration Changed
```

or:

```text
Fatal Serial Error
```

---

# 9. Lua API Behavior

Keep the Lua API compatible:

```lua
Serial.Open()
Serial.Close()
Serial.Read()
Serial.Write(text)
```

However, change their semantics internally.

## Serial.Open()

Instead of always opening the physical COM port:

```text
Serial.Open()
    ↓
Check Serial Manager
    ↓
Already Open?
    ├── YES → return true
    └── NO  → start/open port
```

Repeated calls should be idempotent.

Example:

```lua
Serial.Open()
Serial.Open()
Serial.Open()
```

must not reopen the COM port three times.

---

# 10. Serial.Close()

`Serial.Close()` should not necessarily destroy the physical connection every time a Lua script calls it.

Recommended behavior:

```text
Lua Serial.Close()
       │
       ▼
Release Lua ownership/reference
       │
       ▼
Physical Port remains open
```

The physical port should be closed only when no longer required or when explicitly configured to do so.

An alternative is to introduce:

```lua
Serial.Release()
```

for Lua ownership management while reserving:

```lua
Serial.Close()
```

for actual shutdown.

The final behavior should be clearly documented.

---

# 11. Reference Counting

If multiple Lua scripts use the same serial port, use reference counting.

Example:

```text
Script A → Serial.Open()
Reference Count = 1

Script B → Serial.Open()
Reference Count = 2

Script A → Serial.Close()
Reference Count = 1

Script B → Serial.Close()
Reference Count = 0
```

The physical port can remain open for a configurable idle period or until the application shuts it down.

For the HSF Gateway, keeping the port open permanently is preferable for dedicated hardware.

---

# 12. Dedicated Serial Manager

Create a dedicated C++ component:

```text
SerialManager
```

Recommended structure:

```text
backend/
└── serial/
    ├── SerialManager.h
    ├── SerialManager.cpp
    ├── SerialPort.h
    ├── SerialPort.cpp
    ├── SerialBuffer.h
    └── SerialBuffer.cpp
```

Responsibilities:

```text
SerialManager
├── Configuration
├── Open / Close
├── Connection state
├── RX worker
├── TX queue
├── Receive buffer
├── Error recovery
├── Thread synchronization
└── Lua API integration
```

---

# 13. Asynchronous Receive

Serial reception should not block the Lua runtime.

Recommended:

```text
COM Port
   │
   ▼
RX Worker Thread
   │
   ▼
Receive Buffer
   │
   ▼
Lua Serial.Read()
```

The RX thread continuously reads available data.

Lua should retrieve already-received data from the buffer.

---

# 14. Receive Buffer

Implement a thread-safe buffer.

Example:

```cpp
class SerialBuffer {
public:
    void push(const std::string& data);

    std::string read();

    bool available() const;

    void clear();

private:
    std::mutex mutex_;
    std::string buffer_;
};
```

The implementation should use appropriate synchronization and avoid unnecessary copying.

---

# 15. Serial.Read()

Current behavior:

```lua
local data = Serial.Read()
```

should return:

```text
data available → string
no data         → nil
```

It must not wait several seconds for new data.

Recommended:

```text
Serial.Read()
     │
     ▼
Check RX Buffer
     │
     ├── Data → Return immediately
     │
     └── Empty → Return nil
```

This is especially important for Lua scripts running every 50–100 ms.

---

# 16. Frame Handling

The Serial Manager should support configurable framing.

Possible modes:

```text
RAW
LINE
FIXED_LENGTH
DELIMITER
TIMEOUT
```

For the citizen card reader:

```text
USB Serial
     │
     ▼
Raw bytes
     │
     ▼
Frame Detection
     │
     ▼
Complete Citizen Card String
     │
     ▼
Lua
```

The Lua layer should not need to reconstruct serial frames if the C++ Serial Manager can reliably do so.

---

# 17. Serial Write Queue

Serial writes should not block the Lua runtime.

Recommended architecture:

```text
Lua
 │
 ▼
Serial.Write()
 │
 ▼
TX Queue
 │
 ▼
TX Worker
 │
 ▼
COM Port
```

Example:

```lua
Serial.Write("HELLO")
```

should normally enqueue the message and return immediately.

The C++ worker performs the actual write.

---

# 18. Windows Implementation

For Windows, investigate asynchronous COM port I/O.

Prefer:

```text
OVERLAPPED I/O
```

for asynchronous operations.

Recommended flow:

```text
CreateFile()
      │
      ▼
FILE_FLAG_OVERLAPPED
      │
      ▼
Configure COM
      │
      ▼
RX OVERLAPPED
      │
      ▼
Wait for event
      │
      ▼
Process received bytes
```

Avoid performing long blocking reads directly inside:

```text
Serial.Open()
Serial.Read()
```

or the Lua execution thread.

---

# 19. Windows COM Port Timeouts

Review:

```cpp
COMMTIMEOUTS
```

configuration.

Avoid configurations that cause an unexpected long blocking interval.

For asynchronous operation, carefully configure:

```text
ReadIntervalTimeout
ReadTotalTimeoutMultiplier
ReadTotalTimeoutConstant
WriteTotalTimeoutMultiplier
WriteTotalTimeoutConstant
```

The exact values should be selected based on the device protocol rather than using arbitrary large timeouts.

---

# 20. Windows Port Cleanup

Inspect:

```cpp
CloseHandle()
CancelIoEx()
PurgeComm()
```

during shutdown.

Do not allow `Serial.Close()` to wait indefinitely for a worker thread.

Recommended:

```text
Close Request
     │
     ▼
Signal Worker
     │
     ▼
Cancel Pending I/O
     │
     ▼
Join Worker
     │
     ▼
Close Handle
```

---

# 21. Linux and macOS

Use the same high-level Serial Manager interface across platforms.

Platform-specific implementation:

```text
SerialManager
      │
      ├── WindowsSerial.cpp
      ├── LinuxSerial.cpp
      └── MacSerial.cpp
```

Linux may use:

```text
termios
poll()
select()
epoll()
```

macOS may use:

```text
termios
poll()
select()
kqueue()
```

The Lua API must remain identical.

---

# 22. Automatic Reconnection

If the serial device is disconnected:

```text
CONNECTED
    │
    ▼
DEVICE ERROR
    │
    ▼
DISCONNECTED
    │
    ▼
RECONNECTING
    │
    ├── Failed → Retry
    │
    └── Success
          │
          ▼
      CONNECTED
```

Do not block the main application while reconnecting.

Use a background worker.

Recommended retry interval:

```text
1 second
2 seconds
5 seconds
```

with configurable retry behavior.

---

# 23. Serial State

Expose the serial state to the Web UI and Lua.

Possible states:

```text
CLOSED
OPENING
OPEN
READING
ERROR
DISCONNECTED
RECONNECTING
```

Example:

```lua
local state = Serial.Status()

Log.Info("Serial status: " .. tostring(state))
```

---

# 24. Configuration

Serial configuration should come from the existing configuration system.

Example:

```json
{
    "serial": {
        "enabled": true,
        "port_name": "COM3",
        "baudrate": 115200,
        "parity": "NONE",
        "stop_bits": 1,
        "data_bits": 8,
        "auto_open": true,
        "auto_reconnect": true
    }
}
```

The same configuration mechanism should be used for `Serial2`.

---

# 25. Serial2

Apply the same architecture to:

```text
Serial2
```

API:

```lua
Serial2.Open()
Serial2.Close()
Serial2.Read()
Serial2.Write(text)
```

`Serial2` must have its own:

```text
Serial Manager
Configuration
RX Buffer
TX Queue
Worker
Connection State
```

It must not block or interfere with `Serial`.

---

# 26. Lua Script Optimization

Change Lua scripts from:

```lua
while true do

    Serial.Open()

    local data = Serial.Read()

    Serial.Close()

    Sleep(100)
end
```

to:

```lua
Serial.Open()

while true do

    local data = Serial.Read()

    if data ~= nil then
        -- Process data
    end

    Sleep(100)
end
```

The physical port remains open.

---

# 27. Better Lua Pattern

For continuously connected devices:

```lua
if not Serial.IsOpen() then
    Serial.Open()
end

while true do

    local data = Serial.Read()

    if data then
        ProcessSerialData(data)
    end

    Sleep(50)
end
```

However, the preferred implementation is to have C++ automatically maintain the connection so Lua does not need to repeatedly check and reconnect.

---

# 28. Avoid Serial Ownership Conflicts

Multiple Lua scripts must not independently control the same physical port.

Bad:

```text
Lua Script A
    └── Serial.Open()
    └── Serial.Close()

Lua Script B
    └── Serial.Open()
    └── Serial.Close()
```

Recommended:

```text
             SerialManager
                  │
          ┌───────┴───────┐
          │               │
       Lua A             Lua B
          │               │
          └───────┬───────┘
                  │
              RX Buffer
                  │
               COM Port
```

The C++ backend owns the hardware.

Lua only consumes the Serial API.

---

# 29. Avoid Blocking the Main Backend

Never execute potentially slow operations directly from:

```text
Main Thread
```

Avoid:

```cpp
mainThread -> Serial.Open()
mainThread -> Serial.Read()
mainThread -> Serial.Close()
```

Prefer:

```text
Main Thread
     │
     ▼
SerialManager
     │
     ├── RX Thread
     ├── TX Thread
     └── Reconnect Thread/Task
```

---

# 30. Performance Targets

After optimization, target:

```text
Serial.Open()
    < 100 ms under normal conditions

Serial.Read()
    < 1 ms when no data is available

Serial.Write()
    < 1–5 ms to enqueue data

Serial.Close()
    < 100 ms during normal shutdown

Lua Serial polling
    must not block the application
```

The exact values should be measured on the target hardware.

The primary requirement is:

> No serial operation should block the entire HSF Gateway for multiple seconds.

---

# 31. Benchmark Test

Create a Serial benchmark test.

Test:

```text
Open
Close
Open
Close
Open
Close
```

at least:

```text
100 iterations
```

Record:

```text
Average Open Time
Maximum Open Time
Minimum Open Time
Average Close Time
Maximum Close Time
Read Latency
Write Latency
```

Example output:

```text
Serial Benchmark

Open:
  Average: 18 ms
  Min:      8 ms
  Max:     41 ms

Close:
  Average:  5 ms
  Min:      2 ms
  Max:     12 ms

Read:
  Average:  0.1 ms

Write:
  Average:  0.3 ms
```

---

# 32. Stress Test

Run:

```text
Lua Script 1
Lua Script 2
Lua Script 3
Modbus
REST API
WebSocket
Serial
```

simultaneously.

Verify that serial communication does not cause:

* Web UI freezes
* Lua scheduling delays
* Modbus delays
* REST API delays
* WebSocket delays
* CPU spikes
* Deadlocks

---

# 33. Failure Tests

Test:

```text
COM Port Does Not Exist
COM Port Already In Use
USB Serial Disconnected
USB Serial Reconnected
Invalid Serial Configuration
Device Stops Responding
Serial Read Error
Serial Write Error
Application Shutdown During Read
Application Shutdown During Write
```

The application must recover without crashing.

---

# 34. Logging

Add structured serial logs:

```text
Serial.Open
Serial.Close
Serial.Connect
Serial.Disconnect
Serial.Reconnect
Serial.Read
Serial.Write
Serial.Error
Serial.Timeout
```

Example:

```text
[INFO] Serial COM3 connected
[INFO] Serial RX 42 bytes
[INFO] Serial TX 12 bytes
[WARNING] Serial device disconnected
[INFO] Serial reconnecting...
[INFO] Serial COM3 reconnected
```

Avoid logging every byte at normal log level because this can create excessive I/O.

Use `DEBUG` level for raw traffic.

---

# 35. Implementation Plan

## Phase 1 – Investigation

* [ ] Review existing C++ Serial implementation.
* [ ] Measure `Serial.Open()` execution time.
* [ ] Measure `Serial.Close()` execution time.
* [ ] Measure `Serial.Read()` execution time.
* [ ] Measure `Serial.Write()` execution time.
* [ ] Identify the operation causing the 10–15 second delay.
* [ ] Add high-resolution performance logging.

## Phase 2 – Serial Manager

* [ ] Implement `SerialManager`.
* [ ] Separate hardware lifecycle from Lua API.
* [ ] Implement persistent connection.
* [ ] Implement connection state.
* [ ] Implement thread-safe RX buffer.
* [ ] Implement TX queue.
* [ ] Implement background RX worker.
* [ ] Implement asynchronous TX.
* [ ] Implement error handling.

## Phase 3 – Windows Optimization

* [ ] Review COM port `CreateFile()`.
* [ ] Evaluate `FILE_FLAG_OVERLAPPED`.
* [ ] Implement asynchronous COM I/O.
* [ ] Review `COMMTIMEOUTS`.
* [ ] Implement safe `CancelIoEx()`.
* [ ] Optimize `CloseHandle()`.
* [ ] Ensure worker shutdown is bounded.

## Phase 4 – Lua API

* [ ] Preserve existing `Serial.*` API.
* [ ] Make `Serial.Open()` idempotent.
* [ ] Prevent repeated physical port opening.
* [ ] Make `Serial.Read()` non-blocking.
* [ ] Make `Serial.Write()` asynchronous.
* [ ] Update Lua examples.
* [ ] Update Card Reader workflow.

## Phase 5 – Serial2

* [ ] Apply the same architecture to `Serial2`.
* [ ] Verify independent configuration.
* [ ] Verify independent buffers.
* [ ] Verify independent workers.

## Phase 6 – Testing

* [ ] Open/close benchmark.
* [ ] Continuous RX test.
* [ ] Continuous TX test.
* [ ] Long-running test.
* [ ] USB disconnect/reconnect test.
* [ ] Multiple Lua script test.
* [ ] Full Gateway integration test.
* [ ] Windows test.
* [ ] Linux test.
* [ ] macOS test.

---

# 36. Acceptance Criteria

The implementation is considered successful when:

* [ ] `Serial.Open()` no longer blocks the gateway for 10–15 seconds.
* [ ] Physical COM port is not repeatedly opened/closed by Lua polling.
* [ ] Serial port remains connected during normal operation.
* [ ] Serial RX runs independently from the Lua execution thread.
* [ ] Serial TX does not block Lua.
* [ ] `Serial.Read()` returns immediately when no data is available.
* [ ] Multiple Lua scripts can safely use the Serial subsystem.
* [ ] Serial errors do not stop unrelated Lua scripts.
* [ ] USB disconnect/reconnect is handled automatically.
* [ ] Serial2 has the same optimized behavior.
* [ ] WebSocket and Web UI remain responsive during Serial operations.
* [ ] Modbus and REST operations remain responsive.
* [ ] No deadlocks occur during application shutdown.
* [ ] Performance benchmarks meet the defined targets.

---

# 37. Final Target Architecture

```text
                         HSF Gateway
                              │
                     ┌────────┴────────┐
                     │                 │
                 Main Thread       Web Server
                     │                 │
                     │              WebSocket
                     │
              ┌──────┴──────┐
              │              │
        Lua Runtime      SerialManager
              │              │
       ┌──────┼──────┐       │
       │      │      │       ├── RX Worker
     Lua1   Lua2   Lua3      ├── TX Worker
       │      │      │       ├── Reconnect
       └──────┼──────┘       └── Buffers
              │
              │ Serial API
              ▼
       ┌────────────────┐
       │ SerialManager  │
       └───────┬────────┘
               │
               ▼
          Windows COM
               │
               ▼
       USB Serial Device
```

## Core Design Rule

**C++ owns the physical serial connection. Lua owns the application logic.**

Lua scripts should never repeatedly open and close the physical COM port during normal operation.

The Serial Manager should maintain a persistent, asynchronous, thread-safe connection and expose a lightweight non-blocking API to Lua.

procced.