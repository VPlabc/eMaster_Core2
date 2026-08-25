# HSF Plugin SDK

Write a protocol driver for the HSF Gateway as a shared object, without
modifying the gateway.

Header-only. Nine C headers define a stable C ABI; three C++ headers add
ergonomics that are *not* part of the ABI. You link no SDK object code, so a
plugin built against 1.0 keeps working when the gateway's own code changes
underneath it.

Status: **Plugin API 1.0**, delivered by phases P0–P2 of
`request/updatePluginStruct.md`. The Plugin Manager that loads these (P1 of the
plan's own numbering, Phase 4) does not exist yet — see [What is not here
yet](#what-is-not-here-yet).

## Quick start

```bash
# Install the SDK somewhere
cmake -S sdk/hsf-plugin-sdk -B build-sdk -DCMAKE_INSTALL_PREFIX=$HOME/hsf-sdk
cmake --install build-sdk

# Copy the template and build it — nothing else is required
cp -r plugins/template-cpp my-driver && cd my-driver
cmake -B build -DCMAKE_PREFIX_PATH=$HOME/hsf-sdk
cmake --build build
ctest --test-dir build
```

That produces `build/plugin.so` and `build/manifest.json`, which is exactly the
pair the Plugin Manager installs.

You need CMake 3.16. C++ plugins require C++17; C plugins require only a C11
compiler. You do **not** need the gateway, its vcpkg dependencies, Crow, Lua,
or SQLite.

## C plugin quick start

The ABI headers are C-compatible and the SDK provides a C-only target and
helper:

```cmake
find_package(HSFPluginSDK REQUIRED)
hsf_add_c_plugin(
  NAME hsf.driver.my_c_driver
  SOURCES src/my_c_driver.c
  MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json)
set_target_properties(hsf_driver_my_c_driver PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED ON)
```

Include `hsf/plugin.h`, implement the plugin and driver vtables, then finish
with `HSF_PLUGIN_DEFINE(&kInfo, create_plugin, destroy_plugin)`. A complete
C11 driver is available in `examples/hello-c-plugin/`.

## The four exported symbols

A plugin exports these and nothing else:

```c
uint32_t          hsf_plugin_abi_version(void);
const HSFPluginInfo* hsf_plugin_get_info(void);
HSFStatus         hsf_plugin_create(const HSFPluginContext*, HSFPlugin**,
                                    const HSFPluginVTable**);
void              hsf_plugin_destroy(HSFPlugin*);
```

`HSF_PLUGIN_DEFINE(&info, Create, Destroy)` writes all four. Everything else
stays hidden — `hsf_add_plugin()` compiles with hidden visibility, so two
plugins that happen to share an internal symbol name cannot collide at load
time.

## Headers

| Header | Contains |
|---|---|
| `hsf/version.h` | API version, compatibility rule, platform triple, export macros |
| `hsf/error.h` | `HSFStatus`, `HSFStr` |
| `hsf/value.h` | `HSFValue` tagged union, encodings, byte orders |
| `hsf/logging.h` | logging — host provides |
| `hsf/config.h` | configuration, scoped to your plugin — host provides |
| `hsf/event.h` | event bus — host provides |
| `hsf/transport.h` | serial/TCP/UDP/CAN/SPI/GPIO/mock — host provides |
| `hsf/device.h` | device and point addressing |
| `hsf/driver.h` | the driver vtable — **you** provide |
| `hsf/plugin.h` | entry points, `HSFPluginContext`. Include this one. |
| `hsf/support.hpp` | C++: `DriverBase`, `BlockingIo`, `Guard`. Not ABI. |
| `hsf/mock_transport.hpp` | test-only fake transport. Not ABI. |
| `hsf/testing.hpp` | test harness, no dependencies. Not ABI. |

## Four rules that matter more than the rest

**1. Nothing with a C++ type crosses the boundary.** No `std::string`, no
`std::vector`, no smart pointers in any signature. Use `HSFStr` for borrowed
text and opaque handles for everything else. Internally, use whatever you like.

**2. Never let an exception escape.** An exception unwinding into the host is
undefined behaviour and in practice terminates the gateway — so one driver's bad
`std::stoi` takes down every other protocol. Wrap every entry point in
`hsf::Guard`; `DriverBase` already does.

**3. The transport is non-blocking, and that is deliberate.** `read` returns
`HSF_AGAIN` when it would block, and `HSF_OK` with zero bytes when the peer has
closed. Those are different values because conflating them is *the* classic
non-blocking bug. If you want blocking semantics, use `hsf::BlockingIo` and
declare `owns_thread = 1`. If you want to run on the host's shared event loop —
which is what makes the 256 MB target reachable — implement `on_readable`
instead and leave `owns_thread` at 0.

**4. Borrowed means borrowed.** Every `HSFStr` and `HSFBlob` is valid only for
the duration of the call that produced it, unless a slot says otherwise. Host
and plugin do not share an allocator, so neither may free the other's memory.
When returning one, point it at storage that outlives the call — a member, or a
static. Pointing it at a local is the mistake this SDK's own tests check for.

## Versioning

The Plugin API is versioned independently of the gateway (a gateway at 1.0.0 may
host Plugin API 1.0; the numbers are unrelated).

- **Major must match exactly.** A struct changed shape or a slot changed
  meaning; loading across it is undefined behaviour, not a degraded mode.
- **Minor: host ≥ plugin.** Minor bumps only *append* vtable slots and struct
  fields, so a newer host runs an older plugin. The reverse is refused, because
  the plugin may call a slot the host lacks.

Every versioned struct carries `struct_size` first, which is what makes
appending safe. The version says *which contract*; the size says *how much of it
is present*.

## Testing without hardware

`hsf/mock_transport.hpp` presents itself through the same `HSFTransportVTable`
as a real serial port, so your driver cannot tell the difference — which is the
only way the test proves anything about production. It can also be hostile in
ways real hardware cannot be on demand:

```cpp
hsf::MockTransport t;
t.SetResponder(device_logic, /*fragment=*/2);  // reply arrives 2 bytes at a time
t.StallReads(3);                               // three HSF_AGAIN first
t.LimitNextWrite(4);                           // truncated write
t.FailReadAfter(16, HSF_ERR_IO);               // error at an exact byte
t.CloseRemote();                               // peer hangs up
```

Structure your driver so the protocol is pure functions over byte buffers —
`BuildRequest`, `FrameLength`, `ParseReply` in the template — and test those
directly, with no transport at all. Protocol bugs are cheap to find in a pure
function and expensive to find over a wire.

## What is not here yet

P0–P2 delivered the SDK, the ABI, the mock transport, the test harness, the
CMake integration and the template. Still to come, in the plan's order:

- **Plugin Manager** — discovery, manifest validation, load/unload,
  enable/disable, the state machine, health checks
- **Host-side transports** — the real serial/TCP/UDP implementations behind
  `HSFTransportVTable`, and the event loop that drives `on_readable`
- **`.hsfplugin` packaging** — install, update, rollback. The gateway's OTA
  updater and Lua package manager already do signed, verified,
  rollback-capable artifact deployment; that machinery should be reused rather
  than rewritten
- **Signing and permission enforcement** — libsodium and
  `src/update/SignatureVerifier.cpp` are already in the tree
- **Driver migration** — Modbus and ZK moving out of Core

Two constraints found during P0 that will shape those phases are written up in
`docs/architecture/current-state.md` §12: Crow builds its routing table at
start-up so a plugin cannot add a URL, and the gateway's protocol-specific
configuration is compiled into `ConfigManager` and has to become
schema-described plugin data.
