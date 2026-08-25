#!/usr/bin/env bash
set -euo pipefail

# Build the gateway's native plugins with the same Linux toolchain and ABI
# settings as the gateway. Run build-ubuntu.sh first so this build directory
# already has the vcpkg/toolchain configuration.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-$ROOT/build-ubuntu}"
JOBS="${JOBS:-$( (command -v nproc >/dev/null && nproc) || echo 4)}"

# Match the compiler selected by build-ubuntu.sh on Ubuntu 18.04. Without
# carrying this into the second configure, vcpkg sees the distro g++-7 and
# rebuilds every dependency with a different ABI/compiler hash.
if command -v g++-9 >/dev/null 2>&1; then
  export CC="${CC:-gcc-9}" CXX="${CXX:-g++-9}"
fi

[[ -f "$BUILD_DIR/CMakeCache.txt" ]] || {
  echo "[plugins] configure the core first: scripts/build-ubuntu.sh --build-dir $BUILD_DIR" >&2
  exit 2
}

cmake -S "$ROOT" -B "$BUILD_DIR" \
  -DHSF_BUILD_PLUGIN_SDK=ON \
  -DHSF_SDK_BUILD_TESTS=OFF \
  -DHSF_SDK_BUILD_EXAMPLES=OFF
cmake --build "$BUILD_DIR" --target hsf_driver_modbus hsf_driver_c3protocol -j "$JOBS"

mkdir -p "$BUILD_DIR/plugins/modbus" "$BUILD_DIR/plugins/c3protocol"
cp "$ROOT/plugins/modbus/manifest.json" "$BUILD_DIR/plugins/modbus/manifest.json"
cp "$ROOT/plugins/c3protocol/manifest.json" "$BUILD_DIR/plugins/c3protocol/manifest.json"
test -s "$BUILD_DIR/plugins/modbus/plugin.so"
test -s "$BUILD_DIR/plugins/c3protocol/plugin.so"
chmod +x "$BUILD_DIR/plugins/modbus/plugin.so" "$BUILD_DIR/plugins/c3protocol/plugin.so"
echo "[plugins] staged under $BUILD_DIR/plugins"
