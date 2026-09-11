#!/usr/bin/env bash
#
# Ubuntu Release build (request/deloyToUbuntu.md sections 3.2-3.4).
#
# Runs natively on Ubuntu, or in a container from anywhere else:
#
#   ./scripts/build-ubuntu.sh              # native, on Ubuntu
#   ./scripts/build-ubuntu.sh --docker     # in ubuntu:22.04, from Windows/macOS
#
# The --docker path exists because the machine this project is developed on has
# no Linux toolchain at all. Cross-compiling from Windows would mean a second
# toolchain to keep working; a container gives the exact userspace the target
# runs, so "it built" means something about the target rather than about the
# build host.
#
# NOT A SECOND BUILD SYSTEM (section 3.1's rule). This configures the SAME
# CMakeLists.txt with the same vcpkg manifest as every other platform; it adds
# the Ubuntu package list, the Release flags and the verification steps, and
# nothing else.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

USE_DOCKER=0
RUN_TESTS=1
BUILD_DIR="$REPO_ROOT/build-ubuntu"
OUTPUT_DIR="$REPO_ROOT/build-ubuntu"
DOCKER_PLATFORM=""
# BUILD ON THE OLDEST RUNTIME YOU MUST SUPPORT, not the newest you have.
#
# glibc symbols are versioned and only forward-compatible: a binary built
# against glibc 2.35 (22.04) records references to GLIBC_2.29/2.32/2.33 and
# dies on an older machine before main() runs --
#
#   /lib/x86_64-linux-gnu/libc.so.6: version `GLIBC_2.28' not found
#
# which is exactly what happened deploying to the 18.04 target here. Building
# on 18.04 produces a binary that runs on 18.04 AND on everything newer.
# Override with --image when every target is known to be newer.
IMAGE="ubuntu:18.04"
JOBS="$( (command -v nproc >/dev/null && nproc) || echo 4)"
# Named volume holding the vcpkg checkout, its binary cache and the build tree.
# Survives between runs, so the second build reuses the dependencies.
VOLUME="hsf-gateway-ubuntu-build"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --docker)     USE_DOCKER=1; shift ;;
    --docker-platform|--platform)
      DOCKER_PLATFORM="$2"; shift 2 ;;
    --image)      IMAGE="$2"; shift 2 ;;
    --build-dir)  BUILD_DIR="$2"; shift 2 ;;
    --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
    --no-tests)   RUN_TESTS=0; shift ;;
    --jobs)       JOBS="$2"; shift 2 ;;
    -h|--help)
      sed -n '2,20p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

say() { printf '\033[1m[build]\033[0m %s\n' "$*"; }
die() { printf '\033[31m[build] %s\033[0m\n' "$*" >&2; exit 1; }

# --- container path ---------------------------------------------------------
if [[ $USE_DOCKER -eq 1 ]]; then
  command -v docker >/dev/null || die "docker is not installed"
  docker info >/dev/null 2>&1 || die "the docker daemon is not running (start Docker Desktop)"

  say "Building inside $IMAGE"

  PLATFORM_ARGS=()
  if [[ -n "$DOCKER_PLATFORM" ]]; then
    PLATFORM_ARGS+=(--platform "$DOCKER_PLATFORM")
    # Keep dependency/build volumes separate between x64 and ARM64 builds.
    VOLUME="${VOLUME}-${DOCKER_PLATFORM//\//-}"
    say "Docker platform: $DOCKER_PLATFORM"
  fi

  # Git-Bash/MSYS rewrites anything that looks like a Unix path in an argument
  # into a Windows path before the program sees it, so `-w /src` reaches docker
  # as `C:/Program Files/Git/src` and the run fails with "the working directory
  # is invalid". MSYS_NO_PATHCONV turns that off for this command; the host
  # side of -v still has to be a path Docker Desktop understands, which cygpath
  # gives us.
  HOST_PATH="$REPO_ROOT"
  if command -v cygpath >/dev/null 2>&1; then
    HOST_PATH="$(cygpath -w "$REPO_ROOT")"
    export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
  fi

  # EVERYTHING WRITTEN GOES ON A DOCKER VOLUME, NOT THE BIND MOUNT.
  #
  # The first version of this put vcpkg and the build tree under /src, which is
  # the Windows filesystem shared into the VM. Building OpenSSL there took over
  # three hours and had not finished -- a dependency build is hundreds of
  # thousands of small file operations and every one crosses that boundary. On
  # the volume (native overlay inside the VM) it is minutes. It also stops the
  # build filling the developer's C: drive, which was at 97% here.
  #
  # /src stays mounted so sources are read from the working tree and artifacts
  # can be copied back at the end, but nothing heavy is written to it.
  docker volume create "$VOLUME" >/dev/null

  docker run --rm -t "${PLATFORM_ARGS[@]}" \
    -v "$HOST_PATH:/src" \
    -v "$VOLUME:/work" \
    -e VCPKG_DEFAULT_BINARY_CACHE=/work/vcpkg-cache \
    -e DEBIAN_FRONTEND=noninteractive \
    -w /src "$IMAGE" \
    bash -lc "set -e
              mkdir -p /work/vcpkg-cache
              VCPKG_ROOT=/work/vcpkg scripts/build-ubuntu.sh \
                --build-dir /work/build --jobs $JOBS \
                $([[ $RUN_TESTS -eq 0 ]] && echo --no-tests)
              mkdir -p /src/$(basename "$OUTPUT_DIR")
              cp /work/build/hsf_gateway /src/$(basename "$OUTPUT_DIR")/
              # This one copy is genuinely optional, so it gets its own || true.
              # Previously the || true sat at the end of one long && chain and
              # swallowed the exit status of the ENTIRE build -- a container
              # that died during apt still reported success.
              cp /work/build/BUILD_INFO.txt /src/$(basename "$OUTPUT_DIR")/ 2>/dev/null || true"
  # `docker run`'s exit status IS the build's. Reporting success unconditionally
  # -- which this did -- meant a container that died during apt still printed
  # "Artifacts copied", and the caller believed it.
  DOCKER_STATUS=$?
  [[ $DOCKER_STATUS -eq 0 ]] || die "the containerised build failed (exit $DOCKER_STATUS)"
  # -s, not -x: on a Windows bind mount NTFS carries no execute bit, so a Linux
  # ELF copied out of the container reads as mode 644 and an -x test would fail
  # every time on a build that actually succeeded. The bit is set on the target
  # by package-ubuntu.sh / deploy-ubuntu.sh, where it means something.
  [[ -s "$OUTPUT_DIR/hsf_gateway" ]] || die "the build reported success but produced no binary"
  say "Artifacts copied to $OUTPUT_DIR/"
  exit 0
fi

# --- native path ------------------------------------------------------------
if [[ ! -r /etc/os-release ]]; then
  die "this is not a Linux host. Use --docker, or run this on Ubuntu."
fi
. /etc/os-release
say "Host: ${PRETTY_NAME:-unknown} ($(uname -m))"

# --- 1. dependencies --------------------------------------------------------
APT_PACKAGES=(
  build-essential cmake ninja-build pkg-config git curl zip unzip tar
  autoconf automake libtool python3 ca-certificates
  # vcpkg builds these from source, but their configure steps need these:
  linux-libc-dev
  # libsodium's autotools build fails at aclocal without these two, with the
  # admirably direct "System package autoconf-archive is missing". They are not
  # in ubuntu:22.04's base image, and libsodium is not optional -- password
  # hashing depends on it -- so a build without them cannot produce a working
  # gateway.
  autoconf-archive libltdl-dev
  # OpenSSL's Configure is a Perl script; present in the base image today, but
  # naming it means a slimmer base image fails here with a clear message
  # rather than deep inside a dependency build.
  perl bison flex
)
if [[ "$(id -u)" -eq 0 ]]; then
  say "Installing build dependencies"
  # An end-of-life Ubuntu MAY have moved from archive.ubuntu.com to
  # old-releases.ubuntu.com -- but not always, and not on a schedule worth
  # hard-coding. 18.04 is still served from archive today (it is in ESM), and
  # rewriting it unconditionally broke a working configuration with
  # "does not have a Release file" on all four repositories.
  #
  # So: try what the image ships with, and only rewrite if that actually fails.
  if ! apt-get update -qq 2>/dev/null; then
    say "apt-get update failed; retrying against old-releases.ubuntu.com"
    sed -i 's|archive.ubuntu.com|old-releases.ubuntu.com|g; s|security.ubuntu.com|old-releases.ubuntu.com|g' \
      /etc/apt/sources.list
    apt-get update -qq || die "apt-get update failed against both archive and old-releases"
  fi
  apt-get install -y -qq --no-install-recommends "${APT_PACKAGES[@]}" >/dev/null

  # 18.04 ships g++ 7, which cannot build this project: C++17 <filesystem> is
  # incomplete there and needs -lstdc++fs even where it exists. A newer
  # compiler from the toolchain PPA still links against the DISTRO's glibc, so
  # the resulting binary keeps 18.04 compatibility while the language support
  # is current. (Trying to keep the old compiler and work around <filesystem>
  # would mean changing the source for the oldest target's benefit.)
  if ! command -v g++-9 >/dev/null 2>&1; then
    GXX_MAJOR="$(g++ -dumpversion 2>/dev/null | cut -d. -f1 || echo 0)"
    if [[ "$GXX_MAJOR" -lt 9 ]]; then
      say "g++ $GXX_MAJOR is too old; installing g++-9 from the toolchain PPA"
      apt-get install -y -qq --no-install-recommends software-properties-common >/dev/null
      add-apt-repository -y ppa:ubuntu-toolchain-r/test >/dev/null 2>&1
      apt-get update -qq
      apt-get install -y -qq --no-install-recommends g++-9 gcc-9 >/dev/null
    fi
  fi
  if command -v g++-9 >/dev/null 2>&1; then
    export CC=gcc-9 CXX=g++-9
  fi
else
  MISSING=()
  for pkg in "${APT_PACKAGES[@]}"; do
    dpkg -s "$pkg" >/dev/null 2>&1 || MISSING+=("$pkg")
  done
  if [[ ${#MISSING[@]} -gt 0 ]]; then
    die "missing packages: ${MISSING[*]}
  install them with:  sudo apt-get install -y ${MISSING[*]}"
  fi
  say "All apt dependencies present"
fi

# --- 1b. CMake --------------------------------------------------------------
#
# 18.04 ships CMake 3.10. Two separate problems with that: CMakeLists.txt
# requires 3.20, and 3.10 predates the -S/-B flags (added in 3.13), so it reads
# `cmake -S /src -B /work/build` as "configure the source tree /work/build" and
# fails with the memorably wrong
#
#   The source directory "/work/build" does not appear to contain CMakeLists.txt
#
# Kitware publish static binaries; fetching one keeps the distro's glibc (which
# is the whole reason for building on 18.04) while giving a current CMake.
CMAKE_MIN_MAJOR=3
CMAKE_MIN_MINOR=20
cmake_too_old() {
  command -v cmake >/dev/null 2>&1 || return 0
  local version major minor
  version="$(cmake --version | head -1 | awk '{print $3}')"
  major="${version%%.*}"
  minor="$(echo "$version" | cut -d. -f2)"
  [[ "$major" -lt $CMAKE_MIN_MAJOR ]] && return 0
  [[ "$major" -eq $CMAKE_MIN_MAJOR && "$minor" -lt $CMAKE_MIN_MINOR ]] && return 0
  return 1
}

if cmake_too_old; then
  CMAKE_VERSION=3.28.6
  case "$(uname -m)" in
    x86_64)        CMAKE_ARCH=linux-x86_64 ;;
    aarch64|arm64) CMAKE_ARCH=linux-aarch64 ;;
    *) die "no prebuilt CMake for $(uname -m); install CMake >= 3.20 yourself" ;;
  esac
  say "CMake $(cmake --version 2>/dev/null | head -1 | awk '{print $3}' || echo none) is too old; fetching $CMAKE_VERSION"
  CMAKE_PREFIX=/opt/cmake
  mkdir -p "$CMAKE_PREFIX"
  curl -fsSL "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-${CMAKE_ARCH}.tar.gz" \
    | tar -xz --strip-components=1 -C "$CMAKE_PREFIX" \
    || die "could not download CMake $CMAKE_VERSION"
  export PATH="$CMAKE_PREFIX/bin:$PATH"
fi
say "CMake: $(cmake --version | head -1)"

# CMake's Ninja generator must be available before the vcpkg toolchain is
# loaded. Without this preflight, the manifest install obscures the real error
# with a later "CMAKE_MAKE_PROGRAM is not set" diagnostic.
if ! command -v ninja >/dev/null 2>&1; then
  if [[ "$(id -u)" -eq 0 ]]; then
    apt-get install -y -qq --no-install-recommends ninja-build >/dev/null
  fi
fi
command -v ninja >/dev/null 2>&1 || die "ninja-build is required for the Release build; install it or add it to PATH"
export CMAKE_MAKE_PROGRAM="$(command -v ninja)"

# --- 2. compiler ------------------------------------------------------------
CXX_BIN="${CXX:-g++}"
command -v "$CXX_BIN" >/dev/null || die "no C++ compiler ($CXX_BIN)"
CXX_VERSION="$("$CXX_BIN" --version | head -1)"
say "Compiler: $CXX_VERSION"
# The project is C++17 and uses <filesystem>, which GCC only made usable
# without -lstdc++fs from 9 onwards.
GCC_MAJOR="$("$CXX_BIN" -dumpversion | cut -d. -f1)"
if [[ "$CXX_BIN" == *g++* && "$GCC_MAJOR" -lt 9 ]]; then
  die "g++ $GCC_MAJOR is too old; this project needs C++17 <filesystem> (g++ 9+)"
fi

# --- 3. vcpkg ---------------------------------------------------------------
VCPKG_ROOT="${VCPKG_ROOT:-$REPO_ROOT/.vcpkg}"

# vcpkg.json pins `builtin-baseline` to a specific vcpkg commit, and manifest
# mode resolves every package version by running `git show <baseline>:
# versions/baseline.json` inside the vcpkg checkout. A `git clone --depth 1`
# only contains the tip, so that lookup fails with the memorable-but-unhelpful
#
#   fatal: path 'versions/baseline.json' exists on disk, but not in '39344df...'
#
# The file is right there; the COMMIT is not. Fetching that one commit
# (GitHub allows fetch-by-SHA) keeps the clone small while making the baseline
# resolvable.
BASELINE="$(sed -n 's/.*"builtin-baseline"[[:space:]]*:[[:space:]]*"\([0-9a-f]*\)".*/\1/p' "$REPO_ROOT/vcpkg.json")"

if [[ ! -d "$VCPKG_ROOT/.git" ]]; then
  say "Cloning vcpkg into $VCPKG_ROOT"
  git clone --depth 1 https://github.com/microsoft/vcpkg "$VCPKG_ROOT"
fi

if [[ -n "$BASELINE" ]]; then
  if ! git -C "$VCPKG_ROOT" cat-file -e "$BASELINE^{commit}" 2>/dev/null; then
    say "Fetching the pinned vcpkg baseline $BASELINE"
    git -C "$VCPKG_ROOT" fetch --depth 1 origin "$BASELINE" \
      || die "could not fetch the vcpkg baseline $BASELINE named in vcpkg.json"
  fi
  say "vcpkg baseline $BASELINE present"
  git -C "$VCPKG_ROOT" checkout --detach "$BASELINE"
fi

if [[ ! -x "$VCPKG_ROOT/vcpkg" ]]; then
  say "Bootstrapping vcpkg"
  "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
fi
export VCPKG_ROOT
say "vcpkg: $VCPKG_ROOT"

# --- 4. configure + build ---------------------------------------------------
# A CMakeCache.txt in the SOURCE directory aborts any out-of-source configure
# with "the current CMakeCache.txt directory ... is different than the directory
# ... where CMakeCache.txt was created", naming a path that may mean nothing to
# whoever reads it. This repo had one left from an in-source build on someone's
# Mac. Say what it is and how to clear it, rather than letting CMake explain.
if [[ -f "$REPO_ROOT/CMakeCache.txt" ]]; then
  die "$REPO_ROOT/CMakeCache.txt exists -- a stale in-source CMake configure.
  It makes every out-of-source build fail. Remove it and its CMakeFiles/:
      rm -rf CMakeCache.txt CMakeFiles cmake_install.cmake Makefile"
fi

say "Configuring Release into $BUILD_DIR"
rm -rf "$BUILD_DIR"

# Release-only dependencies. vcpkg builds BOTH a debug and a release variant of
# every port by default -- the log shows "Building x64-linux-dbg" then
# "Building x64-linux-rel" for each -- which doubles the time and the disk for
# artifacts a production build never links. A one-line overlay triplet is the
# supported way to turn that off.
TRIPLET_DIR="$BUILD_DIR/triplets"
mkdir -p "$TRIPLET_DIR"
cat > "$TRIPLET_DIR/x64-linux-release.cmake" <<'TRIPLET'
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_BUILD_TYPE release)
# vcpkg-make 2026-07-09 can drop its generated default configure options on
# Ubuntu 18 (observed natively under linux/arm64): autotools then falls back to
# /usr/local, so DESTDIR stages files under usr/local/ and pkg-config validation
# fails. Supplying the release prefix/linkage explicitly is harmless when the
# helper also supplies them and keeps make-based ports inside vcpkg's package
# staging layout when it does not.
set(VCPKG_MAKE_CONFIGURE_OPTIONS_RELEASE
    "--prefix=${CURRENT_INSTALLED_DIR}"
    "--disable-shared"
    "--enable-static")
set(VCPKG_MAKE_OPTIONS_RELEASE "prefix=${CURRENT_INSTALLED_DIR}")
TRIPLET
cat > "$TRIPLET_DIR/arm64-linux-release.cmake" <<'TRIPLET'
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_BUILD_TYPE release)
set(VCPKG_MAKE_CONFIGURE_OPTIONS_RELEASE
    "--prefix=${CURRENT_INSTALLED_DIR}"
    "--disable-shared"
    "--enable-static")
set(VCPKG_MAKE_OPTIONS_RELEASE "prefix=${CURRENT_INSTALLED_DIR}")
TRIPLET

case "$(uname -m)" in
  x86_64)        VCPKG_TRIPLET="x64-linux-release" ;;
  aarch64|arm64) VCPKG_TRIPLET="arm64-linux-release" ;;
  armv7l|armv8l)
    VCPKG_TRIPLET="arm-linux-release"
    sed 's/VCPKG_TARGET_ARCHITECTURE arm64/VCPKG_TARGET_ARCHITECTURE arm/' \
      "$TRIPLET_DIR/arm64-linux-release.cmake" > "$TRIPLET_DIR/arm-linux-release.cmake"
    ;;
  *) die "unsupported architecture $(uname -m)" ;;
esac
say "vcpkg triplet: $VCPKG_TRIPLET (release only)"
# -O2 -DNDEBUG come from CMAKE_BUILD_TYPE=Release. No -march=native: section
# 3.3 is explicit, and a binary tuned for this builder would fault on an older
# target CPU with an illegal instruction, which looks like a corrupt deploy.
# -static-libstdc++ -static-libgcc removes the OTHER half of the portability
# problem. The failed 18.04 deploy needed GLIBCXX_3.4.26/29 and CXXABI_1.3.13
# as well as newer glibc: libstdc++ is versioned the same way, and a newer
# compiler emits references the target's older runtime does not have. Linking
# it in means the only remaining external ABI is glibc, which building on
# 18.04 already pins. Costs ~2 MB.
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DHSF_ENABLE_ZK=OFF \
  -DHSF_BUILD_PLUGIN_SDK=ON \
  -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
  -DVCPKG_TARGET_TRIPLET="$VCPKG_TRIPLET" \
  -DVCPKG_OVERLAY_TRIPLETS="$TRIPLET_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"

say "Building with $JOBS jobs"
cmake --build "$BUILD_DIR" -j "$JOBS"

BINARY="$BUILD_DIR/hsf_gateway"
[[ -x "$BINARY" ]] || die "no binary at $BINARY"

# --- 5. verify --------------------------------------------------------------
say "Verifying the binary"
"$BINARY" --version || die "the built binary will not run"

# PullSDK is Windows-x86 only, so a Linux build must fall back to the C3
# backend -- which is a working ZK path, not a disabled one. Asserting the old
# "ZK controller: disabled" wording here would now fail a correct build; what
# still matters is that the PullSDK option did not leak in.
"$BINARY" --version | grep -q 'ZK controller: C3 over TCP' \
  || die "expected a C3-backend (PullSDK-free) ZK build on Linux"
# libsodium is a hard dependency now (authentication cannot work without it),
# and OpenSSL gates update signature verification. Both should be real here.
"$BINARY" --version | grep -q 'Update signatures: OpenSSL' \
  || echo "[build] WARNING: no OpenSSL -- OTA updates will refuse to install"

# --- 6. tests ---------------------------------------------------------------
if [[ $RUN_TESTS -eq 1 ]]; then
  say "Running gateway and plugin unit tests"
  ctest --test-dir "$BUILD_DIR" --output-on-failure --no-tests=error --timeout 120
  say "Running the security suite"
  bash ./scripts/security-test.sh "$BINARY" 18080 || die "security tests failed"
  say "Running the Lua packaging suite"
  bash ./scripts/package-lua-test.sh "$BINARY" 18090 || die "Lua packaging tests failed"
  if [[ -f ./scripts/validate-lua.sh ]]; then
    say "Validating Lua scripts"
    bash ./scripts/validate-lua.sh || die "Lua validation failed"
  fi
else
  say "Skipping tests (--no-tests)"
fi

# --- 7. build metadata (section 3.3) ----------------------------------------
BUILD_INFO="$BUILD_DIR/BUILD_INFO.txt"
{
  echo "Built:            $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "Host:             ${PRETTY_NAME:-unknown}"
  echo "Architecture:     $(uname -m)"
  echo "Build type:       Release (-O2 -DNDEBUG, no -march=native)"
  echo "Compiler:         $CXX_VERSION"
  echo "C++ standard:     17"
  echo "Git commit:       $(git -C "$REPO_ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  echo "Version:          $(head -n1 "$REPO_ROOT/VERSION" | tr -d '[:space:]')"
  echo
  echo "--- gateway --version ---"
  "$BINARY" --version
  echo
  echo "--- vcpkg dependencies ---"
  "$VCPKG_ROOT/vcpkg" list 2>/dev/null | sed 's/^/  /' || echo "  (vcpkg list unavailable)"
} > "$BUILD_INFO"
say "Build metadata: $BUILD_INFO"

say "Build successful: $BINARY"
