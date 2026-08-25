# Over-the-air updates

The gateway can check a release server for a newer build, show it to whoever is
looking at the web UI, and — once someone clicks **Update now** — download,
verify, install and restart into it, rolling back on its own if the new release
cannot get through a boot.

This implements `request/CICD.md`. Two places deviate from that sketch, both
noted below: the UI sends its actions over REST rather than up the WebSocket,
and the install model is `releases/` + a `current` symlink rather than A/B
slots.

**Nothing installs by itself.** The check runs on a timer; the result is a
notification. Even a `mandatory` release only loses its *Later* button — it
still waits for the click, because this gateway may be holding a locker door
open for someone standing in front of it.

---

## 1. The four pieces

| Piece | Where |
|---|---|
| Release pipeline | `.github/workflows/release.yml`, `scripts/package.sh`, `scripts/make-manifest.sh` |
| Manifest + packages | Wherever you publish them (GitHub Releases by default) |
| Gateway update client | `src/update/`, config section `update` |
| Update popup | `web/js/update.js`, styles in `web/css/app.css` |

```
  git tag v1.2.3
        |
        v
  release.yml ── package.sh per platform ── make-manifest.sh (sha256 + sign)
        |
        v
  version.json + HSF-Gateway-v1.2.3-<platform>.tar.gz
        |
        |  HTTPS, every 6h
        v
  UpdateManager ── check ── download ── sha256 ── signature ── install ── restart
        |                                                          |
        |  WebSocket {"type":"update"}                             |  exit 75
        v                                                          v
  update.js popup                                            systemd restarts
```

---

## 2. Install layout

`scripts/install-ota.sh` creates it; `UpdateInstaller` maintains it.

```
/opt/hsf-gateway/
├── current -> releases/1.2.3     symlink (POSIX) / junction (Windows)
├── releases/
│   ├── 1.2.2/                    bin/ web/ config/ docs/ deploy/
│   └── 1.2.3/
├── data/                         config.db, clients.db, logs.db, logs/, public key
├── downloads/                    scratch, safe to delete
└── update-state.json             boot trial state
```

The split between `releases/` and `data/` is the load-bearing part. A release
directory is replaced wholesale on every update, so anything the *installation*
owns has to live outside one. That is why the service runs:

```
/opt/hsf-gateway/current/bin/hsf_gateway /opt/hsf-gateway/data/config.db
```

The config path argument is **not optional here**. `main()` derives the log
directory, `clients.db`, `logs.db` and `smartlocker.db` from the config file's
directory, so this one argument keeps every piece of state out of the tree the
next update deletes.

Updating swaps the link, not the files in place. On POSIX the new link is built
under a temporary name and `rename(2)`d over the old one, which is atomic — a
reader sees either the old release or the new one, never neither. Windows has
no atomic replace for a junction, so there is a brief window where `current`
does not exist; a restart landing exactly in it fails to start and is retried
by the service manager. That was judged better than requiring Administrator,
which is what real symlinks on Windows would cost.

**Why not A/B slots.** `request/CICD.md` recommends them. Two named slots buy
you a guaranteed-present fallback with a fixed disk budget, at the cost of a
supervisor outside the binary that knows which slot to boot. Versioned release
directories give the same rollback with the same one-link switch, keep more
than one fallback when there is room for it, and need nothing outside the
process — which matters, because the only thing supervising this gateway is
systemd with `Restart=always`.

---

## 3. Signing keys

Generate once, keep the private half in the release pipeline's secret store and
nowhere else:

```bash
openssl genpkey -algorithm ed25519 -out update-signing-key.pem
openssl pkey -in update-signing-key.pem -pubout -out update_public_key.pem
```

- Private key → repository secret `UPDATE_SIGNING_KEY` (paste the whole PEM).
- Public key → every device, at `<root>/data/update_public_key.pem`
  (`install-ota.sh --public-key update_public_key.pem` puts it there).

Ed25519 is the default. RSA and EC keys also work — `SignatureVerifier` branches
on the key type, using SHA-256 for those and the pure one-shot form for EdDSA.

### What is signed

Not the archive, and not the whole manifest. The signed message is exactly:

```
<version>\n<platform>\n<sha256-hex>\n
```

Three fields, newline separated, trailing newline included.
`SignatureVerifier::BuildSignedMessage` and `sign_message` in
`scripts/make-manifest.sh` must produce this byte for byte — **if you change one,
change the other**.

Signing the digest instead of the multi-megabyte archive keeps verification a
fixed-cost operation on an ARM board, and the digest still binds the archive
byte for byte. The version and platform are in there because a signature over a
bare digest is replayable: whoever can answer the update check could otherwise
serve a genuinely-signed *older* package and roll the fleet back onto a known
hole. `UpdateManager` separately refuses anything not strictly newer than what
is running, so a downgrade needs both defences to fail.

> A footgun worth knowing about: bash `$(...)` strips trailing newlines. Build
> the signed message inside the signing function from its three arguments, never
> pass it in through a command substitution — that drops the final byte and every
> package fails on the device with "signature does not match" after a clean
> download and a matching checksum.

### Builds without OpenSSL

`find_package(OpenSSL)` is QUIET. Without it, `SignatureVerifier_stub.cpp` is
compiled and **every** signature is rejected. That is deliberate: treating
"cannot verify" as "verified" would turn a missing build dependency into remote
code execution on a device that downloads its own updates. Such a build can
still be updated by hand, and can be updated over the air only if someone
explicitly turns `update.require_signature` off. The Configuration page says so
in as many words when it detects the combination.

---

## 4. Manifest format

Three shapes are accepted, because all three are things a release pipeline
reasonably produces.

**A. One static file for every platform of a tag** — what `make-manifest.sh`
writes, and the recommended one:

```json
{
  "version": "1.2.3",
  "channel": "stable",
  "release_date": "2026-08-20T14:00:00Z",
  "mandatory": false,
  "min_version": "1.1.0",
  "release_notes": ["Improve serial performance", "Fix RabbitMQ reconnect"],
  "platforms": {
    "linux-x64": {
      "platform": "linux-x64",
      "url": "https://.../HSF-Gateway-v1.2.3-linux-x64.tar.gz",
      "sha256": "…",
      "size_bytes": 41234567,
      "signature": "…base64…"
    }
  }
}
```

Fields the per-platform entry omits are inherited from the top level. The
platform key must match `HSF_PLATFORM` as CMake computes it — `linux-x64`,
`linux-arm64`, `windows-x86`, `windows-x64`, `macos-arm64`.

**B. A single-release manifest** — the same object without the `platforms` map,
using `url` or `download_url`. A relative `url` resolves against the manifest's
own address, so one file works on whatever host it lands on.

**C. A per-device endpoint** — anything answering the query the gateway sends:

```
GET <update.url>?platform=linux-arm64&channel=stable&version=1.2.2&device_id=GW001
```

Answer with `{"available": false}` for "nothing newer", or the shape above.
`device_id` is `system.machine_id`, which is what a staged rollout would key on.

`mandatory` removes the *Later* button. `min_version` escalates to mandatory
when the running version is below it — the pipeline saying "anything older than
this must not stay in the field", normally a security fix. Neither installs
anything by itself.

---

## 5. Boot trial and rollback

`update-state.json` is what makes rollback work with no agent outside the
process:

```json
{ "pending": "1.2.3", "previous": "1.2.2", "attempts": 1,
  "installed_at": "…", "last_result": "" }
```

- **Install** writes `pending` and `previous`, then restarts.
- **Every boot** of `pending` increments `attempts`. This runs in `main()`
  *before any hardware is touched* — and it has to: a release that dies while
  opening the serial port never reaches the update thread, so a counter
  incremented later would sit at zero forever and the gateway would crash-loop
  on the bad version with the rollback one line below the crash.
- **`health_confirm_sec` of uptime** clears `pending` and prunes old releases.
  Late, on purpose: the attempt is recorded as early as a boot can be observed
  and cleared as late as one can be trusted.
- **`attempts > max_boot_attempts`** repoints `current` at `previous`, records
  `rolled-back:<version>`, and exits 75 so the service manager starts the old
  release. The next boot sees no `pending` and runs normally; the web UI shows
  what happened via `rolled_back_from`.

A release whose directory name disagrees with its binary's `HSF_VERSION` is
neither confirmed nor rolled back — the trial is abandoned with a warning, since
the gateway cannot tell what it is actually running. `release.yml` already gates
the tag against `VERSION`, so the two agree for anything the pipeline built.

### Exit code 75

Chosen as `EX_TEMPFAIL`, meaning "stop me and start me again" as distinct from
0's "I am done". `Restart=always` in the unit covers both, plus a crash — and a
crash *is* how a bad release gets counted against itself, so do not narrow that
to `on-failure`.

---

## 6. Setting it up

### Once per project

1. Generate the key pair (section 3) and add `UPDATE_SIGNING_KEY`.
2. Decide where packages and `version.json` are served from. The workflow
   defaults to the GitHub Release for the tag.

### Once per device

```bash
sudo ./scripts/install-ota.sh \
  --package HSF-Gateway-v1.0.0-linux-arm64.tar.gz \
  --root /opt/hsf-gateway \
  --user hsf \
  --public-key update_public_key.pem \
  --service
sudo systemctl start hsf-gateway
```

Then, on the Configuration page under **Software Update**: set the server URL
and tick **Enabled**. Re-running the installer to reinstall never overwrites
anything already in `data/`.

### Cutting a release

```bash
# bump VERSION, add a "## [1.2.3]" section to CHANGELOG.md, commit
git tag v1.2.3 && git push origin v1.2.3
```

`release.yml` verifies the tag against `VERSION`, requires the CHANGELOG entry,
builds every platform, generates and signs `version.json`, and creates a
**draft** release. Devices see nothing until a human publishes it.

---

## 7. Configuration reference

All under `update` in `config.db`, editable on the Configuration page or from
Lua via `Config.Get("update.enabled")`.

| Key | Default | Notes |
|---|---|---|
| `enabled` | `false` | Off so an upgraded fleet does not start calling an unconfigured host |
| `url` | `""` | Manifest endpoint |
| `channel` | `stable` | Passed through to the server |
| `check_interval_hours` | `6` | |
| `initial_delay_sec` | `30` | Grace so the first check does not compete with startup |
| `require_signature` | `true` | Leave on |
| `public_key_path` | `update_public_key.pem` | Relative to the config directory |
| `ssl_verify` | `true` | |
| `timeout_sec` | `30` | Connect timeout, and the stall timeout for a download |
| `install_root` | `""` | Empty means infer from the executable path |
| `health_confirm_sec` | `120` | Uptime before a release counts as good |
| `max_boot_attempts` | `2` | Failed starts before rolling back |
| `keep_releases` | `2` | Beyond the active one and the rollback target |
| `restart_command` | `""` | Empty exits 75 and lets the service manager restart |

---

## 8. REST and WebSocket API

| Endpoint | Purpose |
|---|---|
| `GET /api/update/status` | Full state, including why an unmanaged install cannot update |
| `POST /api/update/check` | Check now; 202, the answer arrives over the socket |
| `POST /api/update/install` | `{"version":"1.2.3"}` — must match the offered version |
| `POST /api/update/dismiss` | `{"version":"1.2.3"}` — refused for a mandatory update |

State is pushed as `{"type":"update", …}` on the existing `/ws`, coalesced to
one frame per second by the broadcast loop.

`request/CICD.md` drew the *actions* going up the socket too. They are POSTs
because `/ws` has always been push-only, and a POST gives the button a
synchronous answer when a request is refused ("no manifest for that version",
"already in progress") instead of silence.

An install request naming a version the last check did not return is rejected.
Without that, a stale browser tab — or anything else that can POST — could name
an arbitrary version and have the gateway go looking for it.

> **Authentication.** These endpoints are as unauthenticated as every other
> `/api/*` route on this gateway (`web/js/auth.js` is frontend-only; see
> `request/updateUI.md` section 34). Anyone who can reach port 8080 can trigger
> an install. What they *cannot* do is choose what gets installed: it must be a
> version the configured server offered, and it must carry a valid signature.
> Do not expose port 8080 to an untrusted network.

---

## 9. Verifying a change to this code

There is no test suite in this repo, so the checks below were run by hand and
are worth repeating after touching `src/update/`.

- **SHA-256** against the four published FIPS 180-4 vectors plus the
  one-million-`a` case and the 55/56/64-byte padding boundaries.
- **Version precedence** including prerelease ordering, and that malformed or
  hostile strings (`""`, `../../etc`, an overflowing major, trailing shell
  metacharacters) are never "newer".
- **Signature round trip**: sign with `make-manifest.sh`, verify with
  `SignatureVerifier`, and confirm that substituting the digest, the version,
  the platform, the key, or a byte of the signature each fails.
- **A real install** into a scratch managed tree, served by a local HTTP
  server: download → checksum → signature → unpack → seed `data/` → swap
  `current` → exit 75.
- **Boot trial**: a healthy boot clears `pending`; `attempts` past the limit
  repoints `current` at `previous` and exits 75; the boot after that is normal.
