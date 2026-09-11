# Dependency Matrix

Date: 2026-08-29

## Primary dependency source

- Manifest: `vcpkg.json`
- Pinned baseline: `39344dff01c5a5a0134caf2624cdd492f05d30ea`

## Dependencies

| Dependency | Source | Required | Main consumers | Notes |
|---|---|---:|---|---|
| `crow` | vcpkg / `find_package(Crow)` | Yes | `WebServer` | REST API, WebSocket UI, locker UI |
| `asio` | vcpkg manifest | Indirect | Crow / networking stack | Present in manifest; not linked directly in `CMakeLists.txt` |
| `nlohmann-json` | vcpkg / `find_package(nlohmann_json)` | Yes | `ConfigManager`, `WebServer`, plugin/config/update paths | Core JSON model |
| `lua` | vcpkg / `find_package(Lua)` | Yes | `LuaEngine`, package compiler/runtime | Embedded scripting runtime |
| `sqlite3` | vcpkg / `find_package(SQLite3)` | Yes | `ConfigManager`, `LogStore`, `SecurityStore`, `CardClientManager`, `SqlDatabase` | Persistent config and runtime data |
| `curl` | vcpkg / `find_package(CURL)` | Yes | `RestClient`, update downloader | HTTP client |
| `libsodium` | vcpkg / `find_package(unofficial-sodium)` | Yes | security and package/update signing flows | Hard requirement for auth/password hashing |
| `openssl` | vcpkg / `find_package(OpenSSL QUIET)` | Optional | `SignatureVerifier` | Missing dependency builds a fail-closed signature stub |
| `amqpcpp` | vcpkg / `find_package(amqpcpp QUIET)` | Optional | `MqClient` | Missing dependency builds `MqClient_stub.cpp` |

## Non-vcpkg native/platform dependencies

| Dependency | Required | Scope | Notes |
|---|---:|---|---|
| Visual Studio C++ workload | Windows baseline | `build.bat` | Needed for `cl.exe`, MSBuild, and x86 toolchain |
| `plcommpro.dll` and companion DLLs | Optional feature | Windows x86 only | ZK PullSDK support; enforced by CMake guards |
| POSIX threads / `Threads::Threads` | Yes | Linux/POSIX | General background-thread infrastructure |
| Winsock / `ws2_32`, `mswsock`, `iphlpapi` | Windows | Windows builds | Socket and ping support |
| `libdl` via `${CMAKE_DL_LIBS}` | Platform-dependent | plugin loader | Used for dynamic plugin loading on POSIX |

## Built-in optional-feature pattern

The repository already treats several dependencies as capability gates rather
than hard build blockers:

- RabbitMQ: real client or stub
- OpenSSL signatures: verifier or fail-closed stub
- PullSDK ZK path: real implementation or platform stub

This pattern is the right precedent for future module/plugin capability flags.
