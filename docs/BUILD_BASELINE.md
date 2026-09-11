# Build Baseline

Date: 2026-08-29

## Scope

This document records the repository's current build baselines and the scripts
that define them. It is a factual inventory for the Core C / C++ migration, not
the target end-state.

## Current release baselines

### Windows 10

- Status: current working baseline
- Entry script: `build.bat`
- Generator/toolchain: CMake + Visual Studio C++ workload
- Architecture baseline: `Win32` / x86
- vcpkg triplet: `x86-windows`
- Key reason for x86: `plcommpro.dll` PullSDK integration for ZK support is
  32-bit only
- Output directory: `build-win`
- Runtime invocation: `scripts\run.ps1` or `build-win\Release\hsf_gateway.exe`

### Ubuntu 22.04

- Status: current Linux reference baseline
- Entry scripts: `scripts/build.sh`, `scripts/build-ubuntu.sh`
- Generator/toolchain: CMake + GCC/Clang + vcpkg
- Architecture baseline: native host architecture, typically `x64-linux`
- ZK support: forced off in Linux builds; C3 backend remains available
- Output directories: `build-linux`, `build-ubuntu`

## Porting targets, not current baselines

- Ubuntu 18.04
- Windows 11 IoT

These require dedicated target profiles and validation gates. They should not be
treated as equivalent to the current Windows 10 / Ubuntu 22 build paths.

## Repository-defined build entry points

### Windows

- `build.bat`
  - bootstraps vcpkg if needed
  - configures with `--fresh`
  - selects `Win32`
  - uses `x86-windows`

### Cross-platform Linux/developer

- `scripts/build.sh`
  - native incremental build
  - uses `build-linux`
  - infers `x64-linux` / `arm64-linux` / `arm-linux`
  - keeps `HSF_ENABLE_ZK=OFF`

### Ubuntu compatibility / release

- `scripts/build-ubuntu.sh`
  - supports native Ubuntu or `--docker`
  - defaults container userspace to `ubuntu:18.04`
  - installs newer GCC and CMake on old Ubuntu when needed
  - runs verification and integration scripts
  - packages portability assumptions explicitly

### Packaging

- `scripts/package.sh`
  - clean release packaging for Linux/macOS style layouts
  - excludes live `.db` and `config.json`
  - stages `bin/`, `web/`, `config/scripts`, docs, and install helpers

## Build-system facts from `CMakeLists.txt`

- Minimum CMake: `3.20`
- Language level: `C++17`
- Main target: `hsf_gateway`
- Plugin SDK build: optional via `HSF_BUILD_PLUGIN_SDK`
- Optional integrations with stubs:
  - RabbitMQ via `amqpcpp`
  - OpenSSL update-signature verification
  - PullSDK ZK implementation

## Observed local shell state on 2026-08-29

- `cmake` was not on `PATH` in the current command shell
- `cl` was not on `PATH` in the current command shell

That means a raw `cmake` or `cl` invocation is not itself a reliable baseline
check in this workspace. The authoritative Windows baseline remains `build.bat`,
which locates CMake via PATH or Visual Studio.
