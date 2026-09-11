# Platform Matrix

Only rows with an actual build or test result are marked `PASS`.

| Capability | Windows x86 build host | Linux x86_64 | Linux ARM64 / RK3568 | Windows x86_64 |
| --- | --- | --- | --- | --- |
| Existing C++ gateway build | PASS (Release, 2026-09-02) | Pending | Blocked: vcpkg libsodium pkg-config staging | Pending |
| Existing SDK/plugin tests | PASS (96 assertions) | Pending | Pending | Pending |
| Rust Phase 1 workspace | PASS (`cargo test --workspace`) | Pending | Pending | Pending |
| Modbus/C3 protocol tests | PASS (2 + 2 assertions) | Pending | Pending | Pending |
| REST/Lua/RabbitMQ integration | Not exercised | Pending | Pending | Pending |
| Hardware serial/ZK devices | Not exercised | Pending | Pending | Pending |

The ARM64 attempt reached Ubuntu 18.04 aarch64, GCC 9.4.0, CMake 3.28.6, and
the `arm64-linux-release` vcpkg triplet. It currently fails in the pinned
vcpkg-make/libsodium port: autotools stages under `usr/local`, and vcpkg's
pkg-config fixup cannot find `libsodium.pc`. This is a dependency/tooling
blocker, not evidence that the gateway itself is ARM64-incompatible.

