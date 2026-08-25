#!/usr/bin/env bash
#
# Builds and packages the eMaster Gateway for Linux/macOS
# (request/release.md sections 9, 15, 16, 30).
#
# Always configures into a CLEAN build directory: a release must be
# reproducible from the tagged commit, and an incremental build can carry
# stale objects or a stale configure-time Git hash baked in by CMake.
#
# ZK support is left OFF on these platforms -- the ZKTeco PullSDK is a 32-bit
# Windows DLL. The gateway still builds and runs; every zk.* call fails with
# NotSupportedOnThisPlatform. Use the Windows x86 package for ZK card reading.
#
# Usage:
#   scripts/package.sh                 # native host arch
#   scripts/package.sh linux-arm64     # explicit label for a cross build
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

VERSION="$(head -n1 VERSION | tr -d '[:space:]')"
if ! [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+ ]]; then
  echo "VERSION must contain a semantic version like 1.0.0, got '$VERSION'" >&2
  exit 1
fi

# Platform label. Defaults to the host; pass an explicit one when cross
# compiling, since uname would still report the host.
if [[ $# -ge 1 ]]; then
  PLATFORM="$1"
else
  case "$(uname -s)" in
    Darwin) OS="macos" ;;
    Linux)  OS="linux" ;;
    *)      OS="$(uname -s | tr '[:upper:]' '[:lower:]')" ;;
  esac
  case "$(uname -m)" in
    x86_64|amd64)  ARCH="x64" ;;
    aarch64|arm64) ARCH="arm64" ;;
    armv7l)        ARCH="arm" ;;
    riscv64)       ARCH="riscv64" ;;
    *)             ARCH="$(uname -m)" ;;
  esac
  PLATFORM="${OS}-${ARCH}"
fi

BUILD_DIR="$REPO_ROOT/build-release"
STAGE_NAME="HSF-Gateway-v${VERSION}-${PLATFORM}"
DIST_DIR="$REPO_ROOT/dist"
STAGE_DIR="$DIST_DIR/$STAGE_NAME"

echo "[package] eMaster Gateway $VERSION -> $PLATFORM (ZK=OFF)"

# HSF_REUSE_BUILD_DIR names an ALREADY-BUILT tree to package instead of
# configuring a clean one. Off by default, because a release must be
# reproducible from the tagged commit and an incremental build can carry stale
# objects -- that is why this script wipes the directory in the first place.
#
# It exists for scripts/build-ubuntu.sh, which has just done a full clean
# Release build (and run the test suites against that exact binary) moments
# earlier. Rebuilding it here would double a container build for no gain, and
# would package a binary that nothing had tested.
if [[ -n "${HSF_REUSE_BUILD_DIR:-}" ]]; then
  BUILD_DIR="$HSF_REUSE_BUILD_DIR"
  [[ -x "$BUILD_DIR/hsf_gateway" ]] \
    || { echo "HSF_REUSE_BUILD_DIR=$BUILD_DIR has no built hsf_gateway" >&2; exit 1; }
  echo "[package] Reusing the existing build in $BUILD_DIR (no reconfigure)"
else
  CMAKE_ARGS=(-S "$REPO_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DHSF_ENABLE_ZK=OFF -DHSF_BUILD_PLUGIN_SDK=OFF)
  if [[ -n "${VCPKG_ROOT:-}" ]]; then
    CMAKE_ARGS+=("-DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
  fi

  rm -rf "$BUILD_DIR"
  cmake "${CMAKE_ARGS[@]}"
  cmake --build "$BUILD_DIR" --config Release -j "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
fi

EXE="$BUILD_DIR/hsf_gateway"
[[ -x "$EXE" ]] || { echo "Built executable not found at $EXE" >&2; exit 1; }

# --- stage ---------------------------------------------------------------
# Layout mirrors release.md section 15, flattened so the binary sits beside
# web/ and config/ -- which is what ResolveResourceDir() in main.cpp looks
# for first, so the package runs from wherever it is extracted.
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR/bin" "$STAGE_DIR/config" "$STAGE_DIR/scripts"

cp "$EXE" "$STAGE_DIR/bin/"
cp -R "$REPO_ROOT/web" "$STAGE_DIR/web"

# config/: scripts and the TEMPLATE only. config.db, config.json and
# clients.db carry the REST API key and generated client keys
# (release.md section 2) and must never be packaged.
cp "$REPO_ROOT/config/config.example.json" "$STAGE_DIR/config/"
cp -R "$REPO_ROOT/config/scripts" "$STAGE_DIR/config/scripts"

[[ -d "$REPO_ROOT/docs" ]] && cp -R "$REPO_ROOT/docs" "$STAGE_DIR/docs"
for f in README.md CHANGELOG.md LICENSE VERSION; do
  [[ -f "$REPO_ROOT/$f" ]] && cp "$REPO_ROOT/$f" "$STAGE_DIR/"
done
cp "$REPO_ROOT/scripts/run.sh" "$STAGE_DIR/scripts/" 2>/dev/null || true
chmod +x "$STAGE_DIR/scripts/run.sh" 2>/dev/null || true

# OTA install tooling (request/CICD.md). Shipped inside the package because the
# first install is done from a downloaded package, before there is a checkout
# on the device -- and because a release the gateway installs over the air must
# carry the unit file its own installer templated, or a future re-run of
# install-ota.sh on that release has nothing to substitute into.
cp "$REPO_ROOT/scripts/install-ota.sh" "$STAGE_DIR/scripts/" 2>/dev/null || true
chmod +x "$STAGE_DIR/scripts/install-ota.sh" 2>/dev/null || true
[[ -d "$REPO_ROOT/deploy" ]] && cp -R "$REPO_ROOT/deploy" "$STAGE_DIR/deploy"

# Belt and braces: fail loudly rather than ship a credential.
if find "$STAGE_DIR" \( -name '*.db' -o -name 'config.json' \) -print -quit | grep -q .; then
  echo "Refusing to package: staging directory contains files excluded by release.md section 2" >&2
  find "$STAGE_DIR" \( -name '*.db' -o -name 'config.json' \) >&2
  exit 1
fi

"$STAGE_DIR/bin/hsf_gateway" --version | tee "$STAGE_DIR/BUILD_INFO.txt"

# --- archive + checksum --------------------------------------------------
mkdir -p "$DIST_DIR"
TARBALL="$DIST_DIR/${STAGE_NAME}.tar.gz"
rm -f "$TARBALL"
tar -czf "$TARBALL" -C "$DIST_DIR" "$STAGE_NAME"

if command -v sha256sum >/dev/null 2>&1; then
  (cd "$DIST_DIR" && sha256sum "${STAGE_NAME}.tar.gz" >> SHA256SUMS)
  HASH="$(cd "$DIST_DIR" && sha256sum "${STAGE_NAME}.tar.gz" | cut -d' ' -f1)"
else
  # macOS ships shasum, not sha256sum.
  (cd "$DIST_DIR" && shasum -a 256 "${STAGE_NAME}.tar.gz" >> SHA256SUMS)
  HASH="$(cd "$DIST_DIR" && shasum -a 256 "${STAGE_NAME}.tar.gz" | cut -d' ' -f1)"
fi

echo "[package] $TARBALL"
echo "[package] sha256 $HASH"
echo "[package] Extract it somewhere clean and run scripts/run.sh before publishing (release.md section 17)."
