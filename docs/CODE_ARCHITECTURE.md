# Code Architecture

Date: 2026-08-29

## Current shape

The gateway is still a C++17 monolith centered on `src/main.cpp`, with
concrete module construction in-process and no public interface layer between
Core orchestration and protocol/business modules.

The main runtime subsystems are:

- Boot/lifecycle: `src/main.cpp`
- Web/API surface: `src/WebServer.cpp`
- Lua runtime and bindings: `src/LuaEngine.cpp`, `src/LuaRuntimeManager.cpp`
- Configuration/state stores: `ConfigManager`, `LogStore`, `SecurityStore`,
  `CardClientManager`, `SqlDatabase`
- Device/protocol modules: `SerialPort`, `ModbusClient`, `RfidClient`,
  `MqClient`, `zk_controller/*`
- Update system: `src/update/*`
- Native plugin host: `src/plugin_manager/*`

## New internal seam added in this migration slice

The fixed positional wiring between `main.cpp`, `LuaRuntimeManager`, and
`WebServer` is now fronted by `include/hsf/ServiceRegistry.h`.

Current responsibilities of the registry:

- named service lookup
- capability lookup
- per-service lifecycle state
- per-service health state

This remains an internal C++ seam. It does not yet define a public ABI.

## Control flow

1. `main.cpp` loads config and persistent stores.
2. Concrete modules are constructed in-process.
3. Services are registered in `ServiceRegistry`.
4. `LuaRuntimeManager` binds to device/event services through the registry.
5. `WebServer` resolves its required services from the registry.
6. Background loops handle:
   - PLC polling
   - plugin event dispatch
   - web broadcast/status
   - update checks

## Current architectural chokepoints

- `main.cpp` still owns process construction and coarse lifecycle ordering.
- `LuaEngine.cpp` is still the concrete Lua API surface for built-in modules.
- `WebServer.cpp` still contains the bulk of REST/UI handling logic.
- `ConfigManager` still exposes typed built-in protocol config sections.

## Near-term migration implication

The registry removes the highest-friction positional wiring seam without
changing external Lua globals or REST endpoints. That enables later work to
move lifecycle, health, config, and event services behind narrower boundaries
without first rewriting protocol modules.
