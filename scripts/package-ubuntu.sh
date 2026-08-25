#!/usr/bin/env bash
#
# Ubuntu production package (request/deloyToUbuntu.md sections 3.4-3.5).
#
# A THIN LAYER OVER scripts/package.sh, not a replacement. That script already
# stages the release layout, refuses to include any *.db or config.json, and
# writes a checksum -- and it is what the tagged-release pipeline uses on every
# platform. Duplicating it here would give the fleet two package formats that
# drift, which is what section 3.1 warns against. What this adds is the part
# that is genuinely Ubuntu-specific:
#
#   - the shared-library report ldd gives (section 3.4),
#   - the matching apt package list for the target,
#   - the deployment metadata the deploy script records.
#
# Usage:
#   ./scripts/package-ubuntu.sh                 # uses build-ubuntu/
#   ./scripts/package-ubuntu.sh --build-dir X
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

BUILD_DIR="$REPO_ROOT/build-ubuntu"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    -h|--help) sed -n '2,18p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

say() { printf '\033[1m[package]\033[0m %s\n' "$*"; }
die() { printf '\033[31m[package] %s\033[0m\n' "$*" >&2; exit 1; }

[[ "$(uname -s)" == "Linux" ]] || die "run this on Linux (or inside scripts/build-ubuntu.sh --docker)"

VERSION="$(head -n1 VERSION | tr -d '[:space:]')"
ARCH="$(uname -m)"
case "$ARCH" in
  x86_64) PLATFORM="linux-x64" ;;
  aarch64|arm64) PLATFORM="linux-arm64" ;;
  *) PLATFORM="linux-$ARCH" ;;
esac

say "eMaster Gateway $VERSION for $PLATFORM"

# Reuse the tree build-ubuntu.sh just produced rather than paying for a second
# full configure+build -- and, more importantly, so the artifact that ships is
# the exact binary the test suites were run against a moment ago.
if [[ -x "$BUILD_DIR/hsf_gateway" ]]; then
  say "Reusing $BUILD_DIR"
  export HSF_REUSE_BUILD_DIR="$BUILD_DIR"
else
  say "No build at $BUILD_DIR -- package.sh will do a clean build"
fi
./scripts/package.sh "$PLATFORM"

STAGE_NAME="HSF-Gateway-v${VERSION}-${PLATFORM}"
TARBALL="$REPO_ROOT/dist/${STAGE_NAME}.tar.gz"
[[ -f "$TARBALL" ]] || die "package.sh did not produce $TARBALL"

# --- shared library report (section 3.4) ------------------------------------
#
# Answers "what must be installed on the target", which is the question that
# actually matters at deploy time. It deliberately does NOT copy anything out
# of /lib here: section 3.4 says not to, and a glibc or libstdc++ lifted from
# the build host is the classic way to produce a package that segfaults on a
# slightly older target.
BINARY="$BUILD_DIR/hsf_gateway"
[[ -x "$BINARY" ]] || BINARY="$REPO_ROOT/build-release/hsf_gateway"
REPORT="$REPO_ROOT/dist/${STAGE_NAME}.dependencies.txt"

{
  echo "Shared library dependencies for $STAGE_NAME"
  echo "Generated $(date -u +%Y-%m-%dT%H:%M:%SZ) on $( . /etc/os-release; echo "$PRETTY_NAME" )"
  echo
  if [[ -x "$BINARY" ]]; then
    echo "--- ldd ---"
    ldd "$BINARY" | sed 's/^/  /'
    echo
    echo "--- not found (must be installed on the target) ---"
    if ldd "$BINARY" | grep -q 'not found'; then
      ldd "$BINARY" | grep 'not found' | sed 's/^/  /'
    else
      echo "  (none -- every dependency resolved on this host)"
    fi
    echo
    echo "--- owning apt packages ---"
    # Maps each resolved .so back to the package that provides it, which is
    # what someone provisioning the target actually needs.
    ldd "$BINARY" | awk '{print $3}' | grep '^/' | sort -u | while read -r lib; do
      owner="$(dpkg -S "$(readlink -f "$lib")" 2>/dev/null | cut -d: -f1 | head -1)"
      printf '  %-45s %s\n' "$(basename "$lib")" "${owner:-<not from a package>}"
    done | sort -u
  else
    echo "  (no binary at $BINARY -- run scripts/build-ubuntu.sh first)"
  fi
  echo
  echo "--- minimum apt install on the target ---"
  # Everything else (curl, lua, sqlite3, crow, asio, amqpcpp, openssl,
  # libsodium) is built by vcpkg and linked statically or shipped in the
  # package, so the target needs only the base C/C++ runtime.
  echo "  sudo apt-get install -y libstdc++6 libgcc-s1 zlib1g ca-certificates"
  echo
  echo "  Serial hardware additionally needs the service user in the 'dialout' group."
} > "$REPORT"

say "Dependency report: $REPORT"
# Ask ldd directly rather than grepping the report: the report contains the
# heading "--- not found (must be installed on the target) ---" and the
# reassuring "(none -- every dependency resolved on this host)", both of which
# match a bare `grep 'not found'`. So the warning fired on every package,
# including ones with nothing wrong -- a warning that is always on is a warning
# nobody reads.
if [[ -x "$BINARY" ]] && ldd "$BINARY" | grep -q '=> not found'; then
  echo "[package] WARNING: unresolved shared libraries -- see the report" >&2
fi

# --- checksum + manifest ----------------------------------------------------
SHA="$(sha256sum "$TARBALL" | cut -d' ' -f1)"
MANIFEST="$REPO_ROOT/dist/${STAGE_NAME}.manifest.txt"
{
  echo "package:      $(basename "$TARBALL")"
  echo "version:      $VERSION"
  echo "platform:     $PLATFORM"
  echo "sha256:       $SHA"
  echo "size_bytes:   $(wc -c < "$TARBALL")"
  echo "built_at:     $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "git_commit:   $(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  echo "build_host:   $(uname -srm)"
} > "$MANIFEST"

say "$TARBALL"
say "sha256 $SHA"
say "Manifest: $MANIFEST"

# --- contents check (section 3.5's "do not include") ------------------------
say "Verifying package contents"
LISTING="$(tar -tzf "$TARBALL")"
FORBIDDEN=0
while IFS= read -r pattern; do
  if grep -qE "$pattern" <<<"$LISTING"; then
    echo "[package] REFUSING: package contains $pattern" >&2
    grep -E "$pattern" <<<"$LISTING" | sed 's/^/    /' >&2
    FORBIDDEN=1
  fi
done <<'PATTERNS'
\.git/
\.db$
config\.json$
\.key$
\.pem$
\.pdb$
/src/
PATTERNS
[[ $FORBIDDEN -eq 0 ]] || die "package contains files section 3.5 excludes"
say "Contents OK ($(wc -l <<<"$LISTING") entries, no source/secrets/debug artifacts)"
