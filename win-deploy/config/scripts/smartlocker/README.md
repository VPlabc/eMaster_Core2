# SmartLocker

Employee and contractor locker access, written in Lua on top of the HSF
Gateway. Implements `request/SmartLocker/SmartLockerPlan.md`.

A card is presented at a cabinet; the gateway decides from its **local**
database whether that person may open a locker, pulses the PLC output that
releases the latch, watches the door open and close, and records what happened.
The employee list is kept in step with the backend by a daily REST
reconciliation and by RabbitMQ events in between.

---

## Layout

```
smartlocker/
├── main.lua              orchestration only -- the loop, nothing else
├── config.lua            defaults, merged with smartlocker.json
├── smartlocker.json      this installation's settings
│
├── api/
│   ├── server_api.lua    backend REST: active cards, one employee
│   ├── rabbitmq.lua      broker inbox: decode, dispatch, count
│   └── frontend.lua      the HTTP routes the dashboard calls, and its queries
│
├── hardware/
│   ├── modbus.lua        the ONLY file that talks to the PLC
│   ├── zk_reader.lua     card input (ZK controller or the card REST API)
│   └── buzzer.lua        beep patterns, on a ZK aux output or a PLC coil
│
├── database/
│   ├── database.lua      SQLite: schema, statements, transactions
│   ├── employee_db.lua   the employees table
│   └── locker_db.lua     the lockers table and the audit trail
│
├── core/
│   ├── employee.lua           what a record means: role, active, expired
│   ├── contractor_usage.lua   a contractor's working day: NOT_STARTED/USING/COMPLETED
│   ├── admin_access.lua       admin cards, the 5-scan sequence, the override
│   ├── locker.lua             doors, the state machine, restart recovery
│   ├── assignment.lua         who gets which locker
│   ├── access.lua             what happens on a swipe
│   └── sync.lua               daily reconciliation + realtime updates
│
├── utils/
│   ├── json.lua          encode/decode (the gateway has no JSON binding)
│   ├── paths.lua         locating smartlocker.json from the source tree
│   ├── time.lua          timestamps, expiry arithmetic
│   ├── logger.lua        runtime log + structured log + audit tables
│   └── scheduler.lua     cooperative timers for the main loop
│
└── tests/                one script per stage of the plan
```

The database is `config/smartlocker.db` (SQLite), beside the gateway's own
`config.db` and `logs.db`. The floor-plan UI lives outside this folder, in
`web/locker/`, because it is served by the gateway rather than by the script.

---

## Running

Copy nothing, configure two things, start it:

1. On the gateway's **Configuration** page, set the Modbus PLC address, the ZK
   reader address and the REST server + api key. SmartLocker inherits all of
   them.
2. Edit `smartlocker.json` -- at minimum the locker blocks (how many doors, of
   which type, at which addresses) -- then run `tests/test_locker_mapping.lua`,
   which prints the address array they expand to and needs no hardware.
3. Set `lua.script_path` to `smartlocker/main.lua`, or press **Run** on
   `smartlocker/main.lua` in the Lua Scripts page.
4. For the floor-plan display, turn on `web.locker_ui_enabled` and restart the
   gateway (the port is opened at start-up), then open
   `http://<gateway>:8081/`.

Each test is run the same way, from the Scripts list. They are ordered by the
plan's stages and are meant to be run in order:

| Stage | Script | Needs |
|---|---|---|
| 1.0 | `tests/test_locker_mapping.lua` | nothing |
| 1.1 | `tests/test_modbus_input.lua` | PLC |
| 1.2 | `tests/test_modbus_output.lua` | PLC (**opens doors**) |
| 1.3 | `tests/test_modbus_locker.lua` | PLC + someone at the cabinet |
| 1.4 | `tests/test_zk_reader.lua` | reader + a card |
| 1.5 | `tests/test_buzzer.lua` | whichever backend is configured |
| 1.6 | `tests/test_zk_control.lua` | controller (**operates relays**) |
| 2 | `tests/test_server_api.lua` | REST server |
| 2.1 | `tests/test_rabbitmq.lua` | broker (the decode half runs anywhere) |
| 2.2 | `tests/test_database.lua` | nothing |
| 4 | `tests/test_employee.lua` | nothing |
| 5/6 | `tests/test_locker.lua` | nothing |
| 10 | `tests/test_frontend_api.lua` | nothing |

Every test that writes rows uses `test_smartlocker.db`, never the live
database, and empties it at the start of each run.

> Running a test from the **Lua Editor's** Run button works too, but that path
> compiles the buffer with no file behind it, so the module search falls back to
> the scripts root. The two-line prelude at the top of each test handles both
> cases; do not remove it.

---

## The settings that decide whether it works

Everything else has a sane default. These have to be checked against the
hardware, and the tests exist to check them:

| Setting | Wrong value looks like | Settled by |
|---|---|---|
| `plc.door_open_value` | every door reads OPEN, or DOOR_OPEN_TIMEOUT on every swipe | `test_modbus_input.lua` |
| the expansion module's base address (the second entry in each `segments` list) | doors 1-6 work and every door after them is dead -- reads succeed and answer about nothing | `test_modbus_input.lua` |
| `locker.blocks[].output.count` | a locker is handed out that cannot be opened | `test_modbus_locker.lua` |
| `reader.card_format` | every card is CARD_NOT_FOUND | `test_zk_reader.lua` |

---

## The web UI, on its own port

The floor plan lives in `web/locker/` and is served by the gateway on a second
port, next to (not inside) the configuration UI:

```json
"web": { "locker_ui_enabled": true, "locker_ui_port": 8081, "locker_db_path": "smartlocker.db" }
```

Reads and actions travel by different routes on purpose:

| | path | who answers |
|---|---|---|
| the whole model | `GET /api/locker/state` on 8081 | C++, straight from SQLite (read-only) |
| live updates | `WS /ws` on 8081 | C++, pushed when the state changes |
| the audit trail | `GET /api/locker/history` on 8081 | C++, filtered and paged by SQLite |
| remote unlock | `POST /api/locker/lockers/<id>/unlock` | forwarded to **this script** |
| release | `POST /api/locker/lockers/<id>/release` | forwarded to **this script** |
| sync now | `POST /api/locker/sync` | forwarded to **this script** |

Reading from the database directly means the page stays live even while the
script is busy waiting out a 30-second door timeout. Writing goes the other way
for the opposite reason: only the state machine that owns the door may move it,
so an action becomes an HTTP route this script serves (`api/frontend.lua`), the
handler runs on the script's own thread at its next `Sleep()`, and the answer
goes back out the same connection.

The same handlers are reachable on the gateway's main port under
`/api/app/…` — which is how they are tested with curl:

```
curl http://localhost:8080/api/app                       # what this script serves
curl http://localhost:8080/api/app/lockers
curl -X POST http://localhost:8080/api/app/lockers/1/unlock

curl http://localhost:8080/api/app/admin                 # the admin state machine
curl -X POST http://localhost:8080/api/app/admin/exit
curl -X POST http://localhost:8080/api/app/admin/unlock/3
curl -X POST "http://localhost:8080/api/app/contractor/overtime?until=21:00"
```

`ADMIN_ACCESS_MODE` can only be ENTERED at the reader, by card. There is no
route that opens it -- the routes above can watch it, close it, and act inside
one that is already open.

### What the page shows

Layout and styling follow the "Hoàng Sơn Dashboard Format" design source; the
sample arrays in that mockup are the gateway's data here. Four things are worth
knowing before editing `web/locker/`:

- **The cabinet is drawn in three columns, filled from the right**, so locker 1
  sits in the last column and locker 18 in the first — the arrangement the
  design draws. That is a fact about the physical cabinet, not a style choice;
  `GRID_COLUMNS` / `GRID_FILL_FROM_RIGHT` in `app.js` are the one place to
  change it if this cabinet is numbered the other way round. **Check it against
  the real doors before going live.**
- **Two words that look alike.** A tile's colour is the DOOR's `status`
  (EMPTY/ASSIGNED/EXPIRED/ERROR). The small chip on a contractor's tile, and the
  "Ngày sử dụng" row in the panel, are that person's `usage_status` — the
  working day of CardScanPlan section 1. `app.js`'s `usageOf()` re-applies the
  same two rules `core/contractor_usage.lua` does (period first, then the date
  rollover), because a snapshot is a row and not a decision.
- **The stat tiles are the filter.** Clicking one dims every door that does not
  match; there is no separate filter row.
- **The history modal is filtered by SQLite, not by the browser.** It reads
  `GET /api/locker/history` (below), one page at a time, so the whole retention
  window is searchable and the record count in the corner is the real one.
  "Xuất Excel" re-runs the same filter and walks every page, so the file holds
  what the count says — up to `EXPORT_MAX`, and it tells you when that bites.

The role chip in the header is a **view mode, not a security boundary** — this
port has no login, so it only decides which controls the page offers. The
permissions modal says the same thing in Vietnamese, and the port belongs behind
a firewall either way.

### `GET /api/locker/history`

The audit trail, filtered and paged by SQLite (`src/WebServer.cpp`). Its
neighbour `GET /api/locker/logs?limit=` answers "the most recent N rows" and is
unchanged; this one exists because 180 days of a busy cabinet is tens of
thousands of rows, and filtering those in a browser is slow, silently truncated,
and makes the record count a lie.

| parameter | |
|---|---|
| `from`, `to` | `YYYY-MM-DD`, inclusive at both ends of the day. A malformed date is a **400**, not a silently dropped filter — widening a filter behind the operator's back is the failure that misleads. |
| `event` | comma-separated event codes (the modal's chips). Anything outside `A-Z 0-9 _` is a 400. |
| `locker`, `block` | one door, or one cabinet (resolved through `lockers`, so re-wiring a cabinet does not rewrite its history). |
| `q` | free text over the event, its reason, the result, the card — and the **person's name**, which is why this cannot be done client-side: a log row stores a card code, not a name. |
| `limit`, `offset` | 1..1000 (default 200), 0-based. |

```json
{ "ok": true, "total": 12345, "limit": 200, "offset": 0,
  "rows": [ { "id": 9, "locker_id": 7, "locker_number": 7, "block_id": 1,
              "card_code": "ABCD0107", "username": "Phạm Thị D",
              "event": "ACCESS_GRANTED", "result": "GRANTED",
              "reason": null, "created_at": "2026-08-18 08:12:00" } ] }
```

`total` counts what matches the filter *before* `limit`/`offset`. Each row
carries its own `locker_number` and `username`, so a door that has since been
re-wired — or a card that has since been removed — still reads correctly in its
own history.

Note this is the **gateway's** endpoint, not the demo server's: `locker_logs`
lives in `smartlocker.db` on this machine, and the demo server has no locker
tables at all.

## What the gateway gives Lua for this

Three bindings were added to the gateway for this application. If you are
reading old comments that say these do not exist, they are out of date.

**`Db.*` — SQL.** `Db.Open/Exec/Query/QueryOne/Scalar/Begin/Commit/Rollback`,
one SQLite connection per script (`src/SqlDatabase.cpp`), opened WAL so the web
layer can read the same file while this script writes it. Values are always
bound, never interpolated; `Db.NULL` binds SQL NULL, which a Lua `nil` cannot do
inside a parameter array. All four tables of Plan section 15 are real tables
here — `employees`, `lockers`, `locker_logs`, `system_logs` — in
`config/smartlocker.db`.

**`Http.*` — routes.** `Http.Register(method, path, handler)`, where a path
segment written `<id>` matches anything: `lockers/<id>/unlock`. Requests arrive
on the script's own thread; a handler returns `status, body [, content_type]`
and must return promptly, because it is holding an HTTP connection open.

**`zk.*` — relays, outputs and inputs.** `openDoor`, `auxOut`, `pulse`, `beep`,
`controlDevice`, `cancelAlarm`, `restartDevice`, `setNormallyOpen`, `getParam`,
`setParam`, `ioState`, `doorState`, `inputState`, `onAuxInput`.

There is still **no beeper command** in the PullSDK — nothing in
`plcommpro.dll` addresses the reader's own sounder, and no device parameter
configures it. `zk.beep()` pulses a relay, so an audible signal is whatever is
wired to it; `buzzer.backend` picks between an auxiliary output on the
controller (`"zk"`) and a PLC coil (`"plc"`), and stays disabled until one is
named. Two more facts of that protocol shape the code and are worth knowing:
outputs are **write-only** (nothing asks a relay what it is doing), and inputs
are **event-only** — an auxiliary input announces itself as RTLog event 220/221,
so `zk.inputState()` answers `nil` for an input nobody has triggered since
start-up rather than guessing "off".

---

## Structured log types

`Log.Write()` only accepts types declared in `config/log_definitions.json`.
Six were added there for this application:

| Type | Carries |
|---|---|
| `locker_access` | CARD_SCAN, ACCESS_GRANTED, ACCESS_DENIED, CARD_NOT_FOUND, CARD_INACTIVE, CARD_EXPIRED, WRONG_LOCKER_TYPE, NO_LOCKER_ASSIGNED, CONTRACTOR_USAGE, USAGE_NOT_STARTED, USAGE_OUT_OF_HOURS, OVERTIME_UPDATED |
| `locker_door` | LOCKER_UNLOCK, DOOR_OPEN, DOOR_CLOSE, DOOR_OPEN_TIMEOUT, DOOR_CLOSE_TIMEOUT |
| `locker_assign` | LOCKER_ASSIGNED, LOCKER_RELEASED, LOCKER_ASSIGN_FAILED |
| `locker_sync` | EMPLOYEE_SYNC, RABBITMQ_UPDATE |
| `locker_admin` | ADMIN_CARD_SCAN, ADMIN_SCAN_SEQUENCE, ADMIN_SCAN_TIMEOUT, ADMIN_MODE_ENTER, ADMIN_MODE_TIMEOUT, ADMIN_MODE_EXIT, ADMIN_OVERRIDE_ACCESS, ADMIN_OVERRIDE_DENIED, ADMIN_EXPIRED_LOCKER_OPEN |
| `locker_system` | LOCKER_ERROR, PLC_ERROR, READER_ERROR, startup and recovery events |

Definitions are read at gateway startup, so a restart is needed after editing
them.

---

## The contractor's day, and admin cards

Both come from `request/CardScanPlan.md` and are off by default -- a site that
does not configure them behaves exactly as before.

**Two different EXPIREDs.** A contractor record now carries a working day as
well as a period, and they are not the same thing:

| | Where it lives | What it decides |
|---|---|---|
| the PERIOD | `employees.start_at` / `expire_at`, from the server | whether the card works at all. An expired card is refused with the long beep. |
| the DAY | `employees.usage_date` / `usage_status`, kept locally | what the dashboard shows: NOT_STARTED → USING → COMPLETED, and EXPIRED once that day is over. |

The day resets with the date. Yesterday's `EXPIRED` day never keeps anybody out
today -- only the period does that. And nothing extends the period: a contractor
who never opened their locker does not earn extra days for it.

**Finishing the day is a gesture, not a time.** Every open is `USING`, however
many there are — opening the locker at 08:10, at noon and again at 16:00 is
three opens and no ending. The day ends when the contractor says so:
`complete_scan_count` scans of the same card in a row, no more than
`complete_scan_timeout` seconds apart.

```
scan                  -> USING
scan x3 (within 10s)  -> COMPLETED        one long beep + one short
scan again            -> USING again      they came back; the period is still valid
scan x3 again         -> COMPLETED again
midnight while USING  -> that day EXPIRES (swept hourly)
```

### Two holding models — `locker.contractor_mode`

How long a contractor keeps a locker is a site decision, so it is a switch. Pick
one; they answer the same question differently and running both would make
neither predictable.

**Mode 1 — by period (default).** The locker is theirs while their card is
valid. It comes back under the rule below.

**Mode 2 — by hold.** The locker is theirs for `locker.hold_hours` (24) from the
moment it was assigned, then it goes back in the pool for the next person
*whatever state it is in*. The hot-desk model, for a shared cabinet where nobody
keeps a door overnight. Mode 2 also honours the mode 1 rule, so somebody who
finishes and expires inside their hold frees the door early rather than sitting
on it for the rest of the day.

Mode 2 reclaims **without asking whether the person finished**, and that is
deliberate rather than an oversight: a cabinet that promises the next person a
door at a known time cannot also wait for the previous occupant to remember to
sign out. Belongings left inside become whoever-empties-the-locker's problem,
not the software's. A site that would rather wait for the person wants mode 1.

Two guards worth knowing: a locker whose `assigned_at` cannot be read is
**never** reclaimed by the hold rule (a missing timestamp would otherwise mean
"reclaim it on every sweep"), and `hold_hours: 0` turns the rule off. The sweep
is hourly, so a 24-hour hold is taken back within an hour of lapsing.

**A finished contractor's locker is taken back automatically** (`locker.auto_reclaim`,
on by default) — in mode 1 this is the only rule, and it needs *both* conditions:

| last day | card period | what happens |
|---|---|---|
| `COMPLETED` | ended | **reclaimed** — the locker goes `EMPTY` for the next person |
| anything else | ended | left `EXPIRED` for a person to open |

The second row is what makes a scheduled sweep safe. `COMPLETED` is the only
evidence this system has that somebody took their belongings with them; a
contractor whose period ran out mid-day, or whose day was swept to `EXPIRED`
because they never finished it, may still have a bag in there — and a timer is
the worst possible thing to be holding it when it decides otherwise. Those are
what the admin override (section 4) is for: a person opens it and looks.

The sweep runs hourly, after `end_of_day()` settles the day, at start-up, and
after each synchronisation (which is when `expire_at` actually moves). A door
that is mid-cycle is skipped rather than waited for.

No clock can tell "I am done for today" from "I came back for my phone", which
is why the hour of the day now finishes nothing. `complete_scan_timeout` is what
separates a deliberate gesture from ordinary use; set it to `0` and any
`complete_scan_count` opens in a day finish it instead. It has to stay above
`reader.repeat_ignore_ms` (3 s) — the reader drops a repeat of the same card, so
a shorter allowance makes the sequence impossible, and start-up says so.

The morning/afternoon windows now do one job: refusing an open outside them, and
only when `usage_timeout_enabled` is true (off by default). The server can still
extend tonight's finish with `POST /api/app/contractor/overtime?until=21:00` —
stored against that date, so tonight's overtime never becomes tomorrow's normal
finishing time.

**Admin cards are not employee records.** They are listed in `admin.cards` and
recognised before any database lookup, so no synchronisation can deactivate one
and no assignment pass can hand one a locker. Five consecutive scans (no more
than `scan_timeout` apart) open `ADMIN_ACCESS_MODE`; in it, a user card opens
*that person's* locker whatever state their card is in, and a further admin scan
opens the **next** expired locker -- one door per scan. Releasing every latch at
once is what the plan explicitly rules out, and this cabinet's solenoids share a
supply nobody sized for eighteen simultaneous pulses. The mode closes itself
after `override_timeout` seconds of nothing happening, or from the dashboard
(`POST /api/app/admin/exit`).

Note the interaction with the reader's own de-bounce: `reader.repeat_ignore_ms`
(3 s) drops a repeat of the same card, so five scans means five presentations at
least three seconds apart. `admin.scan_timeout` has to stay above it, and
start-up says so if it does not.

---

## Design notes worth knowing before changing anything

**The door wait is not a loop.** One Lua engine is one thread, and every native
binding takes the VM lock. A blocking 30-second `wait_close()` would stop card
reads, the scheduler and the broker for those 30 seconds -- and with two people
at two cabinets, the second is simply ignored. So `core/locker.lua` runs a state
machine one step per pass of the main loop and several doors can be in flight at
once. `Locker.wait_open` / `wait_close` still exist, for the hardware tests.

**Logical addresses are converted in one place.** The gateway's Modbus bindings
take protocol offsets; the configuration is written in logical addresses.
`hardware/modbus.lua` is the only file that knows the difference, and
`plc.address_mode` picks which map it applies -- `"locker_io"` for this cabinet,
`"modicon"` for the classic one.

**This cabinet is bit addressed, not register packed.** The plan assumed sixteen
doors to a word (30001 / 40001). The panel that was built instead gives every
door sensor its own discrete input from **20001** and every lock relay its own
coil from **10001**, six of each on the PLC and the rest on an expansion module
that starts at its own base address. So a block is configured as `segments` --
one entry per module, the address its first point answers at and how many points
it carries -- and `config.lua` flattens them into one address per door:

| door | input (sensor) | output (relay) | module |
|---|---|---|---|
| 1-6 | 20001-20006 | 10001-10006 | PLC internal I/O |
| 7-18 | 20017-20028 | 10017-10028 | expansion |

Nothing above `hardware/modbus.lua` sees the difference: a locker still carries
an address and a bit, and for a coil or a discrete input the bit is 0 because the
address *is* the bit. `tests/test_locker_mapping.lua` prints the whole array and
checks it without needing the PLC; run it after any layout edit.

**Eighteen doors are not eighteen round trips.** One door per address left the
scan cache with nothing to share, so a bit-space read that misses the cache
block-reads the whole run its address belongs to (`Config.BulkRanges()`, derived
from the same segments) and fills the cache with it. A module nothing asked about
in a given pass still costs nothing, and the gap between modules is two runs
rather than one read straddling addresses the PLC does not answer for.

**Reads and writes use the gateway's own PLC connection.** Not a preference:
`Modbus.WriteHolding` has no ip/port form -- only `ReadCoil`,
`ReadDiscreteInput` and `ReadInputRegister` do -- so the configured connection is
the only path that can write a holding register at all. `plc.host`/`plc.port` are
checked against the gateway's setting at startup and a mismatch is reported.

**Never overwrite unrelated output bits.** Every bit write is a read-modify-write
that bypasses the read cache, guarded against re-entrancy -- the 100 ms hold in
the middle of an unlock pulse is exactly where the engine delivers queued events,
so a handler firing there could otherwise interleave with it.

**An empty active-card list is refused.** It is indistinguishable from a server
that answered 200 with an empty envelope after a deploy, and acting on it would
deactivate every card in the building. Override with `sync.allow_empty_list`.

**Nothing on the access path touches the network.** A swipe is decided against
the local database. REST and RabbitMQ being down delays updates; they never keep
somebody out of their locker.

**The UI reads the database, not this script.** Drawing the floor plan costs the
script nothing, so the page stays live while a door timer is running. The cost
is that the dashboard shows only what has been *written*: `door_open` and
`runtime_state` are mirrored into the `lockers` table by `core/locker.lua` on
each change, and nothing else about a swipe is visible to the UI until it lands
in a row.

**Two log destinations, on purpose.** `logs.db` is the gateway's cross-application
record with a validated schema per type (the Logs page); `locker_logs` and
`system_logs` in `smartlocker.db` are this application's own audit trail, joined
to its lockers and shipped to its dashboard. Losing either should not cost the
other, which is why the write goes to both.

---

## Still to do

- **Authorisation on the action routes.** Plan section 34 says administrative
  endpoints must require it. Today anything that can reach port 8081 (or
  `/api/app/…` on 8080) can unlock a locker, so both ports belong on a trusted
  network until the gateway grows an auth layer these routes can sit behind.
- **Nothing verifies the UI's numbers against the panel.** The dashboard shows
  what the database says; a door whose sensor is not wired reads as closed
  there, exactly as it does everywhere else. `test_modbus_input.lua` is what
  proves the sensors.
- **Level 4 integration and long-duration stability tests** (Plan section 38)
  are on real hardware and cannot be scripted from here.
