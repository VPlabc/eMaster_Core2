# Platform Dependencies

Date: 2026-08-29

## OS-specific source files already present

| Area | Windows | POSIX/Linux | Notes |
|---|---|---|---|
| Serial port | `src/SerialPort_win.cpp` | `src/SerialPort_posix.cpp` | Existing clean platform seam |
| ZK PullSDK | `src/zk_controller/PullSdkClient.cpp` | `src/zk_controller/PullSdkClient_stub.cpp` | Real implementation is Windows x86 only |
| Shared TCP transport | `src/TcpSocket.cpp` | `src/TcpSocket.cpp` | Unified class wrapping Winsock / BSD sockets |

## Direct platform APIs in use

| API area | Current locations | Migration note |
|---|---|---|
| Process executable path | `main.cpp` | Windows `GetModuleFileNameW`, Linux `/proc/self/exe` |
| Serial APIs | `SerialPort_*` | Already isolated and should stay that way |
| TCP sockets | `TcpSocket.cpp` | Good candidate for a future transport/platform boundary |
| ICMP / ping | `NetPing.cpp` | OS-specific behavior hidden behind one class today |
| Dynamic loading | `plugin_manager/*` | Natural future Core/platform seam |
| Filesystem/path layout | `main.cpp`, update/package flows, `ConfigManager` | Currently spread across runtime and build/package code |
| Service/process restart | `UpdateManager` restart callback + scripts | Needs explicit target-profile handling |

## Platform-specific constraints already encoded

- PullSDK requires Windows x86
- Linux builds force `HSF_ENABLE_ZK=OFF` for PullSDK while retaining C3 support
- Ubuntu 18 compatibility requires building against an older glibc baseline
- Windows builds rely on Visual Studio toolchain discovery in `build.bat`

## Recommended extraction order

1. transport primitives (`TcpSocket`, serial abstraction)
2. filesystem/layout resolution
3. dynamic library loading
4. timers/process restart helpers
5. service/daemon integration specifics
