# Repository cleanup policy

Build and release output is isolated from source under `build-*`,
`build-presets/`, `target/`, `dist/`, and `release/`. Runtime databases and
SQLite WAL sidecars under `config/` are local state and are ignored, along
with generated credentials and package keys.

This phase changes ignore policy only. It deliberately does not delete tracked
files or runtime state; removal requires reference and deployment verification
first.
