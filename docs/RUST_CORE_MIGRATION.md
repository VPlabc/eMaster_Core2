# Rust Core Skeleton

The Rust workspace is intentionally additive. The existing C++ gateway remains
the behavioral reference and continues to build unchanged. The initial crates
provide only platform-neutral contracts:

- `emaster-config`: owned key/value configuration storage.
- `emaster-event`: cloneable event bus using message passing.
- `emaster-core`: explicit core lifecycle and shared services.
- `emaster-runtime`: lifecycle-safe runtime identity management.
- `emaster-plugin`: manifest validation and registry primitives.

No hardware SDK, operating-system API, FFI, or `unsafe` code is introduced in
this phase. `emaster-core` now owns the event bus and composes the runtime and
plugin registries, with explicit start/stop events and lifecycle tests. Driver
and legacy C ABI work starts only after these contracts are stable and tested.

Phase 1 validation currently covers the developer host with `cargo test
--workspace` and `cargo clippy --workspace --all-targets -- -D warnings`.
Linux x86_64 and ARM64 release builds remain target-matrix work; the existing
C++ gateway remains the behavioral reference during this phase.

## Phase 2 boundary

`include/gateway/c3.h` and `src/ffi/c3.cpp` provide the first stable C ABI
surface over the existing C3 codec. It exposes CRC, frame sizing, session-less
connect encoding, and generic reply status without exposing STL, exceptions, or
vendor handles. Returned buffers are allocated and released through the same C
allocator API. The opaque `C3Client` handle now also covers connect,
disconnect, control, device-parameter reads, and caller-buffer error retrieval;
the implementation remains in C++ while ownership and buffer contracts are
covered by focused ABI tests.

## Phases 3–6 boundaries

The next contract layer is now available as additive crates:

- `emaster-modbus`: Modbus TCP holding-register read and single-register write
  codecs with strict response validation and a transport trait.
- `emaster-serial`: platform-neutral serial configuration plus an explicit
  open/close/read/write lifecycle, with a deterministic memory port for tests.
- `emaster-http`: request validation and a service dispatch boundary returning
  structured HTTP status responses before application handlers run.
- `emaster-rabbitmq`: validated event-bus publishing plus explicit
  connected/disconnected/reconnect state.
- `emaster-lua`: controlled host-call boundary with blocked filesystem/process
  modules (`os` and `io`) and an argument-size execution limit.

These crates deliberately contain no hardware, broker, HTTP server, or Lua VM
dependencies. The existing C++ implementations remain the behavioral
reference while platform adapters and integration tests are introduced in
later migration steps. The phase 3–6 gate is `cargo test --workspace` plus
`cargo clippy --workspace --all-targets -- -D warnings`.

## Phase 7 boundary

`emaster-plugin` now provides an in-memory lifecycle registry with manifest
validation and explicit `install`, `enable`, `disable`, `update`, and `remove`
operations. Enabled plugins cannot be removed, duplicate IDs are rejected, and
updates preserve the current lifecycle state. Package archive extraction,
filesystem installation, native loading, and the stable plugin C ABI remain
separate follow-up work.

## Phases 8–9 boundary

The C++ retention and migration choices are recorded in
`docs/CPP_MIGRATION_DECISIONS.md`. Stable C3 and vendor SDK code remains behind
the C ABI, while portable orchestration is the Rust migration target. A GitHub
Actions workflow validates Rust on Windows x64 and Linux x64 and performs an
ARM64 cross-target check. Hardware and native SDK tests remain deployment or
target-device checks and are not marked passing by CI alone.
