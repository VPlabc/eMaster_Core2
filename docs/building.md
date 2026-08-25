# Building from source

## Prerequisites

- CMake 3.20+
- A C++17 compiler (MSVC, GCC or Clang)
- [vcpkg](https://vcpkg.io) — dependencies are declared in `vcpkg.json`
  (manifest mode): `curl`, `nlohmann-json`, `lua`, `asio`, `crow`, `sqlite3`
- Git (optional; without it the build records the commit as `unknown`)

Set `VCPKG_ROOT`, or clone vcpkg to `build/vcpkg`, which is where the build
scripts look by default.

## The one build option that matters: `HSF_ENABLE_ZK`

`src/zk_controller/` links ZKTeco's PullSDK (`plcommpro.dll`) in-process. That
DLL is a **32-bit Windows binary**, so enabling it forces the entire gateway
to x86.

| | `HSF_ENABLE_ZK` | Result |
|---|---|---|
| Windows x86 | `ON` (default) | Full build, ZK card reading works |
| Windows x64 | `OFF` (default) | Builds; `zk.*` calls fail |
| Linux / macOS | `OFF` (default) | Builds; `zk.*` calls fail |

The default is chosen automatically, so you normally pass nothing. Turning it
`ON` anywhere other than 32-bit Windows is a configure-time `FATAL_ERROR`
rather than a confusing link failure later.

With it `OFF`, `src/zk_controller/PullSdkClient_stub.cpp` is compiled instead
of the real client. The `zk_controller` Lua module is still present and every
function still exists — calls just return failure with
`NotSupportedOnThisPlatform`. Keeping the API shape identical means a script
gets an honest error rather than `attempt to index a nil value`.

## Windows

```bat
build.bat
```

That configures `build-win/` as Win32/x86 with the `x86-windows` triplet and
builds Release. Equivalent by hand:

```powershell
cmake -S . -B build-win -A Win32 `
  -DVCPKG_TARGET_TRIPLET=x86-windows `
  -DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake
cmake --build build-win --config Release
```

For an x64 Windows build (no ZK):

```powershell
cmake -S . -B build-win64 -A x64 `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DHSF_ENABLE_ZK=OFF `
  -DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake
cmake --build build-win64 --config Release
```

## Linux / macOS

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build -j"$(nproc)"
./build/hsf_gateway
```

`HSF_ENABLE_ZK` defaults to `OFF` here, so nothing else is needed.

## Running from the build tree

The gateway searches for `web/` and `config/` in this order:

1. beside the executable
2. `../share/hsf_gateway/` (a `cmake --install` prefix)
3. `../../` (which is what makes `build-win/Release/` find the source tree)
4. the configure-time source paths compiled in as a last resort

So running straight out of the build directory works, and a packaged binary
never falls back to the machine it was built on. See `ResolveResourceDir()` in
`src/main.cpp`.

## Verifying a build

```bash
./build/hsf_gateway --version
```

```text
eMaster Gateway
Version:  1.0.0
Commit:   41a4139
Built:    2026-08-10T08:34:53Z
Platform: windows-x86
Compiler: MSVC 19.51.36248.0
ZK controller: enabled
```

A `-dirty` suffix on the commit means the working tree had uncommitted changes
when CMake configured. Release packages must never carry it — `scripts/package.*`
always configures a clean build directory, but it cannot make your working
tree clean for you.

## Packaging

```powershell
.\scripts\package.ps1 -Arch x86
```

```bash
./scripts/package.sh
```

Both wipe and reconfigure their build directory, stage the package, refuse to
include any `*.db` or `config.json`, write `BUILD_INFO.txt`, and append a
SHA-256 line to `dist/SHA256SUMS`.
