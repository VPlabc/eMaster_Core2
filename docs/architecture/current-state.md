# Current state of the HSF Gateway source base

Deliverable for Phase 1 of `request/updatePluginStruct.md` (§28). This describes
the tree **as it is**, not as the plugin plan wants it, so that later phases can
be sequenced against facts rather than assumptions. Every number here was
measured on the tree at the commit this document was added.

Read §12 "Consequences for the plan" last — that is where the findings turn
into decisions, and it is the part that changes the migration order.

---

## 1. Size and shape

22,159 lines of C++ across 31 translation units, in a flat layout: `src/*.cpp`
plus four subdirectories (`security/`, `update/`, `lua_package/`,
`zk_controller/`) that were added later and are the only existing hint of
module grouping.

The ten largest units:

| Lines | Unit | Role |
|------:|------|------|
| 4,254 | `WebServer.cpp` | REST + WebSocket, two Crow apps, static file serving |
| 3,545 | `LuaEngine.cpp` | Lua VM, every native binding |
| 1,093 | `MqClient.cpp` | RabbitMQ (AMQP-CPP as codec over our own socket) |
| 794 | `security/SecurityStore.cpp` | accounts, sessions, lockout, audit |
| 793 | `ConfigManager.cpp` | all configuration |
| 714 | `LogStore.cpp` | structured business logs |
| 673 | `main.cpp` | construction and wiring of everything |
| 665 | `zk_controller/ZkController.cpp` | ZK panel lifecycle |
| 665 | `update/UpdateManager.cpp` | OTA |
| 614 | `ModbusClient.cpp` | Modbus TCP |

`WebServer.cpp` and `LuaEngine.cpp` are 35% of the codebase between them. Both
are places where *every* protocol has to be touched to add a feature, which is
the structural problem the plugin plan exists to solve.

## 2. There are no interfaces

**The public header tree contains zero `virtual` functions.** Not one abstract
base class, not one polymorphic seam, anywhere in `include/hsf/`.

This is the single most consequential finding in this document, and it cuts
both ways.

It is bad because there is no existing seam to insert the plan's Driver API or
Transport API *at*. Nothing can be swapped, mocked, or loaded dynamically
today. A test cannot substitute a fake transport under `ModbusClient` because
the type is fixed at compile time (see §4).

It is good because there is no legacy vtable, no existing plugin ABI, and no
third-party code depending on a shape we would have to preserve. §4 of the plan
asks for a stable C ABI with opaque handles; we get to define it once, cleanly,
with no compatibility debt.

## 3. Ownership and wiring

`main.cpp` constructs every module as a **stack object** and wires them with
raw, non-owning pointers. There is no dependency-injection container, no
factory, and no registry:

```cpp
hsf::RestClient  rest;
hsf::SerialPort  serial;
hsf::ModbusClient modbus;
hsf::RfidClient  rfid;
hsf::ZkController zk;
hsf::MqClient    mq;
hsf::LuaRuntimeManager lua;
hsf::WebServer   web;
hsf::UpdateManager updater;

lua.Bind(&rest, &serial, &serial2, &modbus, &rfid, &zk, &mq);
web.SetModules(&rest, &serial, &serial2, &modbus, &rfid, &lua, &mq, &zk, &updater);
```

Those two calls are the crux of the migration. Both take a **fixed positional
argument list of concrete module pointers**. Adding a protocol today means
changing both signatures, plus `LuaEngine`, `WebServer` and `ConfigManager`.
That is precisely what the plan's §32 says must stop being necessary.

Lifetime is therefore static: modules live for the process, are never created
or destroyed at runtime, and cannot be unloaded. The plan's plugin state
machine (§17: `ENABLED`/`DISABLED`/`RUNNING`, load/unload) has no counterpart
here — it is entirely new capability, not a refactor of something existing.

## 4. Transports: a de-facto abstraction that is not one

`TcpSocket` is already the shared network transport. It wraps BSD sockets and
Winsock behind one class and is used by `ModbusClient`, `RfidClient`,
`C3Client`, `MqClient`, `NetPing` and the Lua `Tcp.*` bindings.

But it is a **concrete class held by value**:

```cpp
// include/hsf/ModbusClient.h            TcpSocket socket_;
// include/hsf/RfidClient.h              TcpSocket socket_;
// include/hsf/zk_controller/C3Client.h  TcpSocket socket_;
```

So the *concept* of a reusable transport already exists and is proven across
five consumers — the plan is not introducing a foreign idea. What is missing is
only the indirection: an interface, and construction from outside rather than
composition by value. That makes the Transport API (plan §8, Phase 3) a
comparatively cheap and low-risk change, and a good first one.

Serial is further along in one respect and behind in another. `SerialPort`
already splits cleanly into `SerialPort.cpp` (shared open/close and read-loop
logic) plus `SerialPort_posix.cpp` (termios) and `SerialPort_win.cpp` (Win32
`DCB`/`COMMTIMEOUTS`), selected by `WIN32` in CMake. That is the right internal
seam. But it is a serial *port*, not a serial *transport*: it owns its own read
thread and delivers lines through a single callback slot, a shape driven by the
card reader rather than by a general transport contract.

The complete list of places the process touches hardware or the OS directly:

| Access | Where | Notes |
|---|---|---|
| POSIX `termios`, `open` | `SerialPort_posix.cpp` | USB serial and main UART both |
| Win32 `CreateFile`, `DCB` | `SerialPort_win.cpp` | |
| BSD sockets / Winsock | `TcpSocket.cpp` | shared by 5 consumers |
| ICMP / ping | `NetPing.cpp` | |
| ZKTeco PullSDK DLL | `PullSdkClient.cpp` | 32-bit Windows only; `_stub.cpp` elsewhere |
| SQLite files | `SqlDatabase`, `ConfigManager`, `LogStore`, `SecurityStore`, `CardClientManager` | five separate stores |

Notably **no driver hard-codes a device path**. `/dev/ttyUSB0`, `192.168.1.10`
and so on appear only as *defaults on config structs* in
`include/hsf/ConfigManager.h`, never inside `ModbusClient` or `SerialPort`
logic. The plan's §8 requirement ("must not contain hard-coded assumptions
about `/dev/ttyUSB0`") is therefore already met, and the transport work does
not have to undo anything.

## 5. Threading: the plan's hardest constraint

Eight modules own background threads, and `WebServer` alone has eight
thread-construction sites (two Crow apps plus a 1 s broadcast loop):

| Threads | Module |
|---:|---|
| 8 | `WebServer` (2 Crow apps + broadcast) |
| 4 | `LuaRuntimeManager` (one per running script) |
| 2 | `MqClient` (IO thread) |
| 1 each | `SerialPort`, `RfidClient`, `ZkController`, `UpdateManager`, `main`'s PLC poll loop |

Crow additionally runs a worker pool — 16 threads on this development machine,
sized from hardware concurrency.

Set against the plan's §27 minimum target of **2 cores / 256 MB** and its
explicit "avoid unnecessary threads" and "avoid process-per-plugin by default"
requirements, this is the largest gap between where the code is and where the
plan wants it. It is a gap in *design*, not in plugin packaging: wrapping
today's thread-per-module drivers behind a C ABI would preserve the thread
count exactly and satisfy none of §27. See §12.

## 6. Shared global state

Eight singletons, each internally thread-safe, reachable from anywhere:

`Logger`, `ConfigManager`, `RuntimeVariables`, `LogStore`, `CardCache`,
`CardClientManager`, `ModbusRegistry`, `SecurityStore`.

Four are squarely Core services the plan expects to expose to plugins through
the SDK (§3.1: logging, configuration, events): `Logger`, `ConfigManager`,
`RuntimeVariables`, `LogStore`. Exposing them across a C ABI means wrapping
each in a context vtable handed to the plugin at creation —
`HSFPluginContext` in the plan's §4 sketch — rather than letting a plugin call
`Instance()`, which would not link across a shared-object boundary anyway.

Two are protocol-specific state sitting in Core: `ModbusRegistry` (Modbus
points, tagged by Lua runtime id) and `CardCache`. Those move with their
plugins.

There is **no event bus**. The plan lists one as a Core responsibility (§3.1)
and an SDK API (Phase 2). Today events propagate as direct callbacks:
`SerialPort` → `LuaRuntimeManager` fan-out → each `LuaEngine`; ZK RTLog →
queued Lua events; PLC transitions → `main.cpp`'s poll thread. So the Event API
is new construction, and it is the piece that decouples `LuaRuntimeManager`
from concrete module pointers.

## 7. Lua surface

113 native C-function registrations, exposed as 14 globals plus one `require`d
module:

```
Card  Config  Db  GetVariable  Http  Log  Modbus  Mq  Rest  Rfid
Serial  Serial2  SetVariable  Sleep  Tcp      require("zk_controller")
```

`zk_controller` is the outlier and the precedent worth following: it is
registered into `package.preload` rather than as a global. That is exactly the
mechanism a plugin should use to publish its Lua API (plan §16), and it already
works — `LuaEngine::RegisterZkControllerModule` is six lines. A plugin Lua
binding registry can generalise it without inventing a new scheme.

Protocol-specific globals (`Modbus`, `Rfid`, `Serial`, `Serial2`, `Mq`, `Card`)
must keep working after migration; §29 requires existing scripts to run
unchanged. The `smartlocker` application under `config/scripts/` is the real
regression suite for that, and it already ships Lua tests
(`config/scripts/smartlocker/tests/`) covering Modbus input, ZK control and the
locker state machine.

## 8. REST surface and the Test Tool

115 Crow routes. By area:

| Routes | Area |
|---:|---|
| 34 | `/api/test` (Test Tool) |
| 23 | `/api/lua` |
| 9 | `/api/locker` |
| 6 | `/api/card` |
| 5 each | `/api/logs`, `/api/auth` |
| 4 each | `/api/update`, `/api/modbus`, `/api/app` |
| 3 | `/api/serial` |
| 21 | everything else |

One finding here is unexpectedly good news. The plan's §20 warns: "Do not
create a second driver implementation exclusively for testing." **The Test Tool
already complies.** Its 34 routes call straight through to the live module
pointers (`zk->IsConnected()`, `zk->IoState()`, and so on) — there is no
parallel test-only driver anywhere. So Phase 7 is about making the Test Tool
*discover* plugins dynamically instead of naming them, not about deleting a
duplicate implementation.

Routing itself is a hard constraint the plan does not mention: **Crow builds its
routing table at start-up, so a plugin cannot add a URL.** The gateway already
solved this once for Lua, with `/api/app/<path>` forwarding to whatever script
registered that path via `Http.Register`. Plugin REST endpoints will need the
same treatment — a fixed prefix that dispatches by lookup — or plugins must be
loaded before the app starts listening.

## 9. Configuration is protocol-aware

`ConfigManager` defines 13 config structs and names every protocol explicitly:

```
SystemConfig  WebConfig  RestConfig  SerialConfig  ModbusConfig  RfidConfig
ZkConfig  MqConfig  LuaConfig  LoggingConfig  AuthConfig  CardApiConfig
UpdateConfig
```

with typed accessors `GetSerial()`, `GetModbus()`, `GetRfid()`, `GetZk()`,
`GetMq()`. Six of the thirteen are protocol-specific, and each is a compiled-in
C++ struct with a hand-written JSON mapping in `ApplyJson`/`ToJson`.

This directly contradicts plan §3.1 ("Do not place protocol-specific
implementation inside Gateway Core"). A plugin's configuration cannot be a
compiled struct in Core; it has to be schema-described data
(`config.schema.json` in the plan's §12 package layout) that Core stores and
validates generically. That is a genuine redesign of `ConfigManager`'s public
shape, and the web Configuration page is generated against those typed
sections, so the UI moves with it.

Persistence is already generic enough to help: config is stored as one SQLite
row per section, `config(section, data)`, with `data` a JSON blob. Adding a
plugin's section needs no schema migration — only the C++ struct and its
accessor have to go.

## 10. Dependencies

vcpkg manifest: `crow`, `asio`, `nlohmann-json`, `lua`, `sqlite3`, `curl`,
`openssl`, `libsodium`, `amqpcpp`. Pinned baseline
`39344dff01c5a5a0134caf2624cdd492f05d30ea`. Note vcpkg currently vendors **Lua
5.5**, not 5.4.

Two dependencies are optional and already use the pattern the plugin system
needs: `amqpcpp` and the ZK PullSDK are found QUIET, and when absent a
`_stub.cpp` is compiled instead, so every call fails with a clear message
rather than the API vanishing. `MqClient_stub.cpp` and `PullSdkClient_stub.cpp`
are the existing, working precedent for "feature not installed" — the same
problem as "plugin not installed."

`libsodium` is `REQUIRED` and already provides Ed25519 signing and SHA-256 for
OTA updates and Lua package signing. **Plugin signing (§14, Phase 9) can reuse
it directly**, along with the verification code in
`src/update/SignatureVerifier.cpp` and `src/lua_package/PackageKeys.cpp`.

## 11. What already exists that the plan can reuse

Worth stating explicitly, because it reduces the plan's scope materially:

| Plan requirement | Already built |
|---|---|
| §12 package format, §13 install workflow | OTA updates: `releases/` + `current` symlink, SHA-256, Ed25519 detached signatures, boot-trial rollback (`src/update/`) |
| §14 plugin signing | `SignatureVerifier`, `PackageKeys`, libsodium |
| §13 install/rollback for code artifacts | `src/lua_package/PackageManager.cpp` — build, sign, verify, deploy, roll back, `state.json` with active/previous |
| §10.1 in-process, absent-feature handling | the `_stub.cpp` pattern |
| §16 plugin Lua registration | `package.preload`, proven by `zk_controller` |
| §20 Test Tool reusing production drivers | already the case |
| §26 cross-compilation | `scripts/build-ubuntu.sh` containerised build, static libstdc++/libgcc |

The Lua packaging subsystem is the closest structural analogue to what the
plugin system needs, and its state model (`active`, `previous`, history, refusal
to delete either) should be copied rather than reinvented.

---

## 12. Consequences for the plan

Five findings change how the migration should be sequenced.

**1. Transport API first, and it is cheap.** `TcpSocket` is already a working
shared transport with five consumers and no hard-coded paths; only the
indirection is missing. Introducing a transport interface and constructing it
from outside is a small, mechanical, independently testable change — and it is
what unlocks mock-transport testing for everything else. Phase 3 is correctly
ordered and lower risk than it looks.

**2. `Bind`/`SetModules` are the real chokepoint.** Both take fixed positional
lists of concrete module pointers, and they are why adding a protocol touches
Core in four places. Replacing them with a registry that plugins register into
delivers most of the plan's stated benefit, and it can be done before any
driver becomes a `.so`. This should be pulled *earlier* than Phase 5.

**3. Configuration is a bigger job than the plan implies.** Six compiled,
protocol-specific config structs with hand-written JSON mapping, plus a web UI
generated against them, all have to become schema-described plugin data. The
plan mentions `config.schema.json` in passing (§12) and never assigns it a
phase. It needs one.

**4. Crow cannot route to plugins.** The routing table is fixed at start-up.
Plugin REST endpoints need a dispatching prefix like the existing
`/api/app/<path>`, or plugins must load before listening. The plan assumes
plugin REST endpoints are free; they are not.

**5. Threading is the real 256 MB risk, and it is not a packaging problem.**
Thread-per-module plus a 16-thread Crow pool does not fit the §27 minimum
target, and no amount of ABI work changes that. Wrapping the current drivers as
plugins would ship the thread count intact. If the 256 MB target is real, an
event-loop consolidation has to be scheduled as its own phase, and the
Transport API should be designed **non-blocking from the start** — because a
blocking transport interface forces a thread per driver forever, and that
decision gets frozen into the ABI. This is the one place where getting Phase 3
wrong is expensive to undo.

### Recommended sequencing change

The plan's P0→P8 order is sound except for two moves:

- Pull "replace `Bind`/`SetModules` with a registry" forward, into P1, as an
  internal refactor with no ABI implications. It is the highest
  benefit-to-risk item in the whole migration.
- Insert a configuration-schema phase between the Plugin Manager (P1) and
  driver migration (P3). Migrating a driver before its configuration can travel
  with it means migrating it twice.

### Repository layout

New SDK and plugin code lands in `sdk/` and `plugins/` alongside the current
tree. `src/` and `include/hsf/` stay where they are for now. The plan's §30
layout moves nearly every file; doing that before plugins work would produce
one enormous mechanical diff touching every include path and the CMake build,
with no functional gain and a broken bisect history through the middle of the
migration. The move is cheap to do later, once the destination directories have
earned their contents.
