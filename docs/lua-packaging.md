# Production Lua packaging

Compiles a Lua application to bytecode, encrypts and signs it, and runs it
entirely from memory — no `.lua` source needs to exist on the device. This
implements Phase 2 of `request/AdvanceUpdate.md`.

Phase 1 (`docs/security.md`) is a prerequisite: `LUA_COMPILE`, `LUA_DEPLOY` and
`LUA_ROLLBACK` are separate permissions, and without authentication they would
guard nothing.

---

## 1. What this does and does not protect

Read this before deciding what the feature is worth to you.

| | |
|---|---|
| **Signature** | Strong. Only packages built with your signing key will run. The secret key never has to leave the build machine, so this holds even against someone who has extracted everything from a device. |
| **Encryption** | Situational. The device must decrypt to run, so the key is on the device. It protects the artifact everywhere the key is *not* — build server, release bucket, backup, email, a stolen SD card — and not at all against root on the gateway. |
| **Bytecode** | Weak on its own, and the spec says so: it is packaging, not encryption. It removes readable source and raises the cost of reading the logic; a determined reverse engineer has tools. |

If you take one thing from this page: **the signature is the control that
matters**. Encryption raises the bar; the signature sets it.

---

## 2. The pipeline

```
   .lua sources
        |  compile with the gateway's OWN interpreter
        v
   bytecode per module  ──►  bundle (entry + every require()d module)
        |  XChaCha20-Poly1305, header as AAD
        v
   ciphertext  ──►  Ed25519 signature over header+ciphertext
        v
   demoapp-1.4.0.pkg
```

Build & Test runs five stages and reports which one failed:

| Stage | What it proves |
|---|---|
| `compile` | Every module parses. Syntax errors name the file and line. |
| `sign` | Keys are present and usable. |
| `verify` | The artifact opens: signature verifies, payload decrypts, hash matches. |
| `load` | Every compiled module is accepted by **this** interpreter. |
| `write` | The `.pkg` is on disk. |

The `verify` and `load` stages are the point of calling it Build & *Test*. A
package that compiles but cannot be loaded by the target runtime is exactly the
failure you would otherwise discover on the device, at deploy time, with the
doors locked.

### Why the compiler is in-process

Not `luac`. Lua bytecode is tied to the interpreter version, integer and float
widths, and pointer size — the loader rejects a mismatch outright. A `luac` on
the build machine may be 5.3, or 64-bit where the gateway is 32-bit, and the
failure would surface as "bad binary format" on the device. Dumping through the
interpreter that will later load it makes that impossible.

The runtime tag is recorded in every package (e.g. `lua5.5-i8-n8-p4`) and
checked before the signature, so a package for the wrong runtime says exactly
that instead of "invalid signature".

> The embedded interpreter is currently **Lua 5.5** (vcpkg's `lua` port), not
> 5.4 as some older comments in this repo say. Bytecode is not portable between
> them.

---

## 3. Modules and `require()`

A package holds **every** module, not just the entry chunk. On load they go
into `package.preload`, and `package.path`/`package.cpath` are **emptied**, so
`require()` can only resolve in-bundle.

Emptying the path is not tidiness. Without it, a module missing from the bundle
— or named wrongly when built — falls through to the filesystem and quietly
loads the plaintext `.lua` next door. The package then works perfectly on the
build machine and fails on the first device that does not have the sources.

**That bug happened here.** Module names were briefly computed relative to the
application directory instead of the scripts directory, so an app that calls
`require("demoapp.greeting")` got a bundle containing `greeting`. Every
`require()` missed preload, fell through to disk, and the tests passed —
until the source tree was moved away. Hence both the empty path and the
move-the-sources-away test in `scripts/package-lua-test.sh`.

### Naming

Module names are relative to the **scripts directory**, matching what
`package.path` would have resolved:

| File | Module |
|---|---|
| `scripts/smartlocker/main.lua` | `smartlocker.main` |
| `scripts/smartlocker/locker.lua` | `smartlocker.locker` |
| `scripts/smartlocker/util/init.lua` | `smartlocker.util` |

The **entry** is named relative to the *application* directory, because the app
root is the bundle root: for `scripts/demoapp/main.lua`, entry is `main.lua`.

Files under a `test/` directory or ending `_test.lua` are excluded — test
fixtures, and whatever credentials they hard-code for a rig, do not belong in a
production artifact.

---

## 4. Keys

Three, created in `config/` (or the OTA install's `data/`):

| File | What | Where it belongs |
|---|---|---|
| `lua_signing.key` | Ed25519 secret, 64 bytes | **Build machines only** |
| `lua_signing.pub` | Ed25519 public, 32 bytes | Every device |
| `lua_package.key` | XChaCha20-Poly1305, 32 bytes | Every device, and build machines |

Create a signing key with `POST /api/lua/packages/keys` (admin). It refuses to
overwrite an existing one, and it is explicit rather than automatic on first
build: a keypair is an identity, and generating one silently would give every
gateway in a fleet a different one, each rejecting the others' artifacts with
"signature does not verify" — which reads exactly like an attack.

A gateway that only *runs* packages should never hold `lua_signing.key`.

Key files are hex on one line, `0600` where the platform has permissions. On
Windows there are no POSIX modes and the directory ACL is what protects them.

### Fleet key or per-device key?

`lua_package.key` is just a file, so this is your call:

- **One key across the fleet** — one artifact deploys everywhere; one
  compromised device exposes every artifact.
- **Per-device key** — an artifact must be built per device; a compromise is
  contained.

Neither changes the signature guarantee.

### Why XChaCha20-Poly1305 and not AES-256-GCM

Section 2.7 suggests AES-256-GCM. libsodium exposes it **only where the CPU has
AES-NI**, and `crypto_aead_aes256gcm_is_available()` is false on most ARM
boards — precisely the `linux-arm64` target. A cipher that silently is not
there on the deployment hardware is worse than a different one that always is.
XChaCha20-Poly1305 is constant-time in software everywhere and its 192-bit
nonce can be random with no counter to persist. The algorithm name is in the
header, so adding AES-GCM later is a new value, not a new format.

---

## 5. Package format

```
offset  size   field
0       8      magic "HSFLUAP\0"
8       2      format version
10      4      header length H
14      H      header JSON, PLAINTEXT
14+H    8      ciphertext length C
22+H    C      XChaCha20-Poly1305 ciphertext + tag
22+H+C  64     Ed25519 signature over bytes [0, 22+H+C)
```

The header is plaintext on purpose: deciding whether a package *can* run here —
right version, right runtime, right application — must be possible before
decrypting. It carries nothing secret, it is passed as AEAD additional data,
and the signature covers it, so it cannot be edited to make a package claim a
version it is not.

Verification order is fixed: parse header → check runtime tag → **verify
signature** → decrypt. Decrypting first would run the cipher over
attacker-chosen bytes before establishing they came from you.

---

## 6. Using it

**Lua Editor → Production Package.**

1. Application folder (`smartlocker`), entry (`main.lua`), id, version.
2. **Compile** — diagnostics only, writes nothing.
3. **Build & Test** — produces `smartlocker-1.4.0.pkg`.
4. **Deploy** — verifies, repoints, restarts the application immediately.
5. **Roll back** — returns to the previous package.

A deployed package **wins over `lua.script_path`** at startup. Both are "what
this gateway runs", and starting the plaintext script alongside a deployed
artifact would run the business logic twice — two state machines fighting over
the same doors.

### Versioning and rollback

`config/lua_packages/` holds the artifacts and `state.json` names the active
and previous ones — the same shape as the OTA installer's `releases/` +
`current`, and for the same reason: rollback is repointing a name, not
reconstructing anything. Neither the deployed package nor the rollback target
can be deleted.

Deploy and rollback report `start_error` when the package was activated but did
not start. That is deliberately not silent: the gateway is then running
nothing, and the UI says so in red rather than green.

---

## 7. Trade-offs you are accepting

1. **Stripped bytecode means no line numbers.** A runtime error from a deployed
   package reports the module but not the line. Reproduce against source.
2. **The encryption key is on the device.** See section 1.
3. **No behavioural testing.** Build & Test proves the artifact *loads*, not
   that the application is correct — there is no test-case format in the spec
   to run. Test behaviour against the source with Run/Test first.
4. **`package.path` is empty for packages.** Any module reached with
   `require()` must be inside the bundle. Dynamic `require()` of a name
   computed at runtime will fail unless that module was bundled.
5. **Rebuilding after a key change invalidates everything.** Packages signed
   with the old key stop being accepted.

---

## 8. Tests

```bash
./scripts/package-lua-test.sh ./build/hsf_gateway 18090
```

39 black-box checks: key handling, all five build stages, compile/traversal/
version rejections, the artifact containing no plaintext, deploy-and-run,
**running with the source tree moved away**, tamper rejection, versioning and
rollback, and that a viewer can list packages but not build, deploy, roll back
or create keys. Runs in CI on every push.
