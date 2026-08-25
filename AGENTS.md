# Repository Guidelines

## Project Structure & Module Organization

The gateway is a C++17 application. Keep implementation files in `src/` and
public headers in `include/hsf/`, mirroring subdomains such as `security/`,
`update/`, `zk_controller/`, and `plugin_manager/`. Static dashboard assets
live in `web/` (`css/`, `js/`, HTML pages); runtime defaults and schemas live
in `config/`. Use `sdk/hsf-plugin-sdk/` for the plugin ABI and its tests, and
`plugins/template-cpp/` as the reference for new native plugins. Deployment,
build, packaging, and integration checks belong in `scripts/`.

## Build, Test, and Development Commands

- Windows: run `build.bat` from the repository root to bootstrap/configure and
  build with vcpkg and Visual Studio tools. Run the result with
  `scripts\\run.ps1` (or `build-win\\Release\\hsf_gateway.exe config\\config.db`).
- Cross-platform: configure with
  `cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`,
  then build with `cmake --build build --config Release`.
- Linux and release workflows are wrapped by `scripts/build.sh`,
  `scripts/build-ubuntu.sh`, and `scripts/package.sh`; use them instead of
  manually assembling release artifacts.

## Coding Style & Naming Conventions

Use C++17 and follow the surrounding code: four-space indentation, braces on
the same line as declarations, `PascalCase` classes/functions, and
`camelCase_` private members. Match a component's header and source names
(for example, `include/hsf/ModbusClient.h` and `src/ModbusClient.cpp`). Keep
platform-specific code in `_win.cpp` or `_posix.cpp` files. Preserve line
endings: `.sh` and `.lua` use LF; `.bat` and `.ps1` use CRLF. No repository
formatter is configured, so avoid unrelated reformatting.

## Testing Guidelines

Build before testing. Run SDK/plugin CTest suites with
`ctest --test-dir build --output-on-failure`. For gateway integration coverage,
use `scripts/security-test.sh <gateway-binary> <port>` and
`scripts/package-lua-test.sh <gateway-binary> <port>` on a POSIX environment.
Add focused tests beside the component being changed; use descriptive names
such as `test_manifest.cpp` or `example_driver_tests`.

## Commit & Pull Request Guidelines

Recent commits use concise, component-led subjects, e.g. `Plugin SDK: add ABI
tests` or `API authentication, RBAC and abuse protection`. Keep each commit
focused and explain behavior changes in the body when needed. Pull requests
should state the affected subsystem, validation commands/results, configuration
or migration impacts, and linked issue. Include screenshots for dashboard
changes and never commit keys, databases, or generated build outputs.
