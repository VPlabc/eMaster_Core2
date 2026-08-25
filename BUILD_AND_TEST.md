# Build & Run Test Notes

This is a working log of how the backend was actually built and exercised
during development, kept separate from `README.md` (which documents the
intended/official vcpkg-based build). There is no automated test suite yet
(see the Testing checklist in `request/Request/init_overview.md`), so until
one exists, this doc is the reproducible manual procedure.

## 1. Official build (vcpkg + CMake)

This is the supported path for Linux/Windows/macOS and is what
`CMakeLists.txt` / `vcpkg.json` are written for. See `README.md` for full
details:

```bash
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=<path-to-vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build .
./hsf_gateway ../config/config.db
```

This has **not** been exercised end-to-end in this environment because
vcpkg isn't installed here and bootstrapping it (clone + build seven ports
from source) is expensive. Treat it as reviewed-but-unverified until someone
runs it on a machine with vcpkg available.

## 1a. Windows build — verified end-to-end (2026-08-04)

Ran the official vcpkg+CMake path through to a working `hsf_gateway.exe` on
this exact Windows machine via `build.bat` (repo root), which automates the
steps below. Three real bugs surfaced along the way (see §4); all fixed and
re-verified.

- `cmake` is **not** on `PATH`, but Visual Studio Build Tools 2026 ("VS 18",
  MSVC v143-class x64 toolset, confirmed present via
  `vswhere.exe -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64`)
  ships its own CMake 4.3.1 and Ninja under
  `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\`
  (`CMake\bin\cmake.exe`, `Ninja\ninja.exe`). That bundled `cmake.exe` lists
  `Visual Studio 18 2026` as a generator, so no separate CMake install is
  required.
- `build\vcpkg\` in this repo is already a cloned vcpkg checkout, but **not
  bootstrapped** — there's no `vcpkg.exe` there yet; `bootstrap-vcpkg.bat`
  needs to run once first.
- The repo root already has a **stale, foreign** `CMakeCache.txt` / `Makefile`
  / `compile_commands.json` from a prior **Unix Makefiles** configure
  (`CMAKE_CXX_COMPILER=/usr/bin/c++` — clearly copied over from a Linux/macOS
  session, not generated here). Don't `cmake --build .` from the repo root as
  it stands; configure into a fresh directory instead so CMake can't try to
  reuse that cache against MSVC.

### Verified Windows build commands

```powershell
# 1. Bootstrap vcpkg (one-time; build\vcpkg is already cloned but not bootstrapped)
cd build\vcpkg
.\bootstrap-vcpkg.bat
cd ..\..

# 2. Configure into a fresh dir, using VS Build Tools' bundled cmake
#    (nothing named `cmake` is on PATH on this machine)
$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -S . -B build-win -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$PWD\build\vcpkg\scripts\buildsystems\vcpkg.cmake"

# 3. Build
& $cmake --build build-win --config Release

# 4. Run
.\build-win\Release\hsf_gateway.exe config\config.db
```

The Visual Studio generator is used deliberately over Ninja: MSVC is located
from the generated `.sln`/`.vcxproj` at build time, so this works from a
plain PowerShell prompt with no need to first open a "Developer Command
Prompt" or run `vcvarsall.bat`. (Ninja is bundled alongside CMake here too,
for a faster single-config build, but that path needs `cl.exe` on `PATH`
first — e.g. `& "C:\...\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64` —
before configuring with `-G Ninja`.)

**Result:** first configure compiled `curl`, `nlohmann-json`, `libmodbus`,
`lua`, `asio`, `crow`, `sqlite3` from source (~15 min on a clean vcpkg, all
cached afterward via vcpkg's binary cache — a re-configure with no source
changes takes ~3s). `hsf_gateway.exe` built clean and actually ran:

```
[INFO] [System] eMaster Gateway starting
[INFO] [System] ConfigManager: migrated legacy .../config/config.json into .../config/config.db
[WARNING] [Serial] Serial port not available at startup; will need to be opened later
[WARNING] [Modbus] PLC not reachable at startup; will not auto-retry
[INFO] [System] Web server listening on 0.0.0.0:8080
```

(Serial/Modbus warnings are expected — no hardware attached, same as the
Homebrew smoke-test in §2. Lua didn't start because this machine's
`config.json` had an empty `script_path` — also expected, not a bug.)

Verified beyond just startup: `GET /api/config` returned the exact same
values as the pre-migration `config.json` (Modbus/RFID IPs, the REST API
key, serial port, etc. — nothing lost in the migration); `POST /api/config`
changing `web.bind_address`, followed by killing and relaunching the
process, came back with the *new* value — confirming `Save()`/`Load()`
actually round-trip through `config.db` and aren't just mutating in-memory
state.

**2026-08-04 update:** `ConfigManager` was switched from a plain
`config.json` file to a SQLite database (`config/config.db`) for
persistence — see the new `sqlite3` vcpkg dependency and `ConfigManager.cpp`.
`build.bat` (repo root) automates the bootstrap+configure+build sequence
above for one-command builds on this or a similarly-provisioned Windows
machine.

## 2. Local smoke-test build (Homebrew, no vcpkg)

This is what was actually used to verify the code compiles, links, and
behaves correctly end-to-end. It bypasses `CMakeLists.txt` entirely (Homebrew
doesn't ship CMake/pkg-config files for `crow`, `asio`, or a matching
`unofficial-libmodbus` target the way vcpkg does, so pointing CMake's
`find_package` at Homebrew's layout isn't worth fighting) and instead
compiles all translation units directly with `g++`.

### One-time setup

```bash
brew install crow libmodbus nlohmann-json asio lua pkg-config
```

(macOS ships its own `libcurl` headers, so no separate `curl` install is
needed. Homebrew's `lua` formula currently installs 5.5, not the spec's 5.4
— the Lua C API used here (`lua_geti`/`lua_seti`/`lua_isinteger`, etc.) is
unchanged between 5.3–5.5, so this doesn't affect correctness, only which
version is actually running locally.)

### Compile + link

```bash
cd /Users/mac/Work/Win/eMaster_Core

CROW_INC=/opt/homebrew/opt/crow/include
JSON_INC=/opt/homebrew/opt/nlohmann-json/include
ASIO_INC=/opt/homebrew/opt/asio/include
LUA_INC=/opt/homebrew/opt/lua/include/lua5.5
MODBUS_INC=/opt/homebrew/opt/libmodbus/include
LUA_LIB=/opt/homebrew/opt/lua/lib
MODBUS_LIB=/opt/homebrew/opt/libmodbus/lib

g++ -std=c++17 -O0 -Wall -Wextra \
  -Iinclude -I"$CROW_INC" -I"$JSON_INC" -I"$ASIO_INC" -I"$LUA_INC" -I"$MODBUS_INC" \
  -DHSF_WEB_ROOT="\"$(pwd)/web\"" -DHSF_DEFAULT_CONFIG_PATH="\"$(pwd)/config/config.json\"" \
  src/main.cpp src/Logger.cpp src/ConfigManager.cpp src/SystemMonitor.cpp src/RestClient.cpp \
  src/SerialPort.cpp src/SerialPort_posix.cpp src/ModbusClient.cpp src/TcpSocket.cpp src/RfidClient.cpp \
  src/RuntimeVariables.cpp src/CitizenIdParser.cpp src/LuaEngine.cpp src/WebServer.cpp \
  -L"$LUA_LIB" -L"$MODBUS_LIB" -llua5.5 -lmodbus -lcurl -lpthread \
  -o /tmp/hsf_gateway
```

Swap `src/SerialPort_posix.cpp` for `src/SerialPort_win.cpp` (and drop the
`-llua5.5 -lmodbus -lcurl -lpthread` POSIX libs for their Windows
equivalents) if smoke-testing on Windows without vcpkg — untested, since
this environment is macOS.

### Run it

```bash
/tmp/hsf_gateway config/config.json
```

Expect graceful warnings (not crashes) when hardware isn't attached:

```
[ERROR] [Serial] Failed to open serial port /dev/ttyUSB0
[WARNING] [Serial] Serial port not available at startup; will need to be opened later
[ERROR] [Modbus] Modbus connect failed: Operation timed out
[WARNING] [Modbus] PLC not reachable at startup; will not auto-retry
[INFO] [Lua] main.lua started
[INFO] [Lua] Script started
[INFO] [System] Web server listening on 0.0.0.0:8080
```

Then open **http://localhost:8080** in a browser, or run the manual checks
below.

## 3. Manual verification checklist

Everything in this section was run against the Homebrew build above and
passed.

### Static frontend

```bash
curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8080/                 # 200
curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8080/css/app.css      # 200
curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8080/js/dashboard.js  # 200
curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8080/lua_docs.html    # 200
curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8080/nonexistent.html # 404
```

### REST API

```bash
curl -s http://127.0.0.1:8080/api/status
curl -s http://127.0.0.1:8080/api/config
curl -s http://127.0.0.1:8080/api/variables
curl -s "http://127.0.0.1:8080/api/logs?category=Lua&limit=10"
```

### Configuration page: scan/test buttons

The Configuration page's Serial Port section has "Scan" (populates the port
name `<select>` with detected devices) and "Try Open" buttons, and the
REST/Modbus/RFID sections each have a connectivity test button. They're
backed by:

```bash
# Serial port scan (globs /dev/ttyUSB*, /dev/cu.*, etc. on POSIX; reads
# HKEY_LOCAL_MACHINE\HARDWARE\DEVICEMAP\SERIALCOMM on Windows)
curl -s http://127.0.0.1:8080/api/serial/ports

# Serial: one-shot open/close against candidate port/baudrate/data_bits/
# stop_bits/parity, using a throwaway SerialPort instance so it never
# disturbs the gateway's own long-lived connection
curl -s -X POST http://127.0.0.1:8080/api/serial/test \
  -H "Content-Type: application/json" -d '{"port":"/dev/ttyUSB0","baudrate":115200,"data_bits":8,"stop_bits":1,"parity":"N"}'

# REST: one-shot GET against candidate url/api_key/timeout/ssl_enable,
# without touching the live RestClient's saved config
curl -s -X POST http://127.0.0.1:8080/api/rest/test \
  -H "Content-Type: application/json" -d '{"url":"https://example.com","timeout_ms":3000}'

# Modbus: one-shot connect/disconnect against candidate ip/port
curl -s -X POST http://127.0.0.1:8080/api/modbus/test \
  -H "Content-Type: application/json" -d '{"ip":"192.168.1.10","port":502}'

# RFID: one-shot connect + short read window against candidate ip/port/timeout_ms
curl -s -X POST http://127.0.0.1:8080/api/rfid/test \
  -H "Content-Type: application/json" -d '{"ip":"192.168.1.20","port":5000,"timeout_ms":2000}'
```

All five were verified against both a reachable/valid and an unreachable/
invalid target (a real Python `socket` listener for the RFID case, sending
fake card data; a real detected `/dev/cu.*` device node and a nonexistent
one for the serial case) and returned the expected `ok`/`connected` values
in each case — see the bug entry below for one case that initially didn't.

No browser was available to click these buttons directly in this
environment (headless background session), so verification replayed the
exact request bodies `config.js` sends via `curl` and confirmed the
response shapes match what the JS expects.

### Lua editor round-trip

```bash
# Syntax error is reported, ok:false
curl -s -X POST http://127.0.0.1:8080/api/lua/validate \
  -H "Content-Type: application/json" -d '{"code":"local x = "}'

# Valid script runs, SetVariable shows up in /api/variables afterward
curl -s -X POST http://127.0.0.1:8080/api/lua/run \
  -H "Content-Type: application/json" \
  -d '{"code":"SetVariable(\"foo\", 42)\nLog.Info(\"ran from test\")"}'
curl -s http://127.0.0.1:8080/api/variables   # should include "foo":42

curl -s -X POST http://127.0.0.1:8080/api/lua/stop
curl -s http://127.0.0.1:8080/api/status      # lua_running should now be false
```

### Citizen ID card parser

Standalone unit check against the exact example string from
`request/Request/init_overview.md`:

```bash
cat > /tmp/test_parser.cpp << 'EOF'
#include "hsf/CitizenIdParser.h"
#include <iostream>
int main() {
  std::string raw = "IDVNM0123456789012345678901<<59505041M01234567VNM<<<<<<<<<<<8NGUYEN<<VAN<SANG<<<<<<<<<<<<<<";
  auto parsed = hsf::CitizenIdParser::Parse(raw);
  if (!parsed) { std::cout << "PARSE FAILED\n"; return 1; }
  std::cout << hsf::CitizenIdParser::ToJson(*parsed).dump(2) << std::endl;
}
EOF
g++ -std=c++17 -Iinclude -I/opt/homebrew/opt/nlohmann-json/include \
  /tmp/test_parser.cpp src/CitizenIdParser.cpp -o /tmp/test_parser && /tmp/test_parser
```

Expected output matches the spec's example JSON exactly (`serial_number`,
`citizen_id`, `birth_date`, `gender`, `country`, `last_name`,
`middle_name`, `first_name`, `raw_text`).

Note: the spec's prose says the "code" field is 7 digits, but its own
example string has 8. The parser's regex matches the example (a
variable-length digit run) rather than the prose count, since that's the
only self-consistent reading.

### WebSocket realtime broadcast

Requires `pip install websockets`:

```python
import asyncio, websockets, json

async def main():
    async with websockets.connect("ws://127.0.0.1:8080/ws") as ws:
        msg = await asyncio.wait_for(ws.recv(), timeout=5)
        data = json.loads(msg)
        print("type:", data.get("type"))
        print("status keys:", sorted(data.get("status", {}).keys()))
        print("variables:", data.get("variables"))

asyncio.run(main())
```

Expect `type: status`, a `status` object with the 13 `SystemMonitor`/module
fields (`cpu_percent`, `ram_used_mb`, `plc_connected`, `lua_running`, ...),
and a `variables` object mirroring `/api/variables`.

## 4. Bugs found by this process (fixed)

- **`WebServer::Impl::ServeStatic` hung every static-file request.** It
  built a `crow::response` and called `res.end()` internally, but the
  calling route handler *also* returned that response by value — which is
  how Crow finalizes a synchronous handler. Finalizing twice hung the
  connection instead of erroring, so it only showed up as `curl` timing out
  on `GET /`, not as a crash or an obvious log line. Fixed by having
  `ServeStatic` only set `res.code`/`res.body`/headers and letting the route
  handler's `return res;` finalize it once. If you add new static-serving
  helpers, don't call `res.end()` from inside them.

- **`ld: library not found for -lmodbus`** when CMake picked the pkg-config
  branch for libmodbus (e.g. Homebrew, or any Linux distro with a
  `libmodbus.pc`). Root cause: `pkg_check_modules(MODBUS QUIET libmodbus)`
  only returns bare library names in `MODBUS_LIBRARIES` (e.g. `modbus`, not
  `-lmodbus -L/path/to/lib`) — the matching search path lands in
  `MODBUS_LIBRARY_DIRS`/`MODBUS_LDFLAGS`, which `CMakeLists.txt` never
  passed to the linker, so `-lmodbus` was emitted with no `-L` and the
  linker only found it if `/lib`'s install prefix happened to already be a
  default search path (it isn't on Homebrew's `/opt/homebrew`). Fixed by
  switching to `pkg_check_modules(MODBUS QUIET IMPORTED_TARGET libmodbus)`
  and linking the resulting `PkgConfig::MODBUS` imported target instead of
  the raw `MODBUS_LIBRARIES` variable — that target carries the correct
  `-L`/rpath automatically. Verified against Homebrew's actual
  `libmodbus.pc` in an isolated repro (old approach failed to link, new one
  built and linked cleanly).

  This also surfaced a second, related mismatch: pkg-config's `--cflags`
  for libmodbus points straight at `<prefix>/include/modbus` (matching
  libmodbus's own upstream `#include <modbus.h>` convention), while this
  project's source uses `#include <modbus/modbus.h>` (matching vcpkg's
  layout, where the imported target's include dir is the parent). The
  pkg-config branch in `CMakeLists.txt` now also adds each entry in
  `MODBUS_INCLUDE_DIRS`'s *parent* directory via `target_include_directories`
  so the same `#include <modbus/modbus.h>` works under both.

- **`/api/rfid/test` reported `connected:true` against a port nothing was
  listening on.** Root cause was in `TcpSocket::Connect`, not the new route:
  its non-blocking connect used `select()` on the write set to detect
  completion, but a *refused* connection also makes the socket writable —
  `select()` returning `>0` only means connect() finished, not that it
  succeeded. Fixed by reading back `SO_ERROR` via `getsockopt` after
  `select()` and only treating the connection as successful if it's zero.
  Verified with a real Python `socket` listener: refused port now correctly
  returns `connected:false`, and a listener sending data now correctly
  returns `connected:true` with the data. This also fixes the same
  latent issue in the production `RfidClient` reconnect loop and the Lua
  `Tcp.Connect` binding, both of which share `TcpSocket`.

- **A script with no I/O yield point (e.g. `while true do end`) could hang
  the entire gateway, permanently, with no way to recover short of killing
  the process.** Surfaced by a real user script:
  `Serial.Open(); while true do local data = Serial.Read() ... end` (no
  `Sleep`, so also spinning at ~100% CPU). Three compounding problems, all
  fixed together:
  1. **`Sleep(ms)` didn't exist** — added as a global Lua binding
     (`LuaEngine::Lua_Sleep`), documented on the Lua API Docs page.
  2. **`Stop()`/`Restart()` couldn't actually interrupt a running script.**
     `RunSource` holds `mutex_` for the entire top-level execution, and
     `Stop()`/`Restart()` needed that same lock — for a script that never
     returns, they'd hang right along with it, with no way to recover the
     engine at all. Fixed with a debug count-hook
     (`lua_sethook(L_, DebugHook, LUA_MASKCOUNT, 1000)`) that checks an
     `interruptRequested_` atomic and calls `luaL_error` to unwind the
     script; `Stop()`/`Restart()`/`RunSource()` all set that flag *before*
     attempting the lock so a stuck script gets interrupted instead of
     blocking the caller forever. Also had to make `Sleep()` itself check
     the flag between small (20ms) sleep increments — the debug hook only
     fires between Lua *bytecode* instructions, so it can never preempt a
     single long call into a blocking C function; for a script whose real
     wall-clock time is mostly spent inside `Sleep`, the count-hook alone
     left `Stop()` taking as long as the loop's full sleep duration (or
     worse — see the `RunSource` note below).
  3. **The gateway's startup script ran synchronously before the web
     server started.** If it never returned, the dashboard/API/Lua Editor
     were completely unreachable, so there was no way to even click Stop.
     Fixed by running the initial `lua.RunFile()` on its own thread in
     `main.cpp`, joined at shutdown (after `lua.Stop()`, which is what
     actually unsticks it if still running).

  Verified end-to-end against the real reported script: dashboard reachable
  in ~50ms even with the infinite loop running, `lua_running` correctly
  `true` while looping (see next entry), and `Stop()` returning in ~30-45ms
  instead of hanging.

- **`lua_running` reported `false` for a script that was genuinely still
  executing.** It was only set `true` *after* the top-level `pcall`
  returned — for a script that intentionally never returns (a polling loop,
  as opposed to one that just defines handlers), that moment never comes,
  so the dashboard showed it as stopped the entire time it was actually
  running. Fixed by setting `running_` before the `pcall` instead of after
  (and resetting it in the pcall-failure branch, which previously didn't
  need to touch it since it was already false at that point).

- **Starting a different script while an infinite-loop script was already
  running just hung forever** (`RunFile`/`RunSource`, and therefore
  `/api/lua/run` and the new per-file `/api/lua/scripts/<name>/run`) —
  found while testing the script manager below: running a second script
  needs to replace whatever's currently active, but `RunSource` was only
  taking the lock, not signaling interrupt first, so it waited on a lock
  nothing would ever release. Fixed by having `RunSource` set
  `interruptRequested_` before acquiring its lock too, same as
  `Stop`/`Restart`.

### Lua script manager (multi-file scripts)

Added a script manager to the Lua Editor page: a sidebar (`web/js/lua_scripts.js`)
listing every `.lua` file in the scripts directory (wherever `config.lua.script_path`
currently points), each with Open/Run/Set-Default/Delete actions, plus a
"New Script" field. Backed by:

```bash
curl -s http://127.0.0.1:8080/api/lua/scripts                          # list
curl -s http://127.0.0.1:8080/api/lua/scripts/foo.lua                  # read
curl -s -X POST .../api/lua/scripts/foo.lua -d '{"code":"..."}'        # write (create or overwrite)
curl -s -X DELETE http://127.0.0.1:8080/api/lua/scripts/foo.lua        # delete
curl -s -X POST http://127.0.0.1:8080/api/lua/scripts/foo.lua/run     # run this file from disk now
curl -s -X POST http://127.0.0.1:8080/api/lua/scripts/foo.lua/set-default  # run on next gateway startup
```

`IsValidScriptName` in `WebServer.cpp` restricts names to a strict
whitelist (`[A-Za-z0-9_.-]+\.lua`, no `..`) rather than blocking specific
bad substrings. Tested a `..%2f..%2fetc%2fpasswd.lua`-style path-traversal
attempt before this whitelist existed: it wasn't actually exploitable
(Crow's `<string>` route parameter doesn't URL-decode its capture, so the
literal `%2f` characters landed as an odd-but-harmless filename inside the
scripts directory, confirmed by checking the directory afterward — nothing
escaped it), but the whitelist rejects it outright regardless, and a
literal `../evil.lua` in the URL doesn't even reach the handler (404 —
Crow's router won't match a `<string>` segment containing an embedded `/`
in the first place).

### `Serial.Read()` truncated 90-byte frames to 66 bytes

Root cause: `SerialPort::ReadLoop` (in `SerialPort.cpp`) split every incoming
byte stream into CR/LF-delimited "lines" — correct for the Citizen ID card
reader (a single text line per swipe, per spec), but wrong for a generic
device whose 90-byte frame happens to contain a `\r`/`\n` byte as *payload*,
not a terminator. The loop treated that embedded byte as end-of-line,
flushed a truncated 66-byte "line", and the remaining 24 bytes sat in the
internal buffer waiting for the next delimiter — invisible to whatever
polled `Serial.Read()` next, since it only ever returned one flushed line.

Fixed by giving `SerialPort` a second, independent raw-byte accumulator
(`rawBuffer_`) that every incoming byte is appended to regardless of
CR/LF, drained via a new `ReadAvailable()` method — no framing assumptions
at all. `Lua_Serial_Read` now calls `serial_->ReadAvailable()` instead of
reading the line-based buffer the citizen-ID dispatch callback populated
(that callback/buffer, `lastSerialLine_`, was removed from `LuaEngine`
entirely since nothing else used it). Also switched from `lua_pushstring`
(null-terminated — would itself truncate at an embedded `\0`) to
`lua_pushlstring` (length-prefixed) for the same reason.

Verified with a PTY (`pty.openpty()` in Python) standing in for a real
device: wrote a 90-byte payload with a literal `0x0D` at byte 66, ran
`Serial.Open(); Sleep(400); local data = Serial.Read(); Log.Info("GOT_LEN="
.. #data)` via `/api/lua/run`, and confirmed `GOT_LEN=90` in the logs
(previously would have been 66, and would have also corrupted whatever
line-based dispatch was next).

**UPDATE — this turned out to be real, not a test artifact; see the
`Serial.Close()` entry further down for the actual fix.** Original note,
kept for the record of how the conclusion was reached (and got it wrong):
sending SIGTERM to the gateway while a serial port opened against a **PTY**
is open can hang shutdown indefinitely — `SerialPort::PlatformRead`'s
blocking `read()` doesn't seem to honor the configured `VTIME` timeout on a
macOS pseudo-terminal the way it does on a real serial device, so the read
thread never wakes up to notice `stopRequested_`. This reproduced even with
the simplest possible script (`Serial.Open()`, nothing else). Every earlier
test in this doc against a real `/dev/tty.usbserial-*` adapter shut down
cleanly, so this was concluded to be PTY-specific test-harness behavior
rather than a bug affecting real deployments, and left unfixed. That
conclusion was wrong: it wasn't tested against a USB-CDC/ACM device (a
*different* kind of "real hardware" that, like a PTY, has no actual UART
line discipline backing VTIME/VMIN) — and that's exactly the device a user
hit this on in production, via `Serial.Close()` rather than shutdown. One
real serial adapter behaving correctly isn't enough to conclude a whole
category of "real hardware" is unaffected.

### Auto-run-on-startup checkbox (script manager)

Each script in the Lua Editor's script list now has an "Auto-run on
startup" checkbox instead of the earlier "Default" button — checking it
calls the existing `/api/lua/scripts/<name>/set-default`; unchecking it
calls a new `POST /api/lua/clear-default`, which sets `lua.script_path` to
`""` (no script runs automatically until one is checked again; `main.cpp`'s
startup thread already logged-and-continued on a missing/empty script path
before this, so nothing needed to change there).

```bash
curl -s -X POST http://127.0.0.1:8080/api/lua/scripts/foo.lua/set-default   # check
curl -s -X POST http://127.0.0.1:8080/api/lua/clear-default                # uncheck
```

This surfaced a real bug in `ScriptsDir()` (added earlier for the script
manager): it derived the scripts directory from `lua.script_path`'s parent,
so clearing that to `""` made `std::filesystem::path("").parent_path()`
also empty — and the *entire script list* went blank, not just the
"default" marking (confirmed: `GET /api/lua/scripts` returned
`{"scripts":[]}` even though the files were still on disk). Fixed by
falling back to `"<config file's directory>/scripts"` when `script_path` is
empty or has no directory component. Verified: create a script, set it
default, confirm the list shows both files, clear the default, confirm the
list *still* shows both files with `default_name` now empty.

Also re-verified the buffer-clearing behavior from the truncation fix above
still holds (a user question prompted double-checking it): ran a script
that calls `Serial.Read()` twice with only one write in between — first
call returned the written data, second call (nothing new written) returned
`nil`, confirming `ReadAvailable()`'s swap-based clear leaves nothing stale
behind for the next read.

**Unrelated mistake worth recording:** while testing the above, a stray
`curl` round accidentally hit a *live, separately-running* gateway process
still bound to port 8080 (from an earlier manual session) instead of an
isolated test instance — the test build's own bind had silently failed
because that port was already taken, and the difference wasn't checked
before running requests against it. This left the real `config/config.json`
pointing `lua.script_path` at a script created and deleted purely for that
test. Caught and fixed by re-pointing it back at `main.lua`. Lesson applied
for the rest of this session: always confirm a test process's own PID
actually bound its port (`lsof -i :<port>` matching the right PID) before
running anything against it, rather than assuming a successful-looking
`curl` response came from the intended process.

### REST API: `Rest.SetServer`, `Rest.PostForm(path, data)`, table-shaped responses

Reported error running a user script (`Test_Verify.lua`):
`attempt to call a nil value (field 'SetServer')`. Reading that script (and
another, `REST_API.lua`) showed a consistent pattern across both, not a
typo: `Rest.SetServer(url)` instead of `Rest.SetUrl`, `Rest.PostForm(path,
data)` with a path argument, and the return value indexed as a table
(`response.status`, `response.body`, `result.success`, `result.message`) —
none of which the original single-arg, multi-return-value API supported.
Extended it to match rather than telling the user to rewrite two scripts to
match a narrower spec:

- `Rest.SetServer(url)` — alias for `Rest.SetUrl`, same underlying call.
- `Rest.PostForm(data)` still works as before (posts to the configured
  server URL); `Rest.PostForm(path, data)` joins `path` onto that URL first
  (`JoinUrl` in `LuaEngine.cpp` — treats an argument containing `://` as
  already-absolute, otherwise joins with exactly one `/`). `Rest.Get` grew
  the same optional argument for consistency, replacing its old
  "full URL override" meaning (nothing used that yet, so this wasn't a
  breaking change in practice).
- Both now return a single table instead of multiple values: `ok`,
  `status`, `body`, plus — the part that made `result.success`/
  `result.message` actually work — every top-level field of the response
  body is merged in directly when it parses as a JSON object. A server
  replying `{"success":true,"message":"OK"}` is readable as
  `result.success`/`result.message` with no manual JSON decoding in the
  script.

This changes the return shape documented in the Lua API Docs page and used
by `main.lua`'s original example (`local ok, status = Rest.PostForm(...)`)
— by the time this landed, the user had already replaced `main.lua` with
unrelated content that doesn't call `Rest.PostForm`, so nothing else needed
updating, but flagging here in case any other script out there still
depends on the old multi-return convention.

Verified against a real mock HTTP server (Python `http.server`) returning
`{"success":true,"message":"Citizen verified OK"}`: ran
`Rest.SetServer(...); Rest.PostForm("/api/citizen/verify", form)` and
confirmed `result.status == 200` and
`Log.Info(result.message)` printed `Citizen verified OK` — the exact
pattern from `Test_Verify.lua`. Also hit a red herring first: the mock
server's own test process had exited (a `time.sleep(20)` timeout elapsed
during the build) before the first attempt, producing a transport-level
failure (`status=0`) that looked like it could be the fix not working;
confirmed the mock server was actually reachable via a plain `curl` before
concluding the C++/Lua changes themselves were fine.

### `Serial.Close()` hung indefinitely on a real USB-CDC device — the PTY caveat from earlier was real after all

Reported symptom: after `Test_Verify.lua` logged "Citizen Card Received",
nothing else logged — the gateway just sat idle. The very next two lines in
the script after that log are `ParseCitizenCard(raw)` (pure string
processing, can't hang) and `Serial.Close()`. That pointed straight at it.

Root cause: `SerialPort::PlatformRead` (POSIX) relied entirely on the
termios `VTIME`/`VMIN` timeout set in `PlatformOpen` to make the blocking
`read()` periodically return so `ReadLoop` could notice `stopRequested_`.
Real UART hardware honors that timeout; the user's device
(`/dev/tty.usbmodemA62514221` — a USB-CDC/ACM virtual serial port, not a
physical UART) apparently doesn't, so `read()` could block forever with no
incoming data. `SerialPort::Close()` joins that thread, so it hung too —
and since `Close()` is called from inside the running script (a synchronous
native call holding the engine's lock the whole time), the entire Lua engine
hung with it, matching "program idle after read card" exactly.

This is the same failure mode flagged as a "PTY-specific test-harness
quirk, not a bug affecting real deployments" a few entries above — that
conclusion turned out to be wrong. PTYs and USB-CDC/ACM devices are both
*software*-implemented serial ports (no real UART line discipline driving
the timing), so it makes sense they'd share this behavior; the assumption
that only PTYs were affected wasn't verified against real hardware at the
time, just against the actual USB-serial adapter available in this
environment (which apparently *does* honor VTIME, unlike the user's
usbmodem device — clearly not all "real hardware" is equivalent here).

Fixed by no longer trusting VTIME/VMIN for the timeout at all:
`PlatformRead` now gates the blocking `read()` behind its own `select()`
call with a 200ms timeout. `select()`'s timeout is enforced at the file-
descriptor-readiness level by the kernel, independent of whatever the
device's line discipline does with VTIME — this should be reliable
regardless of the underlying serial device type.

Verified by reproducing the exact scenario with a PTY (the one environment
here that reliably exhibits the same non-VTIME-honoring behavior as the
user's real device): open, receive a chunk of data, `Serial.Close()` —
returned in 0.618s total (matching the script's own `Sleep(400)` plus a
fast close) instead of hanging. Also confirmed idle CPU stays at 0% (the
200ms `select()` poll isn't a busy-loop) and rebuilt the user's actual
project binary via their in-place CMake build (`cmake --build .` from the
repo root, which already had a configured `CMakeCache.txt`/`Makefile` from
an earlier vcpkg-based configure) so the fix is in the binary they're
actually running, not just a `/tmp` test copy.

### `Rfid.IsConnected()` + skip-and-continue instead of break

Added a `Modbus.IsConnected()` binding in the previous turn; this turn added
the equivalent `Rfid.IsConnected()` (RFID wasn't exposed to Lua at all
before — `LuaEngine::Bind` grew a fourth `RfidClient*` parameter, `main.cpp`
updated to pass `&rfid`). Also revisited the previous turn's Modbus check:
it used `break`, which permanently stops the whole script the first time
the PLC is unreachable (no automatic reconnect on the C++ side), even if it
recovers later. Restructured `Test_Verify.lua`'s main loop to check both
Modbus and RFID every iteration, log each one's state, and — if either is
down — log an error and `goto continue` (Lua's idiom for "skip to next
loop iteration", since Lua has no `continue` keyword) past the card-
processing block instead of breaking, so the script keeps retrying every
cycle rather than requiring a manual Restart.

Verified on an isolated instance with both PLC and RFID unreachable: logs
`PLC Disconnected`, `RFID Disconnected`, `Skipping this cycle: PLC/RFID not
ready` every ~100-120ms cycle, `lua_running` stays `true` throughout (confirms
it's still looping, not stopped), and `Stop()` still returns in ~37ms against
this new structure. Also validated the script's syntax specifically because
Lua's `goto`/label scoping rules are stricter than a plain "continue" (can't
jump into a local variable's scope) — the card-processing block is wrapped
in its own `do...end` so the `::continue::` label sits outside all of its
locals' scope, which is why it's placed after that block closes rather than
right at the top of the loop.

### RFID card registration: `OnRfidCardRead` never fired — found a real cross-thread deadlock in event dispatch

Feature request: register an RFID badge's UID against the citizen_id from
the most recently *verified* Citizen ID card (no separate employee_id
input), via `POST /api/card/register` (`employee_id`, `card_uid`,
`machine_id`). Needed `Rfid.IsConnected()`'s sibling — a way to actually
*receive* a UID — so added `LuaEngine::Bind`'s fourth `RfidClient*`
parameter's callback dispatching `OnRfidCardRead(uid)`, mirroring how
`OnCitizenCardRead` already worked.

First test: card UID sent over a fake RFID TCP server, gateway logged
`[Rfid] Card UID received: ...` (the C++-side callback ran fine) but
`OnRfidCardRead` never appeared in the Lua logs at all — no error, just
silence. Root cause: `RunSource` holds `mutex_` for a script's *entire*
top-level execution (needed for the Stop-interrupt fix from earlier), and
`Test_Verify.lua`'s top level *is* an infinite `while true do ... end` loop
— so that lock is held essentially forever while the script runs.
`DispatchEvent` (what the RFID callback called) unconditionally locks
`mutex_` before doing anything else. Since the callback runs on
`RfidClient`'s own background thread, it was blocking forever waiting for
a lock the script's own thread never releases — a silent, permanent
deadlock on that one call, not a crash or a logged error.

This exact same risk existed already for `OnCitizenCardRead` (also
dispatched from a background thread, from `SerialPort`'s read thread) — it
just hadn't manifested, because the specific Citizen ID card format has no
embedded CR/LF, so the *line-based* dispatch path (separate from
`Serial.Read()`'s raw-byte path) never actually fires for this card format.
Fixed both, not just the one that was reported, since the underlying cause
is identical.

Fix: added a lock-free event queue (`QueueEvent`/`pendingEvents_`, its own
mutex separate from the engine's main one) that any thread can push a
string-argument event onto without touching `mutex_` at all. Drained by
`DrainPendingEvents`, called from inside `Lua_Sleep` and `Lua_Serial_Read`
— both native bindings, so both already run on the script's own thread
*with `mutex_` already held by that thread's `RunSource` call*, meaning
draining the queue and calling straight into `L` there needs no additional
locking (and re-locking `mutex_` would deadlock anyway — `std::mutex` isn't
recursive). `RfidClient`'s callback (`main.cpp`) and `SerialPort`'s
citizen-card callback (`LuaEngine::Bind`) both switched from `DispatchEvent`
to `QueueEvent`. Plain `DispatchEvent` is kept for cases where the caller
knows the script's top level has already returned (the original "define
handlers, then return" convention, where the lock is free), documented on
both methods now.

**Known gap this doesn't cover:** `OnPlcInputChanged` (integer+number
args, from `main.cpp`'s poll thread) still calls `DispatchEvent` directly —
same deadlock risk, not fixed, since the queue only carries string-argument
events today. Extending `PendingEvent` to carry either shape would close
this; flagging rather than doing it now since it wasn't part of what was
reported and this fix was already large.

Verified end-to-end on an isolated instance with a fake TCP RFID server and
the user's real `demo_card_server` (already running, used as the actual
REST target rather than another mock): citizen card scan → verify fails
(test citizen_id not in the demo DB) → RFID scan → `OnRfidCardRead` fires
correctly now → correctly refuses to register (`LAST_CITIZEN_ID` was never
set, since verify failed) with `Cannot register card: scan a Citizen ID
card first`. Separately verified the actual register call construction by
running it directly with a fixed `LAST_CITIZEN_ID`: request reached the
real server with all three fields correct, got back `404 Employee not
found` (expected — the real server just doesn't have this test ID),
confirming the request/response contract end-to-end.

### First real Windows build (§1a): three bugs found and fixed

Running `build.bat` end-to-end on real Windows hardware for the first time
(previous Windows coverage was tool-inventory only — see the earlier note in
§1a) surfaced three separate failures, none related to each other:

1. **`vcpkg.json`'s `builtin-baseline` wasn't a valid commit SHA.** It was
   set to `"2024.11.16"` — looks like a vcpkg release tag, but
   `builtin-baseline` specifically requires a full 40-character git commit
   hash of the vcpkg repo, not a tag. vcpkg refused to resolve any package
   version and failed immediately on the first `vcpkg install`, before
   touching the compiler at all. Fixed by pointing it at the actual current
   HEAD commit of the `build/vcpkg` checkout
   (`39344dff01c5a5a0134caf2624cdd492f05d30ea` — vcpkg's own error message
   suggested this). Unrelated to the SQLite work below; this would have
   blocked *any* vcpkg-based configure on this machine, Windows or not.

2. **`CMakeLists.txt`'s libmodbus fallback branch used the wrong vcpkg
   package/target names.** It called
   `find_package(unofficial-libmodbus CONFIG REQUIRED)` and linked
   `unofficial::libmodbus::modbus`, but the libmodbus port actually
   installed by this vcpkg checkout exports a config package named plain
   `libmodbus` with a target named plain `modbus` (confirmed by reading the
   installed `libmodbusConfig.cmake` — it even prints
   `message(WARNING "find_package(modbus) is unofficial...")` on every use,
   which is a red herring, not the actual error). `find_package` failed
   outright since no `unofficial-libmodbus` config file exists anywhere in
   the install tree. Fixed by switching to `find_package(libmodbus CONFIG
   REQUIRED)` / `target_link_libraries(... modbus)`. That target's
   `INTERFACE_INCLUDE_DIRECTORIES` also points straight at
   `<prefix>/include/modbus` (matching libmodbus's own `#include
   <modbus.h>` convention) rather than this project's `#include
   <modbus/modbus.h>` convention — same mismatch the pkg-config branch
   already had a fix for, so applied the identical parent-directory fix
   here too, computed generically via `get_target_property(...
   INTERFACE_INCLUDE_DIRECTORIES)` rather than hardcoding a path.

3. **`SystemMonitor.cpp` failed to compile on MSVC**: `AF_UNSPEC`,
   `GAA_FLAG_SKIP_ANYCAST`/`GAA_FLAG_SKIP_MULTICAST`/`GAA_FLAG_SKIP_DNS_SERVER`,
   `GetAdaptersAddresses`, and `IP_ADAPTER_ADDRESSES` were all reported as
   undeclared in `NetworkUp()`. Root cause: `CMakeLists.txt` defines
   `WIN32_LEAN_AND_MEAN` project-wide (to avoid `<windows.h>` dragging in
   legacy Winsock 1.1 and conflicting with Winsock2 elsewhere in the
   codebase), which also means `<windows.h>` no longer implicitly pulls in
   enough of the Winsock2/IP-helper surface for `<iphlpapi.h>`'s
   declarations to resolve — `TcpSocket.cpp` already worked around this by
   including `<winsock2.h>`/`<ws2tcpip.h>` directly, but `SystemMonitor.cpp`
   didn't. Fixed by adding the same two includes before `<windows.h>` /
   `<iphlpapi.h>` in `SystemMonitor.cpp`'s Windows branch.

Also fixed a CMake dev-warning (not a build failure) noticed during the same
run: linking `SQLite::SQLite3` printed a deprecation notice on every
`target_link_libraries` call — this machine's bundled CMake (4.3.1)'s
`FindSQLite3` module renamed the imported target to `SQLite3::SQLite3`,
keeping the old name only as a deprecated alias. Switched to the new name.

All four fixes verified together in one clean rebuild: `hsf_gateway.exe`
built with no errors or warnings and ran, logging the expected
serial/Modbus "hardware not attached" warnings and
`Web server listening on 0.0.0.0:8080` — see §1a for the full transcript and
the config-persistence verification (`/api/config` round-trip surviving a
process restart).

### Card Reader Client API implemented and verified end-to-end (2026-08-04)

Implemented `request/Request/cardInputRESTAPI.md` in full: `CardClientManager`
(SQLite-backed, `config/clients.db`) for API-Key client management,
`CardCache` for the single-slot pending-card cache, `POST /api/card/input`
plus `/api/card/clients[...]` management routes, `Card.Available()`/
`Card.Get()`/`Card.Clear()` Lua bindings, an `OnCardReceived(uid)` event
handler, a new `Card` log category, and the `card_clients.html` management +
test-harness page. Rebuilt clean (no errors/warnings) on the first attempt
after the Windows fixes above, then verified against the real running
gateway on this machine:

- Created a client via `POST /api/card/clients` → got back a generated
  64-hex-char API-Key.
- `POST /api/card/input` with a bogus key → `401 {"success":false,"code":401,
  "message":"Invalid API-Key"}`, exactly the spec's shape.
- Same request with the real key and a `CardData`/`Position` multipart
  body → `200 {"success":true,"message":"Card Accepted"}`; `GET
  /api/card/cache` then showed the cached `uid`/`position`/`timestamp`, and
  the client's `last_access` updated.
- `POST /api/lua/run` with a script calling `Card.Available()` → true →
  `Card.Get()` → correct `uid`/`position` → `Card.Clear()` → confirmed
  `Card.Available()` false immediately after, all via the Lua log output.
- Disabling the client via `POST /api/card/clients/<id>/disable` made the
  same previously-valid API-Key immediately start returning 401.
- Killed and relaunched the process: a client created before the restart
  was still listed afterward, confirming `clients.db` persistence (not just
  in-memory state), same check as the config-persistence verification above.
- `card_clients.html` and `js/card_clients.js` both served with 200, and the
  new `Card` category showed up correctly in `/api/logs`.

### Card Reader Client API: Configuration-page toggle + quick key (2026-08-05)

Added a `card_api.enabled` master switch to `ConfigManager` (new `card_api`
section, same load/save/ApplyJson/ToJson pattern as the other five) and a
"Card Reader Client API" card on the Configuration page: an Enabled
checkbox (wired through the page's existing generic `name="section.field"`
form-save mechanism, no new JS needed for that part) plus a "Quick API-Key"
name field + "Generate & Save" button that calls the same
`POST /api/card/clients` the Card Clients page uses.

Verified against the real running gateway: `GET /api/config` includes
`card_api`; `POST /api/config` with `{"card_api":{"enabled":false}}` then a
`POST /api/card/input` with any key → `403
{"success":false,"code":403,"message":"Card Reader Client API is disabled"}`
regardless of key validity; re-enabling and creating a client via
`POST /api/card/clients` immediately let `POST /api/card/input` succeed
again with that key. `config.html`/`js/config.js` still served fine
afterward.

## 5. Known gaps

- No automated test suite (unit or integration) exists yet.
- The official vcpkg + CMake path is unverified in this environment (see
  §1) — the Homebrew build in §2 verifies the *code*, not the *build
  system*. Before relying on `CMakeLists.txt` in CI, run it once against a
  real vcpkg install and fix whatever `find_package`/`pkg_check_modules`
  assumptions don't hold.
- Windows: the vcpkg+CMake build now compiles and runs end-to-end (§1a,
  verified 2026-08-04) — `hsf_gateway.exe` starts, serves the web dashboard,
  and its SQLite-backed config persists across restarts. Still gaps within
  that: `SerialPort_win.cpp` was exercised only against "no device attached"
  (open fails gracefully) — no real Windows COM port was tested end-to-end
  with actual card-reader traffic; the RFID/Modbus TCP paths likewise only
  hit "unreachable" on Windows, same as every other platform (see the next
  bullet).
- `ModbusClient`/`RfidClient`/`SerialPort` were only tested against
  unreachable hardware (i.e., the "not connected" code paths). Real
  PLC/RFID/serial-device behavior is unverified.
