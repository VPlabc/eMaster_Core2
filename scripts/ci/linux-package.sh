#!/usr/bin/env bash
# Run inside a native or QEMU Ubuntu 22.04 container. Package the tested tree.
set -euo pipefail
target="$1"
bash scripts/build-ubuntu.sh --build-dir /tmp/emaster-build
export HSF_REUSE_BUILD_DIR=/tmp/emaster-build
bash scripts/package.sh "$target"
