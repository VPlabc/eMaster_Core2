# Ubuntu build and remote deployment

Build the gateway for Ubuntu and deploy it to a target server, with health
checks and automatic rollback. This implements Phase 3 of
`request/deloyToUbuntu.md`.

```bash
./scripts/build-ubuntu.sh                                  # or --docker
./scripts/package-ubuntu.sh
./scripts/deploy-ubuntu.sh --host 192.168.3.209 --user ad
```

---

## 1. What this reuses rather than reinvents

Section 3.1 says not to create a second build system where one exists. Three
pieces of §3.9–3.13 were already built for over-the-air updates
(`docs/ota-update.md`) and are used as-is:

| Phase 3 asks for | Already exists |
|---|---|
| `/opt/gateway/releases/` + `current` symlink (§3.9) | `scripts/install-ota.sh` creates exactly this at `/opt/hsf-gateway` |
| systemd unit (§3.10) | `deploy/hsf-gateway.service` |
| Rollback by repointing `current` (§3.13) | `UpdateInstaller` + the same symlink |

So `deploy-ubuntu.sh` **delegates the install step to `install-ota.sh`**. One
layout, one rollback mechanism. Two would eventually disagree about which
release is live, and the disagreement would surface during an outage.

The service is `hsf-gateway`, not `gateway` as the spec's example names it —
the unit already existed under that name and renaming it would orphan the
installs that have it.

---

## 2. Building

### On Ubuntu

```bash
./scripts/build-ubuntu.sh
```

Checks the compiler (g++ 9+, for usable C++17 `<filesystem>`), bootstraps
vcpkg, configures **Release**, builds, verifies the binary runs, and runs both
test suites.

### Anywhere else

```bash
./scripts/build-ubuntu.sh --docker
```

Builds inside `ubuntu:22.04` with the repo mounted, so artifacts land in the
working tree. Needed on the Windows development machine, which has no Linux
toolchain. A container gives the exact userspace the target runs, so "it built"
says something about the target rather than about the build host.

vcpkg's binary cache is mounted at `.vcpkg-cache/`, or every run rebuilds every
dependency from source (~20 minutes).

### Flags

`-O2 -DNDEBUG` via `CMAKE_BUILD_TYPE=Release`. **No `-march=native`** — §3.3 is
explicit, and a binary tuned for the builder faults with an illegal instruction
on an older target CPU, which looks like a corrupted deployment rather than a
build mistake.

`HSF_ENABLE_ZK=OFF`: the ZKTeco PullSDK is a 32-bit Windows DLL. Every `zk.*`
call returns `NotSupportedOnThisPlatform`; all other hardware works.

`build-ubuntu/BUILD_INFO.txt` records compiler, C++ standard, architecture,
build type, git commit, version and the vcpkg dependency list (§3.3).

---

## 3. Packaging

```bash
./scripts/package-ubuntu.sh
```

A thin layer over `scripts/package.sh` — the same script the tagged-release
pipeline uses on every platform — plus the Ubuntu-specific parts:

- **`dist/<name>.dependencies.txt`** — `ldd` output, anything unresolved, and
  the apt package owning each resolved library (§3.4).
- **`dist/<name>.manifest.txt`** — SHA-256, size, git commit, build host.
- **A contents check** that fails the package if it contains `.git/`, `*.db`,
  `config.json`, `*.key`, `*.pem`, `*.pdb` or `src/` (§3.5).

It deliberately does **not** copy libraries out of `/lib`. §3.4 says not to, and
a glibc or libstdc++ lifted from the build host is the classic way to ship
something that segfaults on a slightly older target.

The target needs only:

```bash
sudo apt-get install -y libstdc++6 libgcc-s1 zlib1g ca-certificates
```

Everything else — curl, Lua, SQLite, Crow, Asio, AMQP-CPP, OpenSSL, libsodium —
is built by vcpkg and shipped in the package. Serial hardware additionally
needs the service user in the `dialout` group.

---

## 4. Deploying

```bash
./scripts/deploy-ubuntu.sh --host 192.168.3.209 --user ad
./scripts/deploy-ubuntu.sh --host 192.168.3.209 --user ad --dry-run
```

| Step | |
|---|---|
| 1 | Verify SSH key auth; refuse to continue without it |
| 2 | Build and package (skip with `--skip-build`) |
| 3 | Check the package architecture matches the target's |
| 4 | Upload via rsync (scp fallback) |
| 5 | Re-checksum on the target and compare |
| 6 | Record which release is currently live |
| 7 | Install via `install-ota.sh` into `releases/<version>/` |
| 8 | `systemctl restart` |
| 9 | Poll `/api/health` from the target's loopback |
| 10 | On failure: repoint `current` at the previous release and restart |
| 11 | Write a deployment log |

`--dry-run` stops before anything is uploaded or changed.

### Passwords

**None is accepted anywhere** — not as an argument, not from a file, not from
the environment. `BatchMode=yes` makes ssh fail rather than prompt, and
`--password` is explicitly rejected with a message. §3.15 made structural:
there is no code path that could put a credential into shell history, a process
listing, or this repository.

```bash
ssh-keygen -t ed25519
ssh-copy-id ad@192.168.3.209
```

### sudo on the target

Installing into `/opt`, writing the unit and restarting the service need root.
The script uses `sudo -n` (never prompts) and tells you what to grant if it
fails. Minimum, in `/etc/sudoers.d/hsf-deploy`:

```
ad ALL=(root) NOPASSWD: /bin/bash /opt/hsf-gateway/releases/*/scripts/install-ota.sh, \
                        /bin/systemctl * hsf-gateway, /bin/ln, /bin/mv
```

### The health port is read, not guessed

From the target's own `data/config.db` (`web.port`), per §3.12, falling back to
8080 only when there is no config yet.

---

## 5. Health check

`GET /api/health` — **public**, because a deploy script and a systemd check
both run before any credential exists on the machine.

```json
{ "status": "ok", "version": "1.0.0", "uptime_seconds": 42 }
```

200 healthy, 503 not. It answers liveness, version and uptime and **nothing
else** (§3.11's "do not expose sensitive information"): no configuration, no
addresses, no connection state. `/api/status` has all of that and stays behind
`DEVICE_READ`.

Uptime is the **process's**, from its own start time — not the host's, and not
`SystemMonitor::Sample()`, which holds the CPU-delta baseline the dashboard
reads and would be corrupted by an unauthenticated caller polling it.

Hardware is deliberately **not** part of the verdict. A PLC that is switched
off is a normal Monday; failing a deployment for it would roll back a perfectly
good release.

---

## 6. Rollback

Automatic when the health check fails: `current` is repointed at the previous
release (relative symlink, atomic replace — the same form `install-ota.sh` and
`UpdateInstaller` write) and the service restarted. The previous release is
never deleted before the new one passes.

The log distinguishes three outcomes, because they need different responses:

- rolled back, previous is healthy — you have a working gateway, investigate at leisure;
- rolled back, previous is **not** healthy — the machine is down, go now;
- rollback impossible (first install) — there is nothing to return to.

Manual:

```bash
ssh ad@192.168.3.209
ls /opt/hsf-gateway/releases/
sudo ln -sfn releases/1.0.0 /opt/hsf-gateway/current.new
sudo mv -T /opt/hsf-gateway/current.new /opt/hsf-gateway/current
sudo systemctl restart hsf-gateway
```

---

## 7. Verification (§3.12)

```bash
ssh ad@192.168.3.209
systemctl status hsf-gateway
journalctl -u hsf-gateway -n 100 --no-pager
ss -lntup | grep hsf
curl -s http://127.0.0.1:8080/api/health
```

First sign-in: the gateway prints a one-time admin password to its journal on
first start. `journalctl -u hsf-gateway | grep -A6 "security database was
created"`. See `docs/security.md`.

---

## 8. Deployment log (§3.14)

`dist/deploy-logs/deploy-<timestamp>-<host>.log`:

```
Version:        1.0.0
Git commit:     fdebe5c
Target:         ad@192.168.3.209:22
Target OS:      Ubuntu 22.04.4 LTS (x86_64)
Package SHA256: ...
Previous:       1.0.0
Now live:       1.0.1
Health check:   PASS (port 8080)
Rollback:       not needed
Result:         SUCCESS
```

No credential is recorded, because none exists in the process to record.

---

## 9. Prerequisites checklist

On the **build machine**:

- [ ] Docker running (`--docker`), or Ubuntu with g++ 9+
- [ ] `ssh`, and `rsync` if you want incremental upload

On the **target**:

- [ ] SSH public key in `~ad/.ssh/authorized_keys`
- [ ] `sudo -n` for the installer and systemctl (section 4)
- [ ] `libstdc++6 libgcc-s1 zlib1g ca-certificates`
- [ ] `curl` (the health check runs on the target)
- [ ] `sqlite3` if you want the port read from config rather than defaulted
