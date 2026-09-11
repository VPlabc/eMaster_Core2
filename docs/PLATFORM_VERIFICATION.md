# Platform profile verification

CI validates that the repository contains the five supported configure
profiles: Windows x86, Windows x64, Windows 11 IoT, Ubuntu 18, Ubuntu 22, and
Linux ARM64.
It also checks the important architecture rules: ZKTeco support is enabled
only for Windows x86, and ARM64 selects a toolchain while disabling ZK.

Rust workspace testing continues on Windows and Ubuntu x86_64. ARM64 runs a
cross-target `cargo check`; Ubuntu 18 and hardware runtime verification remain
pending until matching environments are available.

Linux release packaging accepts only `linux-x64` and `linux-arm64`. Explicit
targets are checked against the host architecture before packaging, preventing
an x86_64 binary from being mislabeled as an ARM64 release. ARM64 packages
must be built on an ARM64 host or through a dedicated cross-build profile.

Windows 11 IoT has a separate contract: x64, ZK disabled, and the native
plugin SDK disabled by default. Runtime verification remains explicitly
pending until an IoT SDK/device environment is available.
