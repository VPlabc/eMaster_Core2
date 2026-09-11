# C++ Modules

Date: 2026-08-29

## Keep in C++ during the early migration

These components already contain substantial business logic, protocol logic, or
third-party integration value and should remain implemented in C++ while the
Core seams move first.

| Component | Category | Reason to keep in C++ now |
|---|---|---|
| `WebServer` | Web/API | Large Crow-based route surface and UI integration |
| `LuaEngine` | Scripting | Deep Lua binding logic and runtime state handling |
| `LuaRuntimeManager` | Runtime orchestration | Multi-runtime lifecycle and event fan-out |
| `ModbusClient` | Protocol/device | Existing working protocol implementation |
| `MqClient` | Protocol/integration | AMQP-CPP integration and reconnect logic |
| `ZkController`, `C3Client`, `PullSdkClient` | Protocol/device | Device control, RTLog, SDK integration |
| `RfidClient` | Device/integration | Runtime device-mode handling |
| `RestClient` | Integration | HTTP helper with config/retry behavior |
| `SqlDatabase` and store classes | Database | SQLite wrappers and schema-specific behavior |
| `UpdateManager` and `update/*` | Update orchestration | Existing OTA/install/rollback logic |
| `plugin_manager/*` | Native module host | Current dynamic loading and host service bridge |

## Better candidates for later C-oriented Core seams

These should become C-facing Core services first, even if they continue to call
into C++ during transition:

- lifecycle coordination
- logging facade
- generic configuration access
- health/status reporting
- event dispatch
- timer registration
- plugin/module lifecycle contracts

## Current internal seam status

- Public C ABI: not present yet
- Internal named service registry: present
- Platform isolation: partial
- Generic module configuration: not present yet
