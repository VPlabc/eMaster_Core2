# Changelog

All notable changes to the eMaster Gateway are recorded here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
(see `request/release.md` section 6).

## [Unreleased]

Implements `request/upgrade.md`,
`request/SmartLocker/SmartLockerPlan.md`, `request/CICD.md` and Phase 1 of
`request/AdvanceUpdate.md`.

### Added

**Production Lua packaging (`request/AdvanceUpdate.md` Phase 2, `docs/lua-packaging.md`)**
- Compiles a Lua application to bytecode, encrypts it (XChaCha20-Poly1305),
  signs it (Ed25519) and runs it **entirely from memory** — a deployed gateway
  needs no `.lua` source on disk at all.
- Compilation uses the gateway's own embedded interpreter rather than `luac`,
  so the bytecode cannot be built for a different Lua version or pointer width
  than the one that will load it. The runtime tag is recorded in the package
  and checked before the signature, so a mismatch says so instead of reporting
  an invalid signature.
- A package carries **every** module, not just the entry chunk. They are
  installed into `package.preload` and `package.path`/`package.cpath` are
  emptied, so `require()` resolves in-bundle and cannot fall back to plaintext
  source on disk.
- Build & Test (`POST /api/lua/packages/build`) runs five stages — compile,
  sign, verify, load, write — and reports which failed. The verify and load
  stages re-open the artifact and load every module with the real interpreter
  before it is written, so a package that cannot run is caught on the build
  machine rather than on the device.
- Versioned artifacts in `config/lua_packages/` with a `state.json` naming the
  active and previous package; deploy restarts the application immediately and
  rollback returns to the previous one. Neither the deployed package nor the
  rollback target can be deleted.
- A deployed package takes precedence over `lua.script_path` at startup, so the
  plaintext script cannot run alongside it.
- `LUA_COMPILE`, `LUA_DEPLOY` and `LUA_ROLLBACK` are enforced separately;
  creating a signing keypair is admin-only and refuses to overwrite an existing
  one.
- New Production Package panel on the Lua Editor, and
  `scripts/package-lua-test.sh`: 39 black-box checks including running with the
  source tree moved away, run in CI.

**API security (`request/AdvanceUpdate.md` Phase 1, `docs/security.md`)**
- **Every route on the web port is now authenticated and authorized.** Before
  this, `web/js/auth.js` checked `admin/admin` in the browser and said of
  itself "THIS IS NOT SECURITY" — all 86 API routes, including the ones that
  open a physical door, were reachable with `curl` by anyone who could open the
  port.
- Bearer tokens from `POST /api/auth/login`: 256-bit, opaque (not JWT, so they
  can be revoked), stored hashed, 30-minute lifetime, revoked on logout, on a
  password or role change, on disabling an account, and on restart.
- Passwords hashed with Argon2id via libsodium, which is a **required**
  dependency — unlike the optional-feature stubs elsewhere, a gateway that
  cannot hash a password is one nobody can log into.
- Three roles (ADMIN / OPERATOR / USER) over 21 named permissions. The whole
  policy is one readable table in `src/security/Permissions.cpp`, applied by
  Crow middleware before any handler runs; a route with no entry requires a
  session rather than being public.
- Per-account brute-force lockout (5 attempts, 15 minutes) recorded in SQLite,
  so it survives both a reconnect and a gateway restart; token-bucket rate
  limiting per source IP with separate login and API budgets.
- Backend input validation, request-size ceilings, security response headers,
  a CORS allowlist that is never `*`, and a security audit log in
  `config/security.db` readable on the new **Security** page.
- On first start the gateway seeds one ADMIN account and prints a generated
  one-time password to the console. There is no default password.
- `scripts/security-test.sh`: 60 black-box checks over the whole pipeline,
  run in CI.

### Fixed

- `IsValidScriptPath` rejected `..`, absolute paths and unexpected characters,
  but was a purely lexical check: a **symlink** inside the scripts directory
  contains no forbidden characters, so a script file could be made to read or
  write outside it. Script paths are now resolved and checked for containment.
- Failed logins answered 400 for a malformed username and 401 for a wrong
  password, which distinguished syntactically-invalid usernames from valid
  ones. Every failed login now returns the same 401 and body.
- The admin "Reset password" route could be pointed at your own account, which
  let an admin change their own password without proving they knew the current
  one — the exact check the self-service flow exists to enforce. Refused now in
  the API (`400 USE_SELF_SERVICE_PASSWORD_CHANGE`), not only greyed out in the
  UI. The Change My Password form also gained a confirmation field: the change
  revokes every session at once, so a typo would lock the user out with nothing
  to correct it with.

**Over-the-air updates (`request/CICD.md`, `docs/ota-update.md`)**
- The gateway checks a release server for a newer build every
  `update.check_interval_hours` and offers it in a corner popup on every page.
  Nothing installs without a click; a `mandatory` release only loses its
  *Later* button.
- `src/update/`: `UpdateManager` (check/download/verify/install state machine on
  its own thread), `UpdateInstaller` (release directories and the `current`
  symlink), `Downloader` (streams to disk with progress, unlike `RestClient`),
  `Sha256` (no external dependency — a checksum must be checkable on every
  build) and `SignatureVerifier` (OpenSSL, with a fail-closed stub when it is
  absent).
- Packages are rejected unless their bytes match the manifest's SHA-256 **and** a
  detached signature over `<version>\n<platform>\n<sha256>` verifies against a
  public key on the device. Version and platform are inside the signed message
  so a signed *older* package cannot be replayed as a downgrade.
- Installs unpack into `<root>/releases/<version>/` and repoint `<root>/current`
  (atomically on POSIX). A release that fails `max_boot_attempts` startups is
  rolled back automatically, with no agent outside the process — the boot
  counter is incremented in `main()` before any hardware is opened, and cleared
  only after `health_confirm_sec` of healthy uptime.
- New `update` config section, a **Software Update** card on the Configuration
  page, and `GET /api/update/status` + `POST /api/update/{check,install,dismiss}`.
  Status is pushed over the existing `/ws` as `{"type":"update"}`.
- Release pipeline: `scripts/make-manifest.sh` generates and signs
  `version.json` (one file, all platforms) and `release.yml` publishes it with
  the packages. `scripts/install-ota.sh` and `deploy/hsf-gateway.service` lay
  out and supervise a managed install.
- `openssl` added to `vcpkg.json`. Without it the gateway still builds; update
  signatures then cannot be verified and installs are refused rather than
  silently trusted.

**SmartLocker (`request/SmartLocker/SmartLockerPlan.md`)**
- `config/scripts/smartlocker/`: the locker application in Lua — card access,
  locker assignment with type and gender rules, the door state machine with
  open/close timeouts, daily REST reconciliation, RabbitMQ realtime updates,
  restart recovery, and a test script per stage of the plan.
- Its data lives in `config/smartlocker.db`: `employees`, `lockers`,
  `locker_logs`, `system_logs`.
- Floor-plan dashboard (`web/locker/`) on its **own port**
  (`web.locker_ui_enabled` / `web.locker_ui_port`, default 8081): the cabinet
  drawn door by door, four status colours, filters, a detail panel with the
  card holder and the door's history, remote unlock and release, and a live
  WebSocket feed of the whole model. Reads the database read-only; every action
  is forwarded to the script that owns the hardware.

**SQL for Lua (`Db.*`)**
- `SqlDatabase` (`src/SqlDatabase.cpp`): a general-purpose SQLite handle, WAL,
  bound parameters, `BEGIN IMMEDIATE` transactions with nesting.
- `Db.Open/Close/IsOpen/Path/Exec/Query/QueryOne/Scalar/Begin/Commit/Rollback/
  InTransaction/Quote`, plus `Db.NULL` for binding SQL NULL. One connection per
  script, closed with its runtime.

**HTTP routes from Lua (`Http.*`)**
- `Http.Register(method, path, handler)` with `<segment>` wildcards;
  `/api/app/<path>` (up to three segments) forwards to whichever running script
  serves it, and `GET /api/app` lists them. Handlers run on the script's own
  thread at its next `Sleep()`, with a 5s timeout on the web side.

**ZK controller outputs and inputs**
- `PullSdkClient::ControlDevice` / `SetDeviceParam`, and `ZkController`
  wrappers: `OpenDoor`, `SetAuxOutput`, `PulseOutput`, `Beep`, `CancelAlarm`,
  `RestartDevice`, `SetNormallyOpen`, `GetParams`/`SetParams`, plus door-sensor
  and auxiliary-input state gathered from the RTLog poll.
- Lua: `zk.openDoor`, `zk.auxOut`, `zk.pulse`, `zk.beep`, `zk.controlDevice`,
  `zk.cancelAlarm`, `zk.restartDevice`, `zk.setNormallyOpen`, `zk.getParam`,
  `zk.setParam`, `zk.ioState`, `zk.doorState`, `zk.inputState`,
  `zk.onAuxInput`.
- The PullSDK has no beeper command, so `zk.beep()` pulses a relay: an audible
  signal is whatever is wired to that output.

**Multiple Lua scripts at once (sections 20-28)**
- `LuaRuntimeManager` runs any number of scripts simultaneously, each in its
  own `LuaEngine` — its own `lua_State`, its own thread, its own metrics.
  Scripts also no longer execute on the HTTP worker thread that started them.
- Independent lifecycle per script: `STOPPED` / `STARTING` / `RUNNING` /
  `STOPPING` / `ERROR`, started and stopped individually.
- A script that faults is marked `ERROR` and reaped alone; the others keep
  running. Its error is recorded under the built-in `script_error` log type
  with script, path, message, type, line and runtime id.
- Duplicate-start protection: running an already-running script reports
  "already running" instead of starting a second instance.
- Lua Editor: a checkbox per script with `Run Selected` / `Stop Selected`, a
  live state badge per row, a runtime strip showing every runtime, and per-row
  stop buttons.
- System Info lists every runtime with state, CPU, RAM, core and runtime id,
  plus a block per failed script; `lua_runtime` WebSocket messages per
  section 25.
- `POST /api/lua/scripts/stop`, `GET /api/lua/runtimes`,
  `POST /api/lua/runtimes/prune`; `/api/lua/scripts/run` and `/api/lua/stop`
  now take several script names, or one.

**Structured logging (sections 7-16)**
- `LogStore`: business/application logs in SQLite
  (`log_definitions` / `log_entries` / `log_values`, `config/logs.db`), so
  scripts can log arbitrary field sets without a table per log type.
- Log types and their fields are declared in `config/log_definitions.json`.
  `Log.Write(type, data [, level])` validates against that declaration —
  unknown type, unknown field, missing required field, or a value that doesn't
  parse as its declared type — and writes entry plus values in one
  transaction. `Log.Types()` lists what a script may write.
- Logs page split into **Runtime Log** (the existing live tail) and
  **Structured Logs**: date range, multi-select log type, level, script and
  keyword filters, server-side pagination, a detail view showing every field
  in declaration order, and CSV export of the current page. With a single log
  type selected, that type's fields become the table's columns.
- Entries older than `logging.retention_days` (default 90) are pruned at
  startup.

**Lua configuration API (sections 5-6)**
- `Config.Get` / `Config.Set` / `Config.Exists` / `Config.GetCategory`, keyed
  as `"<section>.<key>"` — the same names the Configuration page and
  `/api/config` use. `Config.Set` persists immediately, and rejects unknown
  keys and type changes rather than accepting a write it would then drop.
- New `system` (`machine_id`, `device_name`), `logging` and `zk` (`ip`, `port`,
  `timeout_ms`, `password`) sections, all editable on the Configuration page.
  `zk` is separate from `rfid` even though `rfid.mode` can be `"zk"`: on a
  machine that reads citizen cards over serial and card UIDs from a ZK
  controller those are two devices at two addresses, and one field cannot
  describe both.
- `config/scripts/config.lua` now reads its installation-level settings —
  server URL, API key, PLC address, ZK address, machine id — from the gateway
  configuration instead of holding a second copy of them. Comments there used
  to read "must match config.json's modbus.ip"; that is now a mechanism rather
  than an instruction. Mechanical timings, retry limits and the coil addresses
  stay literal, since no gateway field means them.
  - Every lookup keeps its pre-migration value as a fallback and warns when it
    uses one, so a database that predates a key (or has the field blank, which
    is what an untouched Configuration page input looks like) behaves exactly
    as before. The same guard covers a gateway too old to have the `Config`
    table at all.
  - The output coils are deliberately not derived from
    `modbus.output_coil_start`: they are not contiguous on this machine (scan
    and out share 1280, collect is 1282), so a base address plus offsets would
    have moved `OUTPUT_OUT` to a terminal nothing is wired to.
- `zkcard.lua` passes the configured ZK comm password instead of a hardcoded
  empty string.

**Modbus (sections 17-19)**
- `Modbus.RegisterDiscreteInput(name, address)` and
  `Modbus.RegisterCoilInput(name, address)`, naming the address space at the
  call site instead of relying on `RegisterInput`'s `"auto"` probing.

### Changed

- Modbus registrations belong to the runtime that made them: re-running or
  stopping one script clears only its own dashboard points. Previously any
  script start wiped the whole registry, which was invisible while only one
  script could run.
- Lua chunks are named after the script, so errors read
  `card/issue.lua:145: attempt to call nil value` rather than
  `[string "-- card issuing workflow..."]:145:` — which identified nothing
  once several scripts could fail at once.
- `Log.Info`/`Warning`/`Error`/`Debug` are unchanged and remain the debugging
  channel; `Log.Write` is the durable, searchable one.

### Fixed

- A script that failed to *compile* reported as a silent `STOPPED` with no
  error: nothing recorded the failure, because the recording only happened for
  a script that had started.
- Handler-only scripts never reported memory or CPU core. The sampler ran from
  the debug hook (every 64,000 VM instructions) and from `Sleep`, and a script
  that only defines handlers reaches neither — so System Info showed 0.00 MB
  for its whole life. Metrics are now also sampled when the top-level code
  returns and after each dispatched event.
- A stopped script's `Tcp.Connect` socket stayed open until the gateway
  exited; it is closed with the Lua state.
- Serial and ZK card callbacks were installed by `LuaEngine::Bind`. Each
  device holds one callback, so once several engines existed the last one
  constructed would have silently taken every card read from the others; the
  manager owns those slots and fans events out to all live runtimes.

## [1.0.0] - 2026-08-10

First packaged release. Everything below shipped during initial development,
so it is listed as Added rather than split across releases that never existed.

### Added

**Card issuing workflow**
- Lua-driven card issuing state machine with failure and timeout handling.
- Citizen ID card reading over USB serial, parsed to JSON and dispatched to
  the `OnCitizenCardRead` handler.
- ZKTeco access-controller integration (PullSDK, in-process), with
  auto-reconnect, heartbeat and RTLog polling for card events.
- Wiegand-26 card value reconstruction from the 24 RTLog data bits.
- TDM-800 LED display support on a second serial port, with Vietnamese and
  English message sets.
- Card Reader Client API: external readers `POST /api/card/input` with a
  per-client API key, managed in their own SQLite database.

**PLC / Modbus**
- Hand-rolled Modbus TCP client (MBAP + PDU) rather than libmodbus, whose
  Windows TCP connect path rejects real Windows `SOCKET` handles.
- Coils (FC01), discrete inputs (FC02), holding registers (FC03), input
  registers (FC04) and the FC05/FC06/FC0F/FC10 writes.
- Named dashboard I/O: `Modbus.RegisterInput` / `RegisterOutput`, polled by
  the gateway and pushed over the WebSocket.
- Typed register points: `Modbus.RegisterRegister` with uint/int 16/32/64,
  float32/64 and string, in ABCD/BADC/CDAB/DCBA byte and word order, plus
  `Modbus.GetRegister` / `SetRegister` for scripts.
- PLC auto-reconnect and a periodic REST health probe.

**Web dashboard**
- Light/dark theme, applied before first paint so there is no flash.
- Login page with Admin and Client roles.
- Dashboard: CPU/RAM history, connection status with last-connected times,
  named PLC input/output panels, PLC register table, runtime variables.
- Lua script browser with folders and per-script runtime statistics.
- Lua editor with syntax highlighting, Admin only.
- Log viewer with level, category and text filters, live tail, pause and
  clear.
- Test Tools: REST API client, REST endpoint browser, Modbus TCP client with
  an address scanner, serial master, and card client management.
- Lua API documentation served from the gateway itself.

**Platform**
- `HSF_ENABLE_ZK` build option. Defaults ON only for Windows x86, where the
  32-bit-only PullSDK can load; OFF elsewhere, which is what allows Linux,
  macOS and 64-bit Windows to build at all. With it off, a stub PullSdkClient
  makes every `zk.*` call fail with `NotSupportedOnThisPlatform` instead of
  removing the module from the Lua API.
- Version and build metadata (`--version`, `/api/status`, System Info tab).
- Resources resolved relative to the executable, so an extracted release
  package does not reach back into the machine it was built on.
- Configuration stored in SQLite, seeded once from a sibling JSON file on
  first start.

### Fixed

- Registered PLC inputs read only from the discrete-input space (FC02), which
  left them frozen on PLCs that answer both function codes at an address
  while carrying the live state in the coil. Inputs now declare their space,
  and each dashboard row shows which function code produced its value.
- Registry updates were keyed by address, so two points sharing an address
  with different sources overwrote each other's value and function code.
  They are now keyed by name.
- Modbus exceptions counted toward the consecutive-failure threshold, so
  probing an unmapped address dropped a healthy PLC link. A device that
  answers is alive, exception or not.
- `ModbusClient::Connect()` returned true when the `connected_` flag was set
  but the socket was closed, logging "PLC reconnected" indefinitely.
- Pausing the log viewer discarded incoming entries rather than deferring
  them; the gateway sends each entry once, so those lines were lost.
- Log messages and card client names were interpolated into the DOM as HTML;
  both are now rendered as text.
- Lua syntax highlighting corrupted comments, because per-token-type regex
  passes re-scanned markup inserted by earlier passes.
- `string.upper` deleted uppercase Vietnamese diacritics (Lua's is ASCII
  only).
- A normal script stop was reported as an error, because the debug-hook
  interrupt was recorded as a fault.
- Runtime metrics were never sampled for a sleeping script.
- `require()` failed for scripts run from the editor, and for scripts in
  subfolders.
- `RfidClient` cached its configuration at startup, so mode and IP changes
  did nothing until restart.
- A `try_lock` on an already-held mutex (undefined behaviour) hung the
  gateway when a Lua script called `zk.connect()`.

### Known issues

- ZKTeco card reading requires the Windows x86 package. The PullSDK ships as
  a 32-bit Windows DLL; Linux, macOS and Windows x64 packages build and run
  without it.
- `ModbusClient` holds its mutex across the TCP connect, so `/api/status` can
  block for up to the connect timeout while a reconnect to an unreachable PLC
  is in flight.
- Role restrictions are enforced in the frontend only. Any authenticated
  client can still call Admin endpoints directly.
- Only one Lua script runs at a time; starting a script clears the previous
  script's registrations. (Fixed in Unreleased, above.)
- The Modbus TCP Server and Serial Slave simulators in Test Tools are not
  implemented.
- There is no automated test suite; validation is the manual checklist in
  `request/release.md` section 31 plus `scripts/validate-lua`.

[1.0.0]: https://example.invalid/HSF_Gateway/releases/tag/v1.0.0
