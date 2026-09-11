# Toolchains

`linux-arm64.cmake` describes the target and accepts compiler paths through
`CC` and `CXX`. The profile intentionally does not hard-code a vendor SDK;
the CI or target image supplies the appropriate cross compiler and sysroot.
