# eMaster Core

A cross-platform (Linux / Windows / macOS) Smart Gateway backend that bridges an
external REST server with industrial hardware — a USB Serial Citizen ID card
reader, a Modbus TCP PLC, and a TCP RFID reader — and exposes a web
monitoring/configuration dashboard plus an embedded Lua scripting engine for
business logic.

See `request/Request/init_overview.md` for the original specification this
project implements.

## Technology Stack

- **Backend:** C++17, [Crow](https://github.com/CrowCpp/Crow) (HTTP + WebSocket
  server), [libcurl](https://curl.se/libcurl/) (REST client),
  [nlohmann/json](https://github.com/nlohmann/json), a hand-rolled Modbus TCP
  client (MBAP header + PDU over the project's own `TcpSocket`, not
  libmodbus — see below), [Lua](https://www.lua.org/) (embedded scripting; vcpkg currently vendors 5.5 — production packages are bytecode, which is version-locked),
  standalone Asio, termios / Win32 serial I/O, `std::thread`.
- **Frontend:** HTML5, CSS3, ES6 (no build step), Bootstrap 5, Chart.js, WebSocket.
- **Build system:** CMake, dependencies managed via [vcpkg](https://vcpkg.io) manifest mode.

## Documentation

| | |
|---|---|
| [docs/API.md](docs/API.md) | API reference — the contract the registration server implements, the gateway's own REST/WebSocket API, the Card Reader Client API |
| [docs/installation.md](docs/installation.md) | Installing a release package, running as a service, upgrading |
| [docs/building.md](docs/building.md) | Building from source, and the `HSF_ENABLE_ZK` option |
| [docs/troubleshooting.md](docs/troubleshooting.md) | Symptom-first fixes (start here when something is wrong) |
| [docs/release-checklist.md](docs/release-checklist.md) | Cutting a release |
| [docs/ota-update.md](docs/ota-update.md) | Over-the-air updates — signing keys, manifest format, the managed install layout, rollback |
| [docs/security.md](docs/security.md) | API authentication, roles and permissions, lockout and rate limits, known gaps, recovering a locked-out gateway |
| [docs/lua-packaging.md](docs/lua-packaging.md) | Production Lua packages — compile to bytecode, encrypt, sign, deploy and roll back |
| [docs/deployment-ubuntu.md](docs/deployment-ubuntu.md) | Ubuntu build, remote deployment to a target server, health checks and rollback |
| [CHANGELOG.md](CHANGELOG.md) | What changed, and current known issues |
| `/lua_docs.html` | Lua API reference, served by the running gateway |

## Building

### Prerequisites

- CMake 3.20+
- A C++17 compiler (GCC, Clang, or MSVC)
- [vcpkg](https://github.com/microsoft/vcpkg), cloned and bootstrapped

```bash
git clone https://github.com/microsoft/vcpkg
./vcpkg/bootstrap-vcpkg.sh   # or bootstrap-vcpkg.bat on Windows
```

### Platform support, and why Windows builds are 32-bit

ZKTeco access-controller support links the PullSDK (`plcommpro.dll`)
in-process, and that DLL is a **32-bit Windows binary**. The `HSF_ENABLE_ZK`
CMake option therefore defaults ON only for Windows x86 — where it forces the
whole gateway to x86 — and OFF everywhere else.

| Target | `HSF_ENABLE_ZK` | ZK card reading |
|---|---|---|
| Windows x86 | ON (default) | yes |
| Windows x64 | OFF (default) | no |
| Linux, macOS | OFF (default) | no |

With it off, the `zk_controller` Lua module is still present and every
function still exists; calls just fail with `NotSupportedOnThisPlatform`. All
other hardware — Modbus PLC, serial card reader, LED display, TCP RFID, the
Card Reader Client API — works on every platform.

### Windows

```bat
build.bat
```

Configures `build-win/` as Win32/x86 with the `x86-windows` triplet and builds
Release.

### Linux / macOS

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build -j"$(nproc)"
./build/hsf_gateway
```

`vcpkg.json` declares the manifest dependencies (`curl`, `nlohmann-json`,
`lua`, `asio`, `crow`, `sqlite3`); vcpkg installs them automatically on first
configure when the toolchain file above is passed.

The gateway finds `web/` and `config/` relative to its own executable, so it
runs straight from the build directory and an extracted release package is
self-contained. Pass a config path explicitly to override:
`./hsf_gateway /etc/hsf-gateway/config.db`.

```bash
./build/hsf_gateway --version
```

### Packaging a release

```powershell
.\scripts\package.ps1 -Arch x86     # Windows
```

```bash
./scripts/package.sh                # Linux / macOS
```

Each wipes and reconfigures a clean build directory, stages the package,
refuses to include any `*.db` or `config.json`, and writes a SHA-256 checksum
to `dist/SHA256SUMS`. See [docs/release-checklist.md](docs/release-checklist.md).

Modbus TCP is implemented directly in `ModbusClient` (MBAP header + PDU
framing over `TcpSocket`) rather than via libmodbus, so there's no separate
Modbus package to install — see the architecture note in `CLAUDE.md` for why.

### Over-the-air updates

Pushing a `v*` tag builds every platform, signs a `version.json` manifest and
creates a **draft** GitHub Release. A gateway installed with
`scripts/install-ota.sh` polls that manifest, shows a popup when a newer build
exists, and — on a click, never on its own — downloads it, checks its SHA-256
and signature, unpacks it into `<root>/releases/<version>/`, repoints
`<root>/current` and restarts. A release that fails to start is rolled back
automatically.

Off by default; enable it under **Software Update** on the Configuration page
once the release server URL is set. Full setup, including generating the
signing key pair, is in [docs/ota-update.md](docs/ota-update.md).

Once running, open **http://localhost:8080** (or whatever `web.port` is set
to in the config file).

## Configuration

All runtime configuration lives in a single SQLite database file (default:
`config/config.db`), editable through the web Configuration page
(`GET`/`POST /api/config`), which reads/writes it as a JSON document over
that REST endpoint: `web`, `rest`, `serial`, `modbus`, `rfid`, `lua`,
`update`. Each
section is stored as one row (`section`, `data`) in the database's `config`
table, `data` being that section's JSON blob.

Upgrading from a pre-SQLite install: if `config.db` doesn't exist yet but a
sibling `config.json` does, it's imported automatically the first time the
gateway starts against the new path.

## Project Layout

```
include/hsf/        Public headers for every module
src/                 Implementation, one .cpp per module
  SerialPort.cpp       shared read-loop/lifecycle logic
  SerialPort_posix.cpp termios backend (Linux/macOS)
  SerialPort_win.cpp    Win32 API backend (Windows)
web/                 Static frontend served by WebServer
  index.html           Dashboard
  config.html          Configuration page
  lua_editor.html       Online Lua editor (run/stop/restart/save/load/validate)
  lua_docs.html          Lua API reference
  logs.html               Log viewer
  card_clients.html        Card reader client management + test harness
config/
  config.db            Runtime configuration (SQLite; created on first run)
  clients.db           Card reader client API-Keys (SQLite; created on first run)
  logs.db              Structured business logs from Log.Write (SQLite; created on first run)
  log_definitions.json  Log types and their fields, validated on every Log.Write
  scripts/main.lua      Default business-logic script
```

## Architecture

```
                +-----------------------+
                |     External Server   |
                |      REST API         |
                +----------+------------+
                           ^
                     HTTP REST API / API-Key
                           |
+--------------------------------------------------------+
|                Smart Gateway Backend                   |
|  RestClient · SerialPort · ModbusClient · RfidClient    |
|  LuaRuntimeManager (LuaEngine per script)               |
|  ConfigManager · WebServer · SystemMonitor              |
|  RuntimeVariables · Logger · LogStore                   |
+--------------------------------------------------------+
                           |
          ---------------------------------------
          |                 |                   |
      USB Serial       PLC Modbus TCP      RFID Reader
```

A Lua script's top-level code runs once at startup to initialize runtime
variables and define handler functions (`OnCitizenCardRead`,
`OnPlcInputChanged`, `OnCardReceived`, ...); the gateway then dispatches
events into those handlers as hardware activity occurs (a card is read, a
PLC input coil changes, an external card reader client posts a UID). See
`config/scripts/main.lua` and the in-app Lua API Documentation page for the
full native API surface (`Rest.*`, `Serial.*`, `Tcp.*`, `Modbus.*`, `Card.*`,
`Log.*`, `Config.*`, `SetVariable`/`GetVariable`).

Several scripts can run at once, each in its own `LuaEngine` (its own
`lua_State`, its own thread, its own CPU/RAM figures in the dashboard's
System Info tab). Tick them in the Lua Editor's script tree and press **Run
Selected**. Every event is delivered to every running script that defines the
handler; a script that faults is marked `ERROR` and stopped on its own,
leaving the others running.
