# Migration Test Report

Date: 2026-09-03

## Local Rust gates

All commands completed successfully on the Windows x86_64 development host:

```text
cargo fmt --all -- --check
cargo check --workspace
cargo test --workspace
cargo clippy --workspace --all-targets -- -D warnings
cargo build --workspace --release
```

The workspace currently contains 20 passing unit tests and one passing
cross-crate integration test across the core,
configuration, event, runtime, plugin, Modbus, serial, HTTP, RabbitMQ, and Lua
crates.

## Native/C3 validation

The Windows Release gateway build completed successfully after pinning the
vcpkg PowerShell path and removing the inaccessible WindowsApps `pwsh.exe`
shim from the build PATH. The C3 C
ABI test executable passed its stateless codec and opaque-client ownership/
buffer checks, and the existing C3 protocol tests passed.

The native Windows CTest suite passed all 9 configured tests in 22.83 seconds.

## Cross-platform status

- Windows x86_64: Rust gates and existing C++ Release build passed locally.
- Linux x86_64: covered by the repository CI workflow, pending hosted-run
  execution.
- Linux ARM64/RK3568: Rust cross-target check is defined in CI. The native C++
  build remains blocked by the pinned vcpkg/libsodium pkg-config staging issue
  documented in `docs/PLATFORM_MATRIX.md`.

## Remaining validation

Real PLC, serial-device, RabbitMQ-broker, Lua-VM, plugin-loader, and hardware
failure-injection tests remain deployment/integration work. They must pass
before those capabilities are marked supported in the platform matrix.
