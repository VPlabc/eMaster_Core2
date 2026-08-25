#!/usr/bin/env bash
#
# Auto build script for Ubuntu/Debian (vcpkg + CMake + GCC).
# The Linux twin of build.bat -- same job, same layout, different toolchain.
#
#   ./build.sh                    # Release, incremental, into build-linux/
#   ./build.sh Debug              # a different configuration
#   ./build.sh --clean            # throw the build directory away first
#   ./build.sh --install-deps     # apt-get the prerequisites (asks for sudo)
#
# WHY vcpkg AND NOT plain apt. Crow and AMQP-CPP are not in the Ubuntu
# archives, and vcpkg.json is already this project's dependency manifest on
# Windows -- one list, one set of versions, both platforms. The apt packages
# below are only what vcpkg itself needs in order to build them.
#
# ZK IS OFF ON LINUX, AND THAT IS NOT A LIMITATION OF THIS SCRIPT. The ZKTeco
# PullSDK is a 32-bit Windows DLL (plcommpro.dll) that nobody can rebuild or
# port; CMakeLists.txt hard-fails if you try to force it on elsewhere. The
# gateway builds and runs fine without it -- the Lua zk.* API stays present and
# every call answers NotSupportedOnThisPlatform (src/zk_controller/
# PullSdkClient_stub.cpp). Card reading over the REST card API, Modbus, RabbitMQ,
# the web UI and the SmartLocker application are all unaffected. Use the Windows
# x86 build where a ZK controller has to be driven.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$REPO_ROOT"

CONFIG="Release"
CLEAN=0
INSTALL_DEPS=0

for arg in "$@"; do
  case "$arg" in
    Debug|Release|RelWithDebInfo|MinSizeRel) CONFIG="$arg" ;;
    --clean)        CLEAN=1 ;;
    --install-deps) INSTALL_DEPS=1 ;;
    -h|--help)
      sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "[build] unknown argument '$arg' -- try --help" >&2
      exit 2
      ;;
  esac
done

BUILD_DIR="$REPO_ROOT/build-linux"
VCPKG_DIR="${VCPKG_ROOT:-$REPO_ROOT/build/vcpkg}"

# --- prerequisites -------------------------------------------------------
#
# The same list the release workflow installs (.github/workflows/release.yml),
# plus ninja because it makes an incremental rebuild noticeably quicker. These
# are vcpkg's build-time needs, not the gateway's runtime needs.
APT_PACKAGES=(build-essential cmake pkg-config curl zip unzip tar git
              autoconf automake libtool ninja-build)

if [[ $INSTALL_DEPS -eq 1 ]]; then
  echo "[build] installing prerequisites with apt-get (sudo)..."
  sudo apt-get update
  sudo apt-get install -y "${APT_PACKAGES[@]}"
fi

# Checked rather than installed by default: a build script that quietly changes
# system packages is a build script that surprises somebody. --install-deps is
# there for when that is what you want.
missing=()
for tool in cmake git curl tar unzip zip pkg-config; do
  command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
command -v g++ >/dev/null 2>&1 || command -v clang++ >/dev/null 2>&1 || missing+=("g++")

if [[ ${#missing[@]} -gt 0 ]]; then
  echo "[build] ERROR: missing tool(s): ${missing[*]}" >&2
  echo "[build] Install them with:" >&2
  echo "          sudo apt-get update && sudo apt-get install -y ${APT_PACKAGES[*]}" >&2
  echo "[build] Or re-run this script with --install-deps." >&2
  exit 1
fi

# CMake 3.20+ is what CMakeLists.txt requires; Ubuntu 20.04 ships 3.16, which
# fails at configure time with a message that does not mention the version.
CMAKE_VERSION="$(cmake --version | head -n1 | awk '{print $3}')"
if [[ "$(printf '%s\n3.20\n' "$CMAKE_VERSION" | sort -V | head -n1)" != "3.20" ]]; then
  echo "[build] ERROR: cmake $CMAKE_VERSION found, 3.20 or newer required." >&2
  echo "[build] On Ubuntu 20.04: use the Kitware APT repository, or snap install cmake --classic." >&2
  exit 1
fi

# --- vcpkg ---------------------------------------------------------------
if [[ ! -x "$VCPKG_DIR/vcpkg" ]]; then
  if [[ ! -d "$VCPKG_DIR" ]]; then
    echo "[build] Cloning vcpkg into $VCPKG_DIR ..."
    git clone --depth 1 https://github.com/microsoft/vcpkg "$VCPKG_DIR"
  fi

  if [[ ! -x "$VCPKG_DIR/bootstrap-vcpkg.sh" ]]; then
    echo "[build] ERROR: \"$VCPKG_DIR\" does not look like a vcpkg checkout." >&2
    echo "[build] Remove it, or point VCPKG_ROOT at a real one." >&2
    exit 1
  fi

  echo "[build] Bootstrapping vcpkg..."
  "$VCPKG_DIR/bootstrap-vcpkg.sh" -disableMetrics
fi

# Native triplet. vcpkg would infer this, but naming it keeps the build
# directory's contents obvious and matches how build.bat spells its own.
case "$(uname -m)" in
  x86_64|amd64)  TRIPLET="x64-linux" ;;
  aarch64|arm64) TRIPLET="arm64-linux" ;;
  armv7l)        TRIPLET="arm-linux" ;;
  *)             TRIPLET="" ;;   # let vcpkg decide
esac

echo "[build] Using cmake $CMAKE_VERSION, vcpkg at $VCPKG_DIR${TRIPLET:+, triplet $TRIPLET}"

# --- configure -----------------------------------------------------------
if [[ $CLEAN -eq 1 ]]; then
  echo "[build] Removing $BUILD_DIR"
  rm -rf "$BUILD_DIR"
fi

CMAKE_ARGS=(
  -S "$REPO_ROOT"
  -B "$BUILD_DIR"
  -DCMAKE_BUILD_TYPE="$CONFIG"
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_DIR/scripts/buildsystems/vcpkg.cmake"
  # Explicit, though it is also the default off Windows: the reader of this
  # script should not have to go and check what the default is.
  -DHSF_ENABLE_ZK=OFF
)

[[ -n "$TRIPLET" ]] && CMAKE_ARGS+=(-DVCPKG_TARGET_TRIPLET="$TRIPLET")
command -v ninja >/dev/null 2>&1 && CMAKE_ARGS+=(-G Ninja)

echo "[build] Configuring into \"$BUILD_DIR\" (the first run builds vcpkg dependencies from source -- 15-30+ min)..."
cmake "${CMAKE_ARGS[@]}"

# --- build ---------------------------------------------------------------
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

echo "[build] Building ($CONFIG, -j$JOBS)..."
cmake --build "$BUILD_DIR" --config "$CONFIG" -j "$JOBS"

EXE="$BUILD_DIR/hsf_gateway"
[[ -x "$EXE" ]] || { echo "[build] Built executable not found at $EXE" >&2; exit 1; }

echo
echo "[build] Build succeeded: $EXE"
"$EXE" --version 2>/dev/null | head -n 2 || true
echo
echo "[build] Run it with:   ./scripts/run.sh"
echo "[build] Package it:    ./scripts/package.sh    (clean release build + tarball)"
