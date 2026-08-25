# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A cross-platform (Linux/Windows/macOS) C++17 "Smart Gateway" backend that
bridges an external REST server with industrial hardware — a USB Serial
Citizen ID card reader, a Modbus TCP PLC, and a TCP RFID reader — plus a
Bootstrap/Chart.js web dashboard and an embedded Lua scripting engine for
business logic. The original spec is `request/Request/init_overview.md`;
`README.md` covers build instructions and project layout in more detail.

## Build / run

Requires vcpkg (manifest mode; `vcpkg.json` lists `curl`, `nlohmann-json`,
`lua`, `asio`, `crow`, `sqlite3`, `amqpcpp`):

```bash
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=<path-to-vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build .
./hsf_gateway ../config/config.json
```

There is no test suite yet (see the Testing checklist in
`request/Request/init_overview.md` for what's expected eventually).

To sanity-check a change without vcpkg (e.g. on macOS with Homebrew), install
`crow nlohmann-json asio lua pkg-config` via `brew` and compile the sources in
`src/` directly against those include paths (Homebrew doesn't ship
CMake/pkg-config files for `crow`/`asio`/`nlohmann-json` uniformly across
versions, so a raw `g++` invocation is more reliable there than trying to
coax CMake's `find_package` to see Homebrew's layout).

## Architecture

**Ownership and threading.** `src/main.cpp` constructs every module as a
plain stack object and wires them together with raw non-owning pointers —
there's no dependency-injection framework. Each hardware-facing module owns
its own background thread:

- `SerialPort` — read loop accumulates bytes into lines, invokes a single
  data callback per line. POSIX (termios) and Windows (Win32 `DCB`/`COMMTIMEOUTS`)
  backends are separate translation units (`SerialPort_posix.cpp` /
  `SerialPort_win.cpp`) selected in `CMakeLists.txt` by `WIN32`; the
  platform-independent open/close/read-loop logic lives once in
  `SerialPort.cpp` and calls into `PlatformOpen`/`PlatformRead`/etc.
- `RfidClient` — owns a reconnect loop against `TcpSocket` (a tiny
  BSD-socket/Winsock wrapper shared with the Lua `Tcp.*` bindings).
- `ModbusClient` — wraps libmodbus; not threaded itself, polled externally
  (see `main.cpp`'s `pollThread`, which also detects input-coil transitions
  and fires `OnPlcInputChanged` into Lua).
- `WebServer` — pimpl'd (`WebServer::Impl` in `WebServer.cpp`) to keep
  `crow.h` out of the public header; runs Crow's server loop plus a 1s
  broadcast loop on two background threads.
- `LuaEngine` — one instance is one script. The Lua VM is guarded by a single
  mutex; every native binding and every `DispatchEvent` call takes it, so a
  script is never entered concurrently from two C++ threads at once.
- `LuaRuntimeManager` — owns the set of running scripts, one `LuaEngine` and
  one thread each (`request/upgrade.md` sections 20-28), and is what the rest
  of the gateway talks to. It also owns the *single* callback slot on
  `SerialPort` and `ZkController`, fanning each event out to every live engine;
  installing those on an engine would let the last-constructed one silently
  take every card read. Its `mutex_` guards the runtime map only — interrupting
  a script, joining its thread and dispatching into a VM all happen with the
  lock released, so a stuck script can't freeze status reporting. Runtimes stay
  in the list in their terminal state (`STOPPED`/`ERROR`) until pruned; a
  runtime's thread is always moved out of the record and joined before the
  record can be destroyed, since destroying a joinable `std::thread` calls
  `std::terminate`.

Cross-cutting singletons (`Logger::Instance()`, `ConfigManager::Instance()`,
`RuntimeVariables::Instance()`) are the only globally-shared state; they are
internally thread-safe. Everything else is reached via the pointers wired up
in `main.cpp`.

**The Lua event model.** A script's top-level code runs once (on
`RunSource`/`RunFile`) to set up runtime variables and *define* handler
functions; the gateway then calls into those named globals as hardware
events occur — it does not run an ongoing script loop. The two wired-up
conventions are `OnCitizenCardRead(cardJson)` (fired from
`LuaRuntimeManager::Bind`'s serial data callback, after `CitizenIdParser` has
parsed the raw card text into JSON) and `OnPlcInputChanged(index, value)`
(fired from `main.cpp`'s poll thread). Every event goes to *every* running
script that defines the handler. See `config/scripts/main.lua` for the
convention. Native bindings are registered via the Lua registry
(`kEngineRegistryKey` in `LuaEngine.cpp`), not a global/static pointer, so the
trampoline functions in `LuaEngine::Lua_*` recover the owning `LuaEngine*`
per-call — which is also how a binding knows which runtime it is serving
(`RuntimeId()` tags that script's `ModbusRegistry` points and its structured
log rows).

**Two logs, deliberately separate.** `Logger` is the runtime/debug channel:
console, a rotating file, and an in-memory ring buffer the Log Viewer's
Runtime tab tails over the WebSocket. `LogStore` is the durable business
record: `Log.Write(type, data)` from Lua, validated against the type
declarations in `config/log_definitions.json` and stored in
`config/logs.db` as `log_definitions` / `log_entries` / `log_values`
(`request/upgrade.md` sections 7-16). The entry/value split is what lets a
script define a new log type with its own fields without creating a table per
type; the Structured Logs tab queries it with server-side filters and
pagination, never loading the table into the browser.

**Modbus TCP is hand-rolled, not libmodbus.** `ModbusClient` speaks the
Modbus TCP wire protocol (MBAP header + PDU) directly over `TcpSocket`
instead of linking libmodbus. This replaced an earlier libmodbus-based
implementation after discovering libmodbus's Windows TCP connect path
rejects real Windows `SOCKET` handles — it checks `ctx->s >= FD_SETSIZE`
(a POSIX-fd assumption; Windows `SOCKET`s are kernel handle values, not
small sequential integers, and are almost always >= the default
`FD_SETSIZE` of 64), so every Modbus TCP connect failed on Windows
regardless of whether the PLC was actually reachable. Don't reintroduce
libmodbus for TCP without accounting for that.

**RabbitMQ is AMQP-CPP over our own TcpSocket, not its TCP module.**
`MqClient` (`src/MqClient.cpp`) uses AMQP-CPP purely as a protocol codec — it
implements `AMQP::ConnectionHandler`, feeding socket bytes to
`Connection::parse()` and writing `onData()` back out over `TcpSocket` on its
own IO thread. That is the library's intended shape (it ships no I/O of its
own) and mirrors `qt-mq-lab`'s `MqConnection` over `QTcpSocket`; the source
spec is that repo's `RabbitMQ.md`, including the v1 message envelope
(`{"id","ts","v":1,"body"}`, `src/MqCodec.cpp`) and the topology defaults in
`MqConfig`. Two rules that are easy to break: AMQP-CPP's `Connection`/`Channel`
are **not** thread-safe, so `Publish()`/`Pause()` only queue work for the IO
thread and never touch them; and a rejected delivery is rejected *without*
requeue, because an undecodable message fails identically on redelivery and
would spin forever holding a prefetch slot. Unlike the Qt reference, this
client reconnects, and it acks a delivery once it is in the inbox (not when a
script reads it) so a script that only listens for `OnMqMessage` can't stall
the consumer at `prefetch` unacked messages. `find_package(amqpcpp)` is QUIET:
without the port, `MqClient_stub.cpp` is built instead and every `Mq.*` call
fails with a clear message rather than the API vanishing — same treatment as
the ZK PullSDK.

**Card Reader Client API.** An alternative card-input path alongside RFID
hardware: external readers (USB/TCP/desktop/mobile) `POST /api/card/input`
with a per-client `API-Key` header and `multipart/form-data` body
(`CardData`, `Position`) instead of the gateway talking to RFID hardware
directly. `CardClientManager` (own SQLite database, default
`config/clients.db`, sibling of `config.db`) manages those clients —
generate/enable/disable/delete, each with a random API-Key and optional
expiration — and authenticates incoming requests. A master
`ConfigManager`-held toggle (`card_api.enabled`, on the Configuration page)
gates `POST /api/card/input` itself (403 when off) without touching
`CardClientManager`'s stored clients; that page also has a "Quick API-Key"
shortcut that calls the same `POST /api/card/clients` the Card Clients page
uses, for the common single-reader case. Accepted cards land in
`CardCache`, a single-slot in-memory cache (not persisted; cleared on
`Card.Clear()` or overwritten by the next card), readable from Lua via
`Card.Available()`/`Card.Get()`/`Card.Clear()` (poll-based, per the spec's
example workflow) and also announced via the `OnCardReceived(uid)` event
handler (queued through `LuaEngine::QueueEvent`, same as `OnRfidCardRead`,
for scripts that prefer that style instead). See
`request/Request/cardInputRESTAPI.md` for the original spec and the
`Card Clients` web page (`card_clients.html`) for client management plus a
test harness that simulates an external reader without physical hardware.

**A production Lua package is a whole application, not one chunk.**
`src/lua_package/` compiles an app to bytecode with the gateway's OWN embedded
interpreter (never `luac` — bytecode is locked to the interpreter version and
pointer width, and vcpkg currently vendors Lua **5.5**, not 5.4), bundles every
module with it, encrypts with XChaCha20-Poly1305 and signs with Ed25519
(`docs/lua-packaging.md`). Two rules are easy to break. Module names are
computed relative to the SCRIPTS directory, not the application directory,
because that is what `package.path` resolves against — get it wrong and every
`require()` misses. And `LuaEngine::RunBundle` **empties `package.path`** after
installing the modules into `package.preload`: without that, a missing or
misnamed module silently falls through to the plaintext `.lua` next door, so
the package works on the build machine and fails on the first device that
lacks the sources. Both of those are written from experience — that exact bug
shipped through a green test run here, which is why
`scripts/package-lua-test.sh` moves the source tree away and restarts.

**API security is middleware, not per-handler checks.** The configuration app
is a `crow::App<SecurityMiddleware>`, not a `SimpleApp`, so
`src/security/SecurityMiddleware.h` runs body-size, rate-limit, authentication
and authorization checks on every request before any handler
(`request/AdvanceUpdate.md` Phase 1, `docs/security.md`). Which permission a
route needs lives in ONE ordered table — `RoutePolicies()` in
`src/security/Permissions.cpp` — and a path with no entry falls through to
"a session is required", so adding a route without classifying it fails closed.
Don't add `if (!authorized)` to a handler; add a row to the table. Accounts,
sessions, lockout counters and the audit trail are in `security.db`, a fifth
store beside `config.db`/`clients.db`/`logs.db`/`smartlocker.db`, because it is
the one file whose backup and disposal rules differ from the rest. libsodium is
`REQUIRED` in CMake rather than stubbed like amqpcpp and the PullSDK: those are
optional features, but with `auth.enabled` defaulting on, a build that cannot
hash a password is a build nobody can log into. Two routes are deliberately
outside the scheme — `/api/card/input` keeps its own API-Key (it is already
authenticated, and readers are deployed), and the locker UI on its own port is
unauthenticated unless `auth.protect_locker_ui` is set.

**Web layer.** `WebServer.cpp` serves the static frontend from `web/`
(paths resolved relative to `HSF_WEB_ROOT`, a compile definition pointing at
`web/` under the source tree — not installed/copied elsewhere at build time)
and exposes the REST + WebSocket API the frontend JS calls
(`web/js/*.js`). When returning a `crow::response` by value from a route
handler, do not also call `res.end()` inside a helper — that double-finalizes
the response and hangs the connection (this bit `ServeStatic` once; see the
comment there).

`WebServer::Impl` runs **two** Crow apps. The second one is the SmartLocker
floor plan (`web/locker/`), on its own port (`web.locker_ui_port`, off by
default) — a different audience from the configuration UI, so a kiosk or a
firewall can be pointed at exactly one of them. It reads `smartlocker.db`
through its own **read-only** `SqlDatabase` connection and pushes the whole
model over a WebSocket whenever it changes; a page that wants something
*changed* posts to `/api/locker/lockers/<id>/unlock` (or `/release`, or
`/api/locker/sync`), which is forwarded to the Lua application. The split is
deliberate: reading straight from SQLite keeps the display live while a script
is mid-door-timeout, and no web thread may ever write a locker row out from
under the state machine that owns the door.

**Lua-served HTTP routes.** Crow's routing table is built at start-up, so a
script cannot add a URL. Instead `/api/app/<path>` (one to three segments)
forwards to whichever running script registered that path with
`Http.Register`; `LuaRuntimeManager::DispatchHttp` finds it, `LuaEngine::
SubmitHttp` queues it, and the handler runs on the *script's own thread* at its
next `Sleep()` — the only thread allowed to enter that VM. The web thread waits
on a condition variable with a 5s timeout (504 after that), because a
polling-loop script holds its engine mutex for its whole run and a direct call
would block a Crow worker forever. Registered paths may contain `<segment>`
wildcards.

**SQL for Lua.** `SqlDatabase` (`src/SqlDatabase.cpp`) is a general-purpose
SQLite handle — distinct from the three purpose-built stores (`config.db`,
`logs.db`, `clients.db`), which own their schemas. It backs the `Db.*` bindings:
one connection per engine, WAL, parameters always bound, `Db.NULL` for SQL NULL
(a Lua `nil` inside a parameter array would end the array), `BEGIN IMMEDIATE`
transactions with a nesting counter. This is why `request/upgrade.md` section
6's "no SQL surface" rule now reads as historical: configuration is still
reached only through `Config.*`, but an application's *own* tables are its
business.

**ZK controller I/O has three shapes, and the SDK dictates all of them.**
`ControlDevice` drives door relays and auxiliary outputs (`ZkController::
OpenDoor`/`SetAuxOutput`, Lua `zk.openDoor`/`zk.auxOut`) — write-only, with a
hold time in *whole seconds*, so millisecond pulses are latch-then-release
(`PulseOutput`, which is also all `zk.beep` is: **the PullSDK has no beeper
command**, so an audible signal is whatever relay it is wired to). Door sensors
arrive only in RTLog status records (`ZkIoState::doors`, four packed bytes,
door 1 in the *low* byte) and auxiliary inputs only as RTLog events 220/221 —
there is no read-input call at all, which is why `zk.inputState()` answers `nil`
rather than `false` for an input nobody has triggered. See
`ZKTecoProtocol/docs/sdk-protocol-reference.md` sections 3.5 and 4.

**Two ZK backends behind one interface.** `ZkController` reaches a panel
either through ZKTeco's PullSDK DLL (`PullSdkClient`) or by speaking the
panel's own C3/InBio TCP protocol (`C3Client` over `C3Codec`), chosen by
`zk.backend`: `"auto"` (PullSDK where its 32-bit Windows DLL can load, C3
everywhere else), `"pullsdk"`, or `"c3"`. C3 exists because PullSDK is
Windows-x86-only, so before it every `zk.*` call on a Linux gateway returned
`NotSupportedOnThisPlatform` — the doors simply could not be opened.
`C3Client` deliberately copies `PullSdkClient`'s six-method surface rather
than exposing a nicer C3-native API, so `ZkController`'s heartbeat/reconnect,
the RTLog poll loop, `RTLogParser`, every `zk.*` binding and the Test Tool
pages work unchanged; the only genuinely new code is the wire protocol. The
one real translation is `C3Client::GetRTLog`, which formats C3's 16-byte
binary records into the PullSDK CSV `RTLogParser` already reads. Two
consequences worth knowing: C3 has no SETPARAM, so `zk.setParam` fails
honestly on that backend rather than silently doing nothing; and every error
path goes through `BackendErrorText()`, because reporting
`PullSdkClient::LastError()` unconditionally — which it once did — blamed a
DLL that was never loaded for failures the C3 client had already explained.
`C3Codec` is a port of `qt-app-base`'s `c3_codec`, with `QRegularExpression`
key/value scanning replaced by a hand-rolled scanner.

**Config.** `ConfigManager` is the single source of truth for `system`, `web`,
`rest`, `serial`, `serial2`, `modbus`, `rfid`, `zk`, `lua`, `logging` and
`card_api` settings, loaded from and saved back to one SQLite database file (default
`config/config.db`; requires the `sqlite3` vcpkg dependency) — one row per
section in a `config(section, data)` table, `data` being that section's JSON
blob. The web Configuration page reads and PATCHes `/api/config`, which calls
`ConfigManager::ApplyJson` then `Save()`; the JSON shape at that REST boundary
is unchanged, only the on-disk persistence format is SQLite instead of a raw
JSON file. Lua reaches the same values through `Config.Get`/`Set`/`Exists`/
`GetCategory`, addressed as `"<section>.<key>"` against the document
`ToJson()` produces — deliberately not a SQL surface
(`request/upgrade.md` section 6).

`config/scripts/config.lua` is a different thing with the same name: the
script-level table of workflow constants that scripts `require`. It captures
the native global as `local Gateway = Config` on its first line, before its own
`local Config = {}` shadows it, and reads installation-level settings through
`Gateway.Get` with the pre-migration literal as a fallback. The split it draws
is worth preserving: addresses, credentials and the machine id come from
`config.db`; mechanical timings, retry limits and *which coil each terminal is
wired to* stay literal there. Notably the output coils are NOT derived from
`modbus.output_coil_start` — on this machine scan and out share 1280 while
collect is at 1282, so no base address describes them, and `*_coil_start` means
"the window the gateway itself polls" rather than "where the wiring is". On first `Load()`
against a path with no existing database, if a sibling `<name>.json` file
is found it's imported once to seed the new database (upgrade path from the
old format) — see `MigrateLegacyJsonIfEmpty` in `ConfigManager.cpp`.
