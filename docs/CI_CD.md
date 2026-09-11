# Gateway CI/CD

The repository uses GitHub Actions, manually created SemVer tags, and draft
GitHub Releases for retained artifacts. These follow the existing release
workflow. Publishing a draft remains a maintainer operation. No repository
settings or remote infrastructure are changed by these files.

## Pull requests and builds

`Gateway CI` runs on PRs targeting `main`, `develop`, and `release/*`, and on
pushes to development branches. Require the stable `Gateway CI required`
check in branch rules. Documentation/WebUI-only changes run validation without
the firmware matrix. Unknown paths and shared inputs trigger all firmware
builds. Logs record the affected component groups.

The gateway currently links its runtime components into one executable. A REST
or Lua change therefore still rebuilds the gateway; component detection is not
equivalent to independent component binaries. Fine-grained native build caching
and separately releasable components remain follow-up work. Existing Rust and
broker workflows continue to provide their own coverage.

The reusable `firmware-build.yml` builds Windows x86/x64/ARM64 and Linux
x64/ARM64/armhf. Windows uses MSVC and the vcpkg baseline from `vcpkg.json`.
Linux uses Ubuntu 22.04 target containers under QEMU and the existing Ubuntu
build/package scripts. CTest and security/Lua integration tests run within the
Linux container. Native Windows builds run CTest. Windows ARM64 is compile and
package only on the x64 host; it never claims native execution success.

`windows-arm64` is the package/platform identifier for Windows IoT ARM64;
`linux-armhf` represents the plan's Linux ARM32 hard-float target. Board model
names do not establish CPU compatibility; verify OS bitness and ABI on each
actual RK board. These workflows do not establish Ubuntu 18.04 compatibility.
The existing Ubuntu compatibility workflow is separate. The previous macOS
release matrix entry is not in this firmware matrix.

QEMU ARM builds can be slow and have a 180-minute timeout. No ARM matrix run
has been verified locally on this Windows workstation. ARM32 dependency/tool
support and Windows ARM64 tool installation need a first hosted CI run before
claiming the multi-platform acceptance criteria.

## Releases and firmware import

1. Use `feature/*` and `fix/*` PRs into `develop`; promote tested commits into
   `main` or `release/*`. Use subjects such as `feat: ...`, `fix: ...`,
   `refactor: ...`, `build: ...`, and `ci: ...`. Existing history is not rewritten.
2. Set `VERSION` and add its `## [VERSION]` entry in `CHANGELOG.md`.
3. Create and push `vVERSION` on a commit reachable from `main` or `release/*`.
   A manual dispatch must name an existing tag. Every package checkout uses the
   resolved commit; validation checks the tag, VERSION and branch ancestry.
4. CI creates `eMaster-{version}-{platform}-{architecture}.zip` or `.tar.gz`,
   one `{archive}.manifest.json` per archive, `SHA256SUMS`, and OTA `version.json`.
   It verifies the complete six-target set before creating the draft.
5. Review the draft and hardware evidence, then publish. The Firmware Server
   imports each archive with its sidecar manifest, validating
   `config/release-manifest.schema.json` and recomputing its checksum. It must
   retain CI metadata and reject duplicate product/version/platform/architecture
   keys. CI artifact storage expires after 14 days; release assets are the
   retained source for Firmware Server import.

`dev`/`alpha` prereleases map to `dev`; other prereleases map to `beta`; versions
without a prerelease map to `stable`. PR artifacts are always `dev`. Build IDs
include the Actions run ID and attempt. Sidecars contain commit, UTC metadata
creation time (`build_time`), actor, archive size and SHA256. The embedded
manifest remains the existing package metadata: an archive cannot contain its
own final SHA256. The sidecar schema is a versioned Firmware Server contract;
`version.json` retains the existing gateway OTA protocol and signature format.

An existing release, including a draft, is rejected. Uploads never use
`--clobber`; partial drafts remain visible for investigation and are not
silently repaired by reruns. Use a new version after a failed partial publish.
Workflow checks alone cannot prevent a repository administrator from changing
assets: configure repository release immutability for server enforcement.

## Signing, protection and hardware setup

Configure these on GitHub before production operation:

- Protect `main` and `release/*`: require PR review, required CI checks, and
  restrict force pushes/deletion. Restrict creation/update/deletion of `v*`
  tags to release maintainers. Protect workflow and CI tooling edits through
  review. Tag ancestry is an additional check, not a substitute for rulesets.
- Enable immutable published releases. The workflow creates drafts and does
  not automatically publish. Further approval gates can be configured through
  environments without placing secrets in source.
- Store the existing `UPDATE_SIGNING_KEY` PEM in Actions secrets. It is written
  to a runner temporary file with restrictive permissions and removed by a
  trap. Unsigned drafts are allowed for the initial phase; production gateways
  with signature enforcement reject them. HSM/KMS integration is not added.
- Register isolated self-hosted runners with labels `linux-arm64`,
  `linux-armhf`, and optionally `windows-arm64`, with native Python 3.13 and
  runtime dependencies. Restrict these runners to trusted workflows/branches;
  never execute untrusted PR code on persistent hardware runners.
- Dispatch `Firmware hardware smoke` with the release tag and successful
  release run ID while its CI artifacts are retained. A hosted job verifies
  the release workflow, tag ancestry and downloaded artifact provenance before
  scheduling hardware. Each board checks its native architecture, checksum,
  executable startup and HTTP readiness using disposable configuration.

The base hardware smoke does not exercise a physical relay, serial port, or
plugin initialization. Add site-specific driver/plugin checks with hardware
fixtures before satisfying that acceptance criterion. Hardware tests are
manual until runners are registered, and are not an automatic release gate.
Branch rules, immutable release settings, runner registration, real hardware
tests, and Firmware Server deployment remain external setup tasks.

## Local validation

Build first with `build.bat Release`, then run CTest against `build-win` with
`-C Release --output-on-failure --no-tests=error`. Tooling tests:

```text
python -m compileall -q scripts/ci
python -m unittest discover -s scripts/ci -p test_*.py -v
```

Tests cover package provenance, version/tag/branch validation, channel mapping,
archive tampering, missing targets, overwrite rejection and path traversal.
Validate workflows with actionlint and edited POSIX scripts with `bash -n`.

Windows plugin loading suppresses the loader's critical-error dialog for the
calling thread and restores its prior error mode afterwards. This lets invalid
DLLs return their OS error during unattended tests and service operation;
the existing invalid-plugin regression test covers that failure path.

References: [Actions workflow syntax](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax),
[checkout ref behavior](https://github.com/actions/checkout),
[vcpkg target platforms](https://learn.microsoft.com/en-us/vcpkg/users/platforms/all).
Windows loader behavior: [SetThreadErrorMode](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-setthreaderrormode).

## Verification recorded on 2026-09-11

- `build.bat Release`: passed, including the Rust workspace Release build.
- CTest on Windows x86: 13/13 passed. The initial invalid-DLL test timed out;
  after the loader error-mode fix, the complete suite passed in 0.17 seconds.
- CI tooling unit tests: 11/11 passed.
- PowerShell parser, edited shell scripts (`bash -n`), packaging/update contract,
  and `git diff --check` for edited tracked files: passed.
- actionlint v1.7.12: all four new/updated workflows passed. Its optional
  shellcheck and pyflakes integrations were disabled; shell syntax and Python
  compilation were checked separately.
- Hosted matrix execution, real ARM hardware, release upload and repository
  protection/immutability settings have not been exercised or configured here.
