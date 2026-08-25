# eMaster Gateway — API Reference

Two independent API surfaces, in opposite directions. Which one you need
depends on what you are building:

| You are building… | Read |
|---|---|
| The **Employee Registration Server** the gateway reports to | [Part 1 — Outbound](#part-1--outbound-what-the-gateway-calls-on-your-server) |
| A client of the gateway (dashboard, monitoring, an integration) | [Part 2 — Inbound](#part-2--inbound-the-gateways-own-rest-api) and [Part 3 — WebSocket](#part-3--websocket) |
| A card reader that pushes UIDs to the gateway | [§2.10 Card Reader Client API](#210-card-reader-client-api) |

Everything here reflects the code in this repository. Section numbers in
brackets, e.g. *(upgrade.md §10)*, point at the spec a behaviour comes from.

**Contents**

- [Part 1 — Outbound: what the gateway calls on your server](#part-1--outbound-what-the-gateway-calls-on-your-server)
- [Part 2 — Inbound: the gateway's own REST API](#part-2--inbound-the-gateways-own-rest-api)
- [Part 3 — WebSocket](#part-3--websocket)
- [Part 4 — What changed in this update](#part-4--what-changed-in-this-update)

---

# Part 1 — Outbound: what the gateway calls on your server

This is the contract a server developer implements. The gateway is the client.

## 1.1 Transport

| | |
|---|---|
| Base URL | `rest.url` in the gateway configuration (Configuration page → REST API) |
| Method | `POST` for all business calls |
| Body | `multipart/form-data` — **not** JSON, not `x-www-form-urlencoded` |
| Auth header | `API-Key: <rest.api_key>` (sent only when the key is non-empty) |
| Timeout | `rest.timeout_ms`, default 5000 |
| Retries | `rest.retry_count` **total attempts**, default 3, backing off 200 ms × attempt |
| TLS | `rest.ssl_enable` — when false, peer *and* host verification are both off |

`multipart/form-data` is used because it is also what the physical card readers
post, so one code path serves both. Every field value is sent as a plain text
part; there are no file parts.

A request counts as successful when the HTTP status is **2xx**. A 3xx, 4xx or
5xx is a failed attempt and is retried up to `retry_count` times.

### The retry is not idempotent-safe on your side

`retry_count` applies to transport-level failure *and* to any non-2xx reply. If
your handler creates a record and then returns 500, the gateway will send the
same form again. Make `/api/card/register` and `/api/card/cancel` idempotent on
`(citizen_id, card_uid)`, or the third retry of a partially-successful register
will leave you with duplicates.

## 1.2 Response contract

Reply with a **JSON object**. The gateway merges your object's top-level fields
into the result table the Lua workflow inspects, so field names matter.

```json
{ "success": true, "message": "OK" }
```

| Field | Type | Meaning |
|---|---|---|
| `success` | boolean | **Required.** The verdict. Checked as `success == true` — a JSON `true`, not `1`, not `"true"`. Anything else is treated as a rejection. |
| `message` | string | Human-readable reason. Shown in the runtime log and used to pick the LED text. Optional but strongly recommended on a rejection. |
| `code` | string/number | Optional machine-readable reason, see [rejection classification](#123-rejection-classification-verify-only). |
| `reason` | string | Optional, same role as `code`. |

### Reserved field names — do not use

The transport fills these in before merging your body, so a top-level field of
the same name **overwrites** it and breaks the gateway's own bookkeeping:

| Name | Holds |
|---|---|
| `ok` | whether the HTTP request itself succeeded (2xx) |
| `status` | the HTTP status code |
| `body` | the raw response text |

A reply of `{"status": "ok"}` replaces the numeric HTTP status with the string
`"ok"`, which is exactly the kind of thing that reads fine in a test and
misreports in production. Put your own status in `success` / `code`.

### 1.2.1 HTTP status vs. `success`

They are independent, and both are read:

| HTTP | `success` | Gateway behaviour |
|---|---|---|
| 2xx | `true` | Accepted, workflow continues |
| 2xx | `false` | **Answered and rejected.** No retry of the HTTP call; the workflow takes its rejection path and shows `message` |
| 2xx | absent | Treated as a rejection by the business calls (`success == true` is required) |
| non-2xx | anything | Transport failure — retried, then reported as "server error" |

Return **200 with `success: false`** for a business rejection (employee not
registered, card already issued). Reserve non-2xx for genuine faults. A 404 for
"unknown employee" costs the gateway three round trips and reports the wrong
kind of error to the person at the machine.

### 1.2.2 An unreachable server is a different state

The workflow distinguishes three outcomes, and they drive different LED text:

1. **No reply** (connection refused, DNS failure, timeout) → "server error".
2. **Reply with `success: false`** → the employee was rejected; which rejection
   is decided next.
3. **Reply with `success: true`** → proceed.

### 1.2.3 Rejection classification (verify only)

On a rejected verify, the gateway decides between *"you are not registered —
go and register"* and *"your account is inactive — talk to HR"*, because the
person standing at the machine has to do something different in each case.

It lowercases the concatenation of `code`, `reason` and `message` and looks for
any of these substrings:

```
inactive · not active · disabled · locked · suspended · blocked
khong hoat dong · da bi khoa · bi khoa · ngung hoat dong
```

A match shows **account inactive**; anything unrecognised falls back to the
safe generic **not registered**. So to have an inactive account reported
correctly, say so in one of those three fields — e.g.
`{"success": false, "code": "account_inactive", "message": "Tài khoản đã bị khóa"}`.

The patterns live in `config/scripts/citizen.lua` (`INACTIVE_PATTERNS`) and are
meant to be extended as your wording settles.

## 1.3 Endpoints

### `POST /api/citizen/verify`

Sent once per issuing attempt, immediately after the citizen ID card is read
and parsed. Asks: may this person be issued a card?

| Form field | Source (offsets into the raw card text) |
|---|---|
| `citizen_id` | 12 characters, offsets 16–27 |
| `citizen_name` | assembled name, see the warning below |
| `birthday` | **6 characters**, offsets 30–35 — not 8, and not normalised |
| `gender` | **1 character**, offset 38 |
| `serial_number` | 10 characters, offsets 6–15 |
| `country` | the literal `"VNM"` — hardcoded, never read from the card |
| `machine_id` | `Config.MACHINE_ID` — identifies which kiosk asked |

All values are strings sliced straight out of the card's own encoding
(`config/scripts/citizen.lua`). Nothing is normalised, validated or
transliterated: parse `birthday` according to your card layout, and do not
assume it is 8 digits or ISO-8601.

> **`citizen_name` can contain double spaces.** It is built as
> `last .. " " .. middle .. " " .. first`, with each part defaulting to `""`.
> A person with no middle name yields `"NGUYEN  VAN"` — two spaces — and a
> partially-parsed name can have a leading or trailing one. Collapse whitespace
> before any exact-match comparison.

A field whose value came out `nil` is **absent from the multipart body**
entirely rather than sent empty, so treat every field except `machine_id` as
optional on the wire.

Reply `success: true` to let the workflow dispense a card.

#### `role` — which machine issues the card *(new)*

This gateway drives **two dispensers**, loaded with different card stock:

| Machine | For | PLC I/O |
|---|---|---|
| `staff` | employees — *Nhân viên* | X0–X3 / Y0–Y2 |
| `contractor` | contractors — *Nhà thầu* | X4–X7 / Y4–Y7 |

Your verify reply decides which one hands over a card. Return the person's role
as a **top-level string** alongside `success`:

```json
{ "success": true, "role": "contractor" }
```

The gateway takes the first of these fields that is present and non-empty:

```
role · user_role · staff_type · employee_type · type
```

and matches it **case-insensitively as a substring**, so `"Contractor"`,
`"NHA THAU"` and `"external contractor"` all select the contractor machine:

| Selects `contractor` | Selects `staff` |
|---|---|
| `contractor` · `subcontract` · `vendor` · `nha thau` · `nhà thầu` | `staff` · `employee` · `nhan vien` · `nhân viên` |

Both spellings of the Vietnamese are accepted, with and without diacritics.

**If the field is missing, empty, or matches nothing, the card is issued from
the `staff` machine** and the gateway logs a warning naming the raw text it
received. That is a deliberate fallback — a person is served rather than turned
away over an unmapped spelling — but it means a typo in your role value fails
*silently to the wrong stock*. Check the gateway's log (or the `CitizenRole`
and `CardMachine` dashboard variables) after adding a new role value.

The mapping lives in `Config.ROLE_MACHINE` in `config/scripts/config.lua` and is
meant to be extended as your wording settles; adding a value is one line and
needs no rebuild.

> **Two parsers exist.** The workflow uses the Lua one described above. The C++
> `CitizenIdParser` — which feeds the `OnCitizenCardRead(cardJson)` event, and
> whose JSON uses `birth_date`, `last_name`, `middle_name`, `first_name`,
> `raw_text` — is a separate code path. If you are building against the event
> rather than against this endpoint, that is the shape you will see.

### `POST /api/card/register`

Sent after a card has been dispensed and its UID read at the scan position.
Binds the card to the employee.

| Form field | Notes |
|---|---|
| `citizen_id` | the same value sent to verify |
| `card_uid` | uppercase hex, MSB-first — the number printed on the card |
| `machine_id` | |

On a rejection the gateway collects the card, retries up to
`Config.MAX_SCAN_RETRY` times (a fresh card each time), then gives up and shows
a server error. So a persistent `success: false` here costs three cards' worth
of mechanical cycles — return a rejection only if a retry could not help.

**`card_uid` is reconstructed, not raw.** The ZK controller reports only the 24
Wiegand-26 data bits; the value on the card is one bit wider. The gateway
shifts left and appends the computed odd-parity bit, so the UID you receive
matches what is printed and what other systems display. If you are comparing
against values captured from a different reader, expect an off-by-a-factor-of-2
if that reader reported the raw 24 bits instead.

### `POST /api/card/cancel`

Sent when a card was registered but the workflow could not complete — the
employee never took it, or a later stage failed. Undo the binding.

| Form field | Notes |
|---|---|
| `employee_id` | **Note the name.** This endpoint sends `employee_id`, while verify and register send `citizen_id`. Same value. |
| `card_uid` | |
| `machine_id` | |

The inconsistency is in the shipped script (`config/scripts/rest.lua`); it is
documented rather than silently fixed because your endpoint may already depend
on it.

### `POST /api/card/status/` — machine health

**Live as of this update.** The gateway sends these unprompted; implement the
endpoint or the machine's consumable alerts go nowhere. Nothing else on the
server needs to change.

Reports a consumable or hardware condition read off the PLC's discrete inputs:
empty hopper, full reject bin, low card stock.

| Form field | Notes |
|---|---|
| `machine_id` | `Config.MACHINE_ID`, e.g. `GW001` — which kiosk |
| `machine` | **which dispenser**: `staff` or `contractor`. `machine_id` names the kiosk and is identical for both, so this is the only field that says which hopper needs refilling |
| `issue` | `card_not_in_hopper` · `card_reject_full` · `card_source_low` |
| `status` | `"active"` when the condition appears, `"cleared"` when it goes away |
| `address` | the Modbus input address behind the flag, as a **string** (`"1027"`) |
| `message` | human-readable detail, `"<machine>: <text> (input <address>)"` |

The trailing slash is deliberate: frameworks with `APPEND_SLASH` answer a
slash-less POST with a redirect that drops the form body.

#### The three conditions

| `issue` | Input (staff / contractor) | `message` text | Blocks issuing? |
|---|---|---|---|
| `card_not_in_hopper` | 1027 / 1031 | `Staff: No card in the hopper (input 1027)` | **Yes, for that machine only** — someone whose role selects it is refused; the other machine keeps serving |
| `card_reject_full` | 1026 / 1030 | `Staff: Card reject bin is full (input 1026)` | No — reported and logged, cards keep being issued |
| `card_source_low` | 1025 / 1029 | `Staff: Card source is low (input 1025)` | No — early warning only |

Both dispensers are polled, so **every issue arrives twice over: once per
machine**, distinguished by the `machine` field. Addresses come from
`Config.MACHINES` in `config/scripts/config.lua` (staff X1–X3, contractor X5–X7)
and are read in whichever Modbus address space `Config.INPUT_SOURCE` names
(currently `discrete`, FC02).

Only when **both** hoppers are empty does the gateway stop inviting scans
altogether; a single empty hopper is a per-person refusal at the moment their
role picks that machine.

#### When a call happens

The inputs are read every `Config.STATUS_POLL_SEC` (2 s) by the idle loop, and
again — fresh, uncached — when an issuing run starts and before each attempt
that needs a card from the hopper. **Reads are not reports.** A call goes out
only on:

1. **An edge.** The condition appeared (`status=active`) or went away
   (`status=cleared`).
2. **A repeat.** While a condition is still present it is re-sent every
   `Config.STATUS_REPEAT_SEC` (300 s), so an alert lost to a server outage or
   a ticket closed by mistake comes back on its own.

So a full reject bin produces one `active`, a repeat every five minutes while it
stays full, and one `cleared` when it is emptied — not a call per poll.

#### Semantics your handler must assume

- **It is a state assertion, not an event.** There is no event id, sequence
  number or timestamp in the form. Key on `(machine_id, machine, issue)` — the
  dispenser is part of the key, or the contractor hopper's `cleared` will close
  the staff hopper's alert — store `status` as the current state, and stamp your
  own receipt time. Repeats are expected and must be idempotent: do not open a
  new ticket per call.
- **`active` can repeat after a gateway restart.** The edge state lives in the
  gateway's memory, so a restart with the bin still full re-sends `active`.
- **A condition that clears while the gateway is down is never reported.** You
  will simply stop receiving repeats. If you alert on these, treat "no repeat
  within a few multiples of 300 s" as stale rather than as still-active.
- **Silence is not `cleared`.** When the PLC read itself fails (unreachable
  PLC), the flag is *unknown* and nothing is sent — deliberately, so a network
  fault cannot be mistaken for a hopper that refilled itself.
- **No queue, no catch-up.** A failed report is logged locally and dropped; the
  next attempt is the 300 s repeat. Do not expect a backlog to arrive after an
  outage.

#### Response

Acceptance here is laxer than the other three endpoints: a 2xx with no JSON, or
with no `success` field, counts as delivered. Only a transport failure or an
explicit `success: false` is treated as not accepted (and only affects a log
line — the machine's behaviour never depends on your answer).

`200 OK` with an empty body is a perfectly good implementation.

#### Worked example

```
POST /api/card/status/ HTTP/1.1
Host: registration.example.com:8091
API-Key: <rest.api_key>
Content-Type: multipart/form-data; boundary=----abc123

------abc123
Content-Disposition: form-data; name="machine_id"

GW001
------abc123
Content-Disposition: form-data; name="machine"

contractor
------abc123
Content-Disposition: form-data; name="issue"

card_reject_full
------abc123
Content-Disposition: form-data; name="status"

active
------abc123
Content-Disposition: form-data; name="address"

1030
------abc123
Content-Disposition: form-data; name="message"

Contractor: Card reject bin is full (input 1030)
------abc123--
```

```json
HTTP/1.1 200 OK
{ "success": true }
```

Implemented by `config/scripts/machine.lua` (detection, edge/repeat logic) and
`RestApi.ReportCardStatus` in `config/scripts/rest.lua` (the call itself). Every
condition is also written to the gateway's own system error log, so the machine
keeps a record even when the server never hears about it.

## 1.4 Health probe

Every 15 seconds the gateway issues a plain `GET` to the configured base URL
(no path appended) with the `API-Key` header, purely to keep the dashboard's
REST indicator truthful on a gateway whose script makes no calls.

- Success here is **2xx or 3xx** — laxer than the business calls.
- Nothing is done with the body.
- Answer it cheaply. A 404 from your router still counts as failure and will
  show the link as down, so give the base URL *something* that returns 200.

## 1.5 Worked example

```
POST /api/citizen/verify HTTP/1.1
Host: registration.example.com:8091
API-Key: <rest.api_key>
Content-Type: multipart/form-data; boundary=----abc123

------abc123
Content-Disposition: form-data; name="citizen_id"

012345678901
------abc123
Content-Disposition: form-data; name="citizen_name"

NGUYEN VAN A
------abc123
Content-Disposition: form-data; name="machine_id"

GW001
------abc123--
```

Accept:

```json
HTTP/1.1 200 OK
{ "success": true, "message": "OK" }
```

Reject, employee unknown:

```json
HTTP/1.1 200 OK
{ "success": false, "code": "not_registered", "message": "Nhân viên chưa đăng ký" }
```

Reject, account disabled — note `code` carries a pattern word so the machine
shows the right text:

```json
HTTP/1.1 200 OK
{ "success": false, "code": "account_inactive", "message": "Tài khoản đã bị khóa" }
```

Test either direction without hardware from the gateway's **Test Tools → REST
API Client** page, which posts arbitrary multipart forms to any URL.

---

# Part 2 — Inbound: the gateway's own REST API

Served by the gateway itself, default `http://<gateway>:8080` (`web.port`,
`web.bind_address`).

## 2.1 Conventions

- All responses are JSON. `Content-Type` is set for static assets; API routes
  return a JSON body regardless.
- Success/failure is reported two different ways depending on the route's age:
  most return `{"ok": true|false, "error": "…"}`, the card endpoints return
  `{"success": …, "code": …, "message": …}`. Both shapes are documented per
  route below.
- Errors carry `{"error": "…"}` with a 4xx/5xx status.

## 2.2 Authentication — read this before exposing the gateway

**There is none on `/api/*`.** The login page and the Admin/Client roles are
enforced **in the browser only** (`web/js/auth.js` against a hardcoded user
table, session in `localStorage`). Any client that can reach the port can call
every endpoint below, including ones that write Lua scripts to disk, drive PLC
coils and change configuration.

The one exception is `POST /api/card/input`, which requires a per-client
`API-Key`.

Bind to `127.0.0.1`, or put the gateway behind something that authenticates,
until backend enforcement exists (updateUI.md §34).

## 2.3 Status and configuration

### `GET /api/status`

```json
{
  "cpu_percent": 12.5, "ram_used_mb": 812, "ram_total_mb": 16256,
  "disk_used_gb": 210.4, "disk_total_gb": 476.9,
  "network_up": true, "uptime_seconds": 91820, "thread_count": 42,

  "build": { "version": "1.0.0", "commit": "a1b2c3d", "date": "…",
             "platform": "windows-x86", "compiler": "MSVC 19…",
             "zk_enabled": true },

  "rest_ok": true, "serial_open": false, "plc_connected": true,
  "rfid_connected": false, "lua_running": true,

  "connections": {
    "rest":   { "connected": true, "last_connected": 1786442516, "changed_at": 1786442516 },
    "plc":    { … }, "rfid": { … }, "serial": { … }, "lua": { … }
  }
}
```

`last_connected` is unix seconds; `0` means never since boot.

### `GET /api/config` · `POST /api/config`

GET returns the whole configuration document. POST applies a **partial** patch
and saves — send only the sections you are changing:

```json
{ "modbus": { "ip": "192.168.1.12" } }
```

Sections: `system`, `web`, `rest`, `serial`, `serial2`, `modbus`, `rfid`, `zk`,
`lua`, `logging`, `card_api`.

Both methods return the **full configuration document** — POST answers with the
state after the patch, so there is no need to GET again. A value of the wrong
JSON type rejects the whole patch with `400 {"error": "invalid config"}`;
unknown keys are ignored silently, so a typo'd field name looks like success.
Read the response back if it matters.

`rest.api_key` is returned in plain text. Anyone who can GET this endpoint has
your upstream credential.

## 2.4 Logs

Two separate systems — see [§4](#part-4--what-changed-in-this-update).

| Route | Purpose |
|---|---|
| `GET /api/logs?category=<name\|all>&limit=<n>` | Runtime log tail from the in-memory ring buffer. Newest last. Categories: `System` `Rest` `Serial` `Modbus` `Rfid` `Lua` `Card`. Default limit 200 |
| `POST /api/logs/clear` | Empties the in-memory buffer. The log **file** is untouched — a UI button must not be able to destroy an audit trail |
| `GET /api/logs/definitions` | Declared structured log types, their fields, the scripts that have logged, and the total entry count |
| `GET /api/logs/structured?…` | Search structured logs (below) |
| `GET /api/logs/structured/<id>` | One entry with every field, plus `field_order` |

Runtime entry:

```json
{ "seq": 1421, "timestamp": "2026-08-11 17:01:56",
  "level": "INFO", "category": "Lua", "message": "…" }
```

`seq` is monotonic across all categories and is the dedupe key when merging the
REST history with the WebSocket stream — timestamps have 1 ms resolution and
collide.

### `GET /api/logs/structured`

| Parameter | Notes |
|---|---|
| `type` | Log type. Repeatable, or comma-separated. Omit (or `all`) for every type |
| `level` | `DEBUG` `INFO` `WARNING` `ERROR`, or `all` |
| `script` | Exact script name, as reported by `/api/logs/definitions` |
| `keyword` | Substring match against **any field value**, the script name and the log type. `%` and `_` are escaped, so they match literally |
| `from`, `to` | `YYYY-MM-DDTHH:MM` or `YYYY-MM-DD HH:MM[:SS]`, inclusive. A minute-precision `to` is widened to that whole minute, so `23:59` includes `23:59:30` |
| `limit` | Default 50, **capped at 500** |
| `offset` | Default 0 |

```json
{
  "total": 1284, "limit": 50, "offset": 0,
  "entries": [
    { "id": 1284, "timestamp": "2026-08-11 17:01:56",
      "log_type": "card_issue", "script_name": "card/issue.lua", "level": "INFO",
      "fields": { "citizen_id": "012345678901", "result": "success" } }
  ]
}
```

`total` is the match count ignoring `limit`/`offset` — what a pager needs.
Entries are newest first. **Every field value is a string**, whatever type it
was declared as; the declaration is validated on write, not re-typed on read.

## 2.5 Lua runtimes

Several scripts run at once, each in its own runtime *(upgrade.md §20-28)*.

| Route | Notes |
|---|---|
| `GET /api/lua/runtimes` | `{"runtimes": [ … ], "running_count": n}` |
| `GET /api/lua/runtime` | Same list under `scripts`, plus the aggregate `running`/`state`/`last_error` the dashboard's status light uses |
| `POST /api/lua/scripts/run` | Start one (`?name=`) or many (`{"names": [...]}`) |
| `POST /api/lua/scripts/stop` | Stop one or many, same argument shapes |
| `POST /api/lua/stop[?name=]` | Without `name`, stops **everything** |
| `POST /api/lua/restart?name=` | Stop-then-start. Defaults to the editor buffer |
| `POST /api/lua/runtimes/prune` | Forget stopped/failed runtimes. `{"ok": true, "removed": n}` |
| `POST /api/lua/run` | Run source with no file behind it: `{"code": "…"}`. Appears as `(editor buffer)` |
| `POST /api/lua/validate` | Compile-only syntax check: `{"code": "…"}` → `{"ok", "error"}` |

Runtime object:

```json
{ "runtime_id": 3, "name": "card/issue.lua", "path": "C:\\…\\issue.lua",
  "state": "RUNNING", "start_time": 1786442516, "uptime_seconds": 37.0,
  "cpu_percent": 2.4, "memory_kb": 1843.2, "cpu_core": 5,
  "last_error": "", "error_line": 0 }
```

`state` is one of `STOPPED` `STARTING` `RUNNING` `STOPPING` `ERROR`. Stopped
and failed runtimes stay in the list until pruned, so a failure remains visible
next to the scripts still running. `cpu_core` is `-1` when not yet sampled —
that is "unknown", not core −1.

`run` and `stop` report **per script**, because one script failing to start
must not be reported as the batch failing:

```json
{ "ok": false,
  "results": [
    { "name": "a.lua", "ok": true,  "already_running": false, "error": "" },
    { "name": "b.lua", "ok": false, "already_running": true,  "error": "b.lua is already running" },
    { "name": "c.lua", "ok": false, "already_running": false, "error": "c.lua:4: attempt to index a nil value" }
  ] }
```

`already_running` is its own outcome, not a failure *(§27)*. Top-level `ok` is
false only if something genuinely failed to start.

Starting a script blocks up to **1.5 s** waiting for its top-level code to
settle, so a syntax error is reported inline. A script that never returns (an
intentional polling loop) reports success after that timeout and keeps running.

### Script files

| Route | Notes |
|---|---|
| `GET /api/lua/scripts` | Tree of `.lua` files under the scripts directory, each with `name` `size` `state` `runtime_id` `last_error`, plus `default_name` |
| `GET·POST·DELETE /api/lua/scripts/file?name=` | Read / write (`{"code": …}`) / delete one script |
| `POST /api/lua/scripts/rename` | `{"from": …, "to": …}`. Creates missing folders |
| `POST /api/lua/scripts/set-default` · `POST /api/lua/clear-default` | Set/clear the startup script |
| `GET·POST /api/lua/scripts-dir` | Report or change the scripts root. POST validates existence and writability first |
| `GET·POST /api/lua/script` | Legacy single-file route; always targets the current startup script |

`name` travels as a **query parameter**, not a path segment: names may contain
`/` for subfolders, which a Crow `<string>` capture cannot carry. Names are
validated against a strict whitelist (alphanumerics, `_`, `-`, `.`, `/`), must
end `.lua`, and reject `..`, a leading `/` and empty segments — these come
straight off the wire.

## 2.6 PLC / Modbus

| Route | Notes |
|---|---|
| `GET /api/modbus/io` | Registered named points: `{"inputs": [...], "outputs": [...], "registers": [...]}` |
| `POST /api/modbus/output` | Drive a named coil: `{"name": …, "value": true}` |
| `POST /api/modbus/register` | Write a named typed register: `{"name": …, "value": …}` |
| `POST /api/modbus/test` | Connectivity probe: `{"ip", "port"}` → `{"ok", "error"}` |
| `POST /api/test/modbus` | Arbitrary read/write, see below |

Outputs and registers resolve **by name** through the registry a script
registered, deliberately: this is not a "write any coil" primitive. An unknown
name is 404, an unconnected PLC is 503, and an input register (FC04, read-only
in the protocol) is 403.

Input point:

```json
{ "name": "Card Taken", "address": 1024, "value": false, "valid": true,
  "source": "discrete", "function_code": 2 }
```

`valid` is false until the address has been read at least once, so the UI can
show "unknown" rather than a confident OFF. `function_code` records which
address space actually answered — the single most useful field when an input
appears frozen, because coils (FC01) and discrete inputs (FC02) are separate
spaces and many PLCs answer both at one address while only one carries the live
value.

`POST /api/test/modbus` takes `{"ip", "port", "op", "address", "count",
"value"}` where `op` is `read_coils` · `read_discrete_inputs` ·
`read_holding_registers` · `read_input_registers` · `write_coil` ·
`write_register`. Reads return `values` as an array.

## 2.7 Serial

| Route | Notes |
|---|---|
| `GET /api/serial/ports` | `{"ports": ["COM1", …]}` |
| `POST /api/serial/test` | Try opening a candidate port: `{"port", "baudrate", "data_bits", "stop_bits", "parity"}` |
| `POST /api/serial2/led-test` | Write a test string to the LED display: adds `{"text"}` |
| `POST /api/test/serial` | Full round trip: adds `{"data", "hex", "append_cr", "read_ms"}` → `{"ok", "received", …}` |

`read_ms` is clamped to 0–5000. `hex: true` needs an even number of digits.

## 2.8 REST client tools

| Route | Notes |
|---|---|
| `POST /api/rest/test` | Test a candidate upstream config before saving: `{"url", "api_key", "timeout_ms"}` → `{"ok", "error"}` |
| `POST /api/test/rest` | Free-form request, below |

```json
POST /api/test/rest
{ "method": "POST", "url": "https://…", "headers": { "API-Key": "…" },
  "form_fields": { "citizen_id": "012345678901" },
  "body": "", "timeout_ms": 5000, "verify_ssl": true }

→ { "ok": true, "status": 200, "elapsed_ms": 42.1, "headers": "…", "body": "…" }
```

`url` is required (400 otherwise); everything else defaults. `form_fields` and
`body` are mutually exclusive — `form_fields` wins and is sent as multipart,
matching what the readers post.

`headers` and `form_fields` must be objects of **string → string**. A non-string
value is skipped silently, so `{"Position": 1}` sends no `Position` part at all;
quote it.

## 2.9 Other

| Route | Notes |
|---|---|
| `GET /api/variables` | Runtime variables as a flat object of name → value |
| `POST /api/rfid/test` | Probe a reader in a given `mode` |
| `GET·DELETE /api/card/cache` | The single-slot card cache: the cached entry, or `null` |

## 2.10 Card Reader Client API

For external readers (USB, TCP, desktop, mobile) that push UIDs to the gateway
instead of the gateway talking to reader hardware. Full spec:
`request/Request/cardInputRESTAPI.md`.

### `POST /api/card/input`

The **only** authenticated endpoint.

| | |
|---|---|
| Header | `API-Key: <per-client key>` — surrounding whitespace is trimmed |
| Body | `multipart/form-data` |
| `CardData` | the card UID. Required, non-empty |
| `Position` | integer. Required |

```json
200 { "success": true,  "message": "Card Accepted" }
400 { "success": false, "code": 400, "message": "CardData is required" }
401 { "success": false, "code": 401, "message": "Invalid API-Key" }
403 { "success": false, "code": 403, "message": "Card Reader Client API is disabled" }
```

403 is the master switch (`card_api.enabled`), independent of whether your key
is valid. An accepted card lands in a single-slot in-memory cache — **the next
card overwrites it** — and fires `OnCardReceived(uid)` in every running script.

### Client management

| Route | Notes |
|---|---|
| `GET /api/card/clients` | `{"clients": [ … ]}` — **includes `api_key` in plain text** |
| `POST /api/card/clients` | `{"name": "…", "expires_at": 0}` → the created client, with its generated key |
| `POST /api/card/clients/<id>/enable` · `/disable` | |
| `DELETE /api/card/clients/<id>` | |

Client: `{"id", "name", "api_key", "enabled", "created_at", "last_access",
"expires_at"}`. Times are unix seconds; `expires_at: 0` means never.

These management routes are themselves unauthenticated (§2.2) — anyone who can
reach them can mint a working API key or read every existing one.

---

# Part 3 — WebSocket

`ws://<gateway>:8080/ws`. Push-only; inbound messages are ignored. Two frame
types, distinguished by `type`. **Switch on `type`** — new types get added.

## `status` — once per second

```json
{ "type": "status",
  "status":    { …/api/status… },
  "variables": { "PLCConnected": true, … },
  "modbus_io": { "inputs": [...], "outputs": [...], "registers": [...] },
  "lua_runtime": { "running": true, "state": "RUNNING", "running_count": 2,
                   "last_error": "", "scripts": [ …runtime objects… ] },
  "logs": [ …runtime log entries since the previous tick, max 200… ] }
```

`logs` carries **only what is new since the last tick**, and each entry is sent
exactly once. A client that skips a frame loses those lines permanently — if
you implement a pause, freeze the *view* and keep ingesting.

## `lua_runtime` — one frame per runtime per tick

*(upgrade.md §25)*

```json
{ "type": "lua_runtime", "runtime_id": 1234, "script": "card/issue.lua",
  "state": "RUNNING", "cpu_percent": 2.4, "memory_mb": 1.8,
  "cpu_core": 0, "timestamp": 1754890000 }
```

On a fault, `error` and `error_line` are added:

```json
{ "type": "lua_runtime", "runtime_id": 1234, "script": "card/issue.lua",
  "state": "ERROR", "error": "card/issue.lua:145: attempt to call a nil value",
  "error_line": 145, … }
```

Note `memory_mb` here versus `memory_kb` in the REST runtime object. The
`status` frame already carries every runtime under `lua_runtime.scripts`; these
per-runtime frames exist for clients watching one script, and are re-sent every
tick rather than only on change.

---

# Part 4 — What changed in this update

Implements `request/upgrade.md`. Relevant to anyone who has already integrated:

**Two things a server team must act on.**

**1. `role` on the verify reply** — this gateway now drives two dispensers
(staff / contractor) holding different card stock, and
[the `role` field](#role--which-machine-issues-the-card-new) in your
`/api/citizen/verify` response is what picks one. Omit it and every card comes
out of the staff machine.

**2. [`POST /api/card/status/`](#post-apicardstatus--machine-health) is now
wired and sending.** It was previously documented as a specification with
nothing behind it. The gateway now watches three consumable/jam conditions on
each dispenser — inputs 1025–1027 for staff, 1029–1031 for contractor — and
reports every appearance, clearance and 5-minute repeat, tagged with a `machine`
field. Implement the endpoint (a bare `200 OK` is enough) or those alerts are
discarded; the machine itself is unaffected either way, since its behaviour
never depends on the answer.

Nothing else moved. Verify, register, cancel and the base-URL health probe keep
the same paths, the same **request** form fields and the same
`success`/`message` contract — `role` is a new *response* field on verify, and
an absent one just means every card comes from the staff machine. A server built
against the previous gateway keeps working without modification; it simply
cannot reach the contractor dispenser until it starts sending `role`.

**Inbound, backward compatible:**

- `/api/lua/runtime` still answers, and its `scripts` array is unchanged in
  shape — it just may now contain more than one entry, and each entry gained
  `runtime_id`, `path` and `error_line`.
- `/api/lua/scripts/run` still accepts `?name=`; the `{"names": [...]}` form
  and the per-script `results` array are additions.
- `/api/lua/scripts` entries gained `state`, `runtime_id`, `last_error`.
- `/api/logs` is unchanged.
- `/api/config` gained the `system`, `logging` and `zk` sections.

**Inbound, new:**

- `/api/logs/definitions`, `/api/logs/structured`, `/api/logs/structured/<id>`
- `/api/lua/scripts/stop`, `/api/lua/runtimes`, `/api/lua/runtimes/prune`
- `/api/lua/stop` and `/api/lua/restart` now take an optional `?name=`

**WebSocket:** the `status` frame is unchanged apart from `lua_runtime` gaining
`running_count` and more entries in `scripts`. The `lua_runtime` frame type is
new — a client that does not filter on `type` will now see frames it does not
recognise.

**Two log systems, deliberately separate:**

| | Runtime log | Structured log |
|---|---|---|
| Written by | `Log.Info/Warning/Error/Debug` | `Log.Write(type, data)` |
| Stored in | console + rotating file + bounded ring buffer | SQLite (`config/logs.db`) |
| Shape | free-form text | declared fields, validated on write |
| Read via | `GET /api/logs`, WebSocket | `GET /api/logs/structured` |
| Retention | ring buffer ages out; file kept | `logging.retention_days`, default 90 |

Log types are declared in `config/log_definitions.json`. A write is rejected
whole on an unknown type, an undeclared field, a missing required field, or a
value that does not parse as its declared type — which is what makes a
misspelled field a loud error rather than a silently missing column.
