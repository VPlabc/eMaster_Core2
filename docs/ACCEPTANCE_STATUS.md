# Acceptance status

Status for the current `1.2.0` test build:

| Area | Status | Evidence or blocker |
|---|---|---|
| Rust workspace | Passed | `cargo test --workspace -q` |
| Production MQTT/RabbitMQ compilation | Passed | Windows production-feature checks |
| C ABI | Passed | `hsf_connectivity_abi_tests.exe` |
| C3 FFI | Passed | `hsf_c3_ffi_tests.exe` |
| Dynamic configuration | Passed | `hsf_dynamic_config_tests.exe` |
| Service registry | Passed | `hsf_service_registry_tests.exe` |
| Windows x86 Release build | Passed | `build.bat Release` |
| Windows x86 package | Passed | `HSF-Gateway-v1.2.0-windows-x86.zip` |
| Package checksum/tamper checks | Passed | `test-package-verifier.ps1` |
| Update data preservation fixture | Passed | `test-update-fixture.ps1` |
| Ubuntu 18/22 build | CI pending | Requires Linux/Docker runner |
| ARM64 build/package | CI pending | Requires ARM64 or emulated target runner |
| Windows 11 IoT runtime | Pending | Requires IoT SDK/device |
| Live MQTT/RabbitMQ integration | Pending | Requires reachable brokers |
| OTA install/rollback | Pending | Requires managed deployment fixture and restart control |
| Long-duration soak | Pending | Requires deployed gateway and monitoring window |

## Current environment blockers

The current Windows host cannot enumerate WSL (`E_ACCESSDENIED`) and the
Docker daemon is unavailable (`docker_engine` access/connect failure). These
restrictions prevent local execution of Ubuntu, ARM64, and broker-container
gates; the corresponding CI workflows remain the reproducible execution path.

“Passed” means the check ran locally or in an available automated job. A
“Pending” item must not be represented as a successful target-runtime test.
