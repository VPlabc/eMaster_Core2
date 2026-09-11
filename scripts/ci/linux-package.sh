#!/usr/bin/env bash
# Run inside a native or QEMU Ubuntu 22.04 container. Package the tested tree.
set -euo pipefail
target="$1"
# The target image is intentionally minimal. Install the build driver before
# CMake/vcpkg starts for every architecture; otherwise vcpkg reports a generic
# install failure and the useful root cause appears later as
# CMAKE_MAKE_PROGRAM missing.
apt-get update -qq
apt-get install -y -qq --no-install-recommends ninja-build build-essential pkg-config git curl ca-certificates tar >/dev/null
command -v ninja >/dev/null || { echo "ninja-build is required but missing from PATH" >&2; exit 2; }
export CMAKE_MAKE_PROGRAM="$(command -v ninja)"
bash scripts/build-ubuntu.sh --build-dir /tmp/emaster-build
export HSF_REUSE_BUILD_DIR=/tmp/emaster-build
bash scripts/package.sh "$target"
