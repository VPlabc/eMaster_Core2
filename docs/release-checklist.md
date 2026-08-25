# Release checklist

Working form of `request/release.md` section 31, with the steps that do not
apply to this project removed and the ones that are automated marked as such.

Copy this into the release PR or issue and tick as you go.

## What is automated

`scripts/package.ps1` / `scripts/package.sh` already do these — no manual step:

- clean build directory (a release is never an incremental build)
- version and Git commit stamped into the binary
- `BUILD_INFO.txt` written from `--version`
- `*.db` and `config.json` refused if they ever reach the staging directory
- `.tar.gz` / `.zip` produced and a SHA-256 line appended to `dist/SHA256SUMS`

`.github/workflows/build.yml` runs the build and Lua syntax check on every
push; `release.yml` builds all platforms and drafts the GitHub Release when a
`v*` tag is pushed.

---

## 1. Source

- [ ] Working tree clean (`git status`) — otherwise the binary is stamped `-dirty`
- [ ] `VERSION` bumped, matching the tag you intend to push
- [ ] `CHANGELOG.md` updated, including **Known issues**
- [ ] No secrets: `git grep -niE "api[_-]?key|password|secret|token" -- . ':!request' ':!docs'`
- [ ] `LICENSE` placeholders (`YOUR_COMPANY_NAME`) replaced
- [ ] No debug/commented-out code in the diff

## 2. Build

- [ ] Windows x86 (`HSF_ENABLE_ZK=ON`) — the only ZK-capable package
- [ ] At least one 64-bit target (`HSF_ENABLE_ZK=OFF`)
- [ ] `--version` reports the expected version, commit and platform
- [ ] No warnings that are new since the last release

## 3. Lua

- [ ] `scripts/validate-lua.sh` passes
- [ ] Each script under `config/scripts/` loads without error
- [ ] Card issuing workflow runs end to end (`request/release.md` section 13)
- [ ] Failure paths: citizen not found, card not detected, PLC error, server
      error, 30-second timeout, card collection, cancellation, max scan retry
- [ ] Script start/stop reports `STOPPED`, not `ERROR`

## 4. Web UI

Both themes, both roles:

- [ ] Login; Admin sees Configuration and Lua Editor, Client does not
- [ ] Dashboard: CPU/RAM chart, connection status with last-connected times
- [ ] PLC Input / Output panels; Admin output toggle
- [ ] PLC Registers: values, Raw column, inline write, out-of-range refusal
- [ ] Lua Editor: open, edit, save, run, stop
- [ ] Logs: filters, search, live tail, pause, clear, download
- [ ] Test Tools: REST client, Modbus client, serial master, card clients
- [ ] WebSocket updates arrive without a refresh

## 5. Hardware

Skip any not present, and say so in the release notes rather than leaving it
ambiguous.

- [ ] PLC over Modbus TCP
- [ ] Citizen ID card reader (Serial1)
- [ ] TDM-800 LED display (Serial2)
- [ ] RFID reader (`tcp_json`)
- [ ] ZKTeco controller (`zk` mode) — **Windows x86 package only**
- [ ] Card Reader Client API

## 6. Package

- [ ] Extract into a **clean directory**, not the development tree
- [ ] `scripts/run.sh` / `run.ps1` starts it there
- [ ] Dashboard reachable, config seeded from the template
- [ ] Package contains no `config.db`, `config.json` or `clients.db`
- [ ] `sha256sum -c SHA256SUMS` passes

This is `request/release.md` section 17 and the step most worth not skipping:
it is the only one that catches a binary still reaching back into the machine
it was built on.

## 7. Git

- [ ] Merged to `main`
- [ ] `git tag -a vX.Y.Z -m "eMaster Gateway vX.Y.Z"`
- [ ] Tag matches `VERSION` and the `--version` output
- [ ] Pushed (`git push origin main && git push origin vX.Y.Z`)
- [ ] GitHub Release created, artifacts and `SHA256SUMS` attached
- [ ] Release notes: highlights, supported platforms, known issues, upgrade
      notes

---

## Not applicable to this project

For anyone reconciling this against `request/release.md`:

- **Section 10, frontend npm build** — the dashboard is static HTML/CSS/ES6
  with no build step. `web/` ships as-is.
- **Sections 12 and 31, automated unit/integration tests** — there is no test
  suite. Validation is this checklist plus `scripts/validate-lua.sh`. Do not
  tick test boxes that were never run.
- **Section 3, repository layout** — the recommended `backend/ frontend/ lua/`
  tree was deliberately not adopted; the existing `src/ include/ web/
  config/scripts/` layout is kept and the packaging scripts map onto it.
