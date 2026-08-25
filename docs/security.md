# API security

Every route on the configuration web port is authenticated and authorized
before its handler runs. This implements Phase 1 of
`request/AdvanceUpdate.md`.

Before this, `web/js/auth.js` checked `admin/admin` in the browser and kept
"the session" in localStorage. It said of itself *"THIS IS NOT SECURITY"*, and
it was right: all 86 API routes were reachable with `curl` by anyone who could
open the port — including the ones that open a physical door.

---

## 1. First start

On an empty `config/security.db` the gateway seeds **one** ADMIN account and
prints its password to the console, once:

```
================================================================
  A new security database was created.
  Sign in at the web UI with:

      username:  admin
      password:  gD9yWUsnsNA2B7nP

  This password is shown ONCE and is not recoverable.
  Change it after signing in.
================================================================
```

There is no default password at any point. The database stores an Argon2id
hash, so a lost password cannot be recovered — see *Recovering a locked-out
gateway* below.

---

## 2. The pipeline

`src/security/SecurityMiddleware.h`, running as Crow middleware on every
request:

```
Request
   -> body size ceiling        413 if over auth.max_body_bytes
   -> rate limit               429 (token bucket, per source IP)
   -> [ auth.enabled == false stops here ]
   -> route policy lookup      src/security/Permissions.cpp
   -> authentication           401 unless a valid bearer token
   -> authorization            403 unless the role holds the permission
   -> handler                  input validation + business logic
   -> audit + security headers
```

Two properties are worth stating explicitly.

**The policy is a table, not 86 checks.** `RoutePolicies()` in
`src/security/Permissions.cpp` is the entire authorization policy, readable top
to bottom. Ninety `if (!authorized) return 403;` lines at the top of ninety
handlers would be ninety chances to forget one, and a forgotten one is
invisible because the route keeps working.

**Unclassified routes fail closed.** A path with no entry in the table gets
`kAuthenticated`. Adding a route without a policy makes it require a session,
which is inconvenient; the alternative would make it public, which is a hole.

---

## 3. Roles

| | ADMIN | OPERATOR | USER |
|---|---|---|---|
| Read status, logs, scripts | yes | yes | yes |
| Read configuration | yes | yes | no |
| Doors, relays, device tests | yes | yes | no |
| Change configuration | yes | no | no |
| Edit / run Lua | yes | no | no |
| Install updates | yes | no | no |
| Manage users, read audit log | yes | no | no |

OPERATOR is the shift-supervisor account: it can do what recovering a stuck
machine takes, and nothing that changes what the machine *is*.

Permission names (`RELAY_CONTROL`, `SYSTEM_CONFIG_WRITE`, …) are the same
strings in the C++ table, the `/api/auth/me` response and the audit log.

---

## 4. Sessions

- Opaque 256-bit random tokens from libsodium, **not JWT**. Revocation and key
  management are the concerns `request/AdvanceUpdate.md` section 1.1 raises
  about JWT, and in a single process with a SQLite store a JWT buys nothing.
- Sent as `Authorization: Bearer <token>`.
- 30 minutes by default (`auth.token_lifetime_sec`).
- Stored **hashed** (SHA-256) in `sessions`. Whoever reads the database at rest
  does not come away with usable credentials. SHA-256 rather than Argon2id
  because a token is 256 bits of real entropy — there is nothing for a slow KDF
  to defend, and this runs on every request.
- Revoked by logout, by a password change, by a role change, by disabling the
  account, and by restarting the gateway.

Sessions do not survive a restart. That is deliberate: a token stolen from a
machine should not outlive the reboot an operator performed *because* something
looked wrong.

### Changing a password

There are two flows, and they are deliberately not interchangeable.

**Your own** — `POST /api/auth/password`, the *Change My Password* card. Needs
the current password, the new one twice, and signs you out everywhere. Each
part earns its place: the current password because a token left on an unlocked
screen must not be enough to lock the real owner out; the confirmation because
the change revokes every session at once, so a typo in a password nobody read
back locks you out of the gateway with nothing to correct it with; the sign-out
because that is the same revocation that makes a stolen token stop working the
moment its owner reacts.

**Someone else's** — `PATCH /api/users/<id>` with `password`, the *Reset
password* button. An admin resetting another account, so it does not ask for
that user's old password.

The second is refused against **your own** account (`400
USE_SELF_SERVICE_PASSWORD_CHANGE`), in the API and not merely in the UI.
Without that it is a second door into the same room with the lock left off: an
admin could change their own password without proving they knew it, which is
exactly what the first flow exists to require.

### Passwords

Argon2id (`crypto_pwhash`), INTERACTIVE profile — about 64 MB and ~0.1 s.
MODERATE wants 256 MB, which on a 512 MB board would mean a login that swaps
the card-reading thread out. Parameters are stored inside each hash, so raising
them later does not invalidate existing accounts.

libsodium is a **required** dependency, unlike amqpcpp, the ZK PullSDK and
OpenSSL, which all degrade to stubs. Those are optional features; with
`auth.enabled` defaulting on, a gateway that cannot hash a password is a
gateway nobody can log into.

---

## 5. Brute force and rate limiting

Five failed attempts on one account, then a 15-minute lockout
(`auth.max_failed_attempts`, `auth.lockout_seconds`). Failures are rows in
SQLite, so the counter survives a reconnect **and a restart of the gateway**.
The lockout is per account, so one attacker cannot lock the fleet out by
hammering `admin`... but they *can* lock out `admin` specifically, which is the
accepted trade for the property that guessing cannot continue indefinitely.

Rate limiting is a token bucket per source IP, in memory:

| Bucket | Default |
|---|---|
| `/api/auth/login` | 0.2/s sustained, burst 10 |
| everything else | 40/s sustained, burst 120 |

The login burst is **higher** than the lockout threshold on purpose. Both defend
password guessing, and whichever trips first is the one the user sees; at burst
5 the limiter answered 429 on the sixth attempt and the lockout never engaged,
hiding the more useful signal. The limiter remains the backstop for what the
lockout cannot see — an attacker rotating usernames, where no single account
reaches five.

`X-Forwarded-For` is **not** consulted. It is trivially forged, and trusting it
would let one attacker present a fresh source per request and walk through both
defences. Behind a real reverse proxy that has to become an explicit,
configured decision.

The bucket map is bounded (4096 keys). Keying by client IP is otherwise itself
a denial of service. When full it sweeps idle buckets, and if that frees
nothing it **allows** the request — a rate limiter that fails closed under
pressure takes the machine down on the attacker's behalf.

---

## 6. Input validation

`src/security/Validation.h`. Backend validation is the boundary; the frontend's
is for the user's benefit only.

Reviewed for injection (section 1.6):

| Class | Finding |
|---|---|
| SQL | Clean. Every store binds parameters. `LogStore`'s dynamic `WHERE` only ever appends fixed `?` placeholders. |
| Path traversal | `IsValidScriptPath` rejected `..`, absolute paths and odd characters, but was purely lexical — a **symlink** inside `scripts/` contains no forbidden characters. Now resolved and checked for containment by `Validate::PathStaysWithin`. |
| Command | `std::system` is reached only from the OTA installer, with a whitelisted version string and quoted paths, and from `update.restart_command`, which is admin configuration. |
| Log injection | CR/LF stripped by `SanitiseForLog` before anything user-supplied reaches a log line. |
| UTF-8 | Overlong encodings and surrogates rejected, the classic way to smuggle `/` or NUL past a naive filter. |

Bodies are capped before parsing (64 KB, 1 MB for Lua): discovering a body was
too big *after* parsing it is the work the attacker wanted done.

---

## 7. HTTP hardening

`Content-Security-Policy`, `X-Content-Type-Options: nosniff`,
`X-Frame-Options: DENY`, `Referrer-Policy: no-referrer`, and `Cache-Control:
no-store` on API responses.

The CSP allows `'unsafe-inline'` and `cdn.jsdelivr.net` because several pages
carry inline `<script>` and load Bootstrap and Chart.js from that CDN.
Tightening it means editing the frontend first; it is honestly permissive
rather than quietly broken.

CORS is an allowlist (`auth.allowed_origins`), never `*`. Empty — the default —
emits no `Access-Control-Allow-Origin` at all, leaving the browser's
same-origin rule in place, which is correct for a UI the gateway serves itself.

**CSRF is not implemented, and does not apply.** Authentication is a bearer
header the page sets explicitly. Browsers do not attach it automatically, so a
cross-site form post carries no credentials (section 1.12.A).

**There is no TLS.** The gateway speaks plain HTTP. Tokens and passwords cross
the network in the clear, so the web port must not be exposed to an untrusted
network — put it behind a VPN or a TLS-terminating reverse proxy. This is the
largest remaining gap in Phase 1.

---

## 8. Audit log

`config/security.db`, table `audit_log`, readable at `/api/audit` and on the
Security page. ADMIN only (`USER_READ`) — not `LOG_READ`, which an operator and
a viewer both hold: this log records who failed to log in, from where and how
often, which is a map of the attack surface rather than an operational log.

Events: `LOGIN_SUCCESS`, `LOGIN_FAILED`, `ACCOUNT_LOCKED`, `LOGOUT`,
`TOKEN_EXPIRED`, `ACCESS_DENIED`, `RATE_LIMITED`, `PASSWORD_CHANGED`,
`USER_CREATED`, `USER_PERMISSION_CHANGED`.

Passwords, tokens and keys are never written to it, which the test suite
asserts by submitting a known password over the API and grepping both logs for
it.

Pruned at startup to `auth.audit_retention_days` (90).

---

## 9. Known gaps

These are real and deliberate; none is hidden behind an option that implies
otherwise.

1. **No TLS.** See above. The single most important thing to put in front of
   this gateway.
2. **The WebSocket token travels in the query string.** The browser WebSocket
   API cannot set an `Authorization` header. Query strings reach proxy logs in
   a way headers do not. Mitigated by short-lived, revocable tokens.
3. **The token lives in `localStorage`.** Script injected into a page could
   read it. This is the standard bearer-token trade; it buys CSRF immunity, and
   is why the CSP exists and why tokens expire in 30 minutes.
4. **The SmartLocker UI on port 8081 is unauthenticated by default.** It is a
   kiosk display — but it can open doors. `auth.protect_locker_ui` turns
   authentication on for it; leaving it off is a statement that its network
   segment is trusted.
5. **`/api/card/input` keeps its own API-Key scheme.** It is already
   authenticated; layering bearer tokens on top would break every deployed
   reader for no gain.
6. **Login timing.** Argon2id runs only when the username exists, so response
   time distinguishes a real account from an unknown one. Closing that means
   hashing a dummy on every miss, at ~64 MB a go — a cheap denial of service
   against a small board. The lockout and rate limiter bound how much an
   attacker can learn from it.
7. **No password policy beyond a 12-character minimum.** Section 1.12.F asks
   for more.

---

## 10. Recovering a locked-out gateway

Passwords are hashed and tokens are not recoverable, so with physical or shell
access to the device:

**Wait out a lockout** — 15 minutes, or clear it:

```bash
sqlite3 /opt/hsf-gateway/data/security.db "DELETE FROM login_failures;"
```

**Start over with a fresh admin** — deleting the user table makes the next
start seed a new account and print a new one-time password:

```bash
systemctl stop hsf-gateway
sqlite3 /opt/hsf-gateway/data/security.db "DELETE FROM users;"
systemctl start hsf-gateway
journalctl -u hsf-gateway | grep -A6 "security database was created"
```

**Last resort — turn authentication off** to get back in, then turn it on again
from the Configuration page:

```bash
sqlite3 /opt/hsf-gateway/data/config.db \
  "UPDATE config SET data = json_set(data, '\$.enabled', json('false')) WHERE section = 'auth';"
```

While it is off, every endpoint on that port is open to anyone who can reach
it. The gateway logs a warning at startup saying so.

---

## 11. Tests

```bash
./scripts/security-test.sh ./build/hsf_gateway 18080
```

Starts a gateway against a throwaway config, reads the one-time password it
prints, and runs 60 black-box checks covering `request/AdvanceUpdate.md`'s
Phase 1 checklist: authentication, RBAC per role, password-change rules
(including that the admin reset route cannot be turned on yourself), session
revocation, injection
and traversal, lockout behaviour across a restart, rate limiting, headers, and
that no secret reaches a log. Runs in CI on every push.

Black-box on purpose: "an unauthenticated caller gets 401" is a property of the
whole pipeline, and a unit test calling `SecurityStore` directly would keep
passing if the middleware stopped being installed.
