# Native Dependency Inventory

This inventory is the migration baseline. Versions are taken from the current
`vcpkg.json`/build output; a target is supported only after its dependency set
has been built and exercised on that target.

| Dependency | Version observed | Language/ABI | Build source | Current target evidence |
| --- | --- | --- | --- | --- |
| SQLite3 | 3.53.4 | C | vcpkg | Windows x86 Release gateway build |
| libcurl | 8.21.0 | C | vcpkg | Windows x86 Release gateway build |
| OpenSSL | 3.6.3 | C | vcpkg | Windows x86 Release gateway build |
| Lua | 5.5.0 | C | vcpkg | Windows x86 Release gateway build |
| RabbitMQ/AMQP-CPP | 4.3.27 | C++ | vcpkg | Windows x86 Release gateway build |
| Asio | 1.32.0 | C++ headers | vcpkg | Windows x86 Release gateway build |
| Crow | 1.3.3 | C++ headers | vcpkg | Windows x86 Release gateway build |
| nlohmann-json | 3.12.0 | C++ headers | vcpkg | Windows x86 Release gateway build |
| libsodium | 1.0.22 | C | vcpkg | ARM64 build blocked in pkg-config staging |
| ZK `plcommpro` SDK | repository vendor files | vendor C ABI | `third_party/plcommpro` | Windows build only; Linux/ARM64 availability pending |

The Rust skeleton introduced in Phase 1 intentionally has no third-party native
dependencies. FFI and legacy SDK dependencies will be added behind explicit
target profiles after their headers, libraries, ABI, and licenses are recorded.

