# Release manifest

Windows and POSIX package scripts now write `manifest.json` into the staged
runtime directory before creating the archive. It records the product,
semantic version, UTC build timestamp, platform, architecture, Git commit, and
release build type.

The same staging directory is validated immediately before archiving. The
manifest is metadata only; OTA authenticity remains provided by the existing
signed `version.json` workflow and package checksum verification.

CI additionally publishes an external `{archive}.manifest.json`, defined by
`config/release-manifest.schema.json`, for Firmware Server import. This sidecar
contains the final archive SHA256, byte size, Actions build ID, full commit,
channel and audit metadata. The embedded manifest cannot carry the SHA256 of
its enclosing archive. See `docs/CI_CD.md` for the release and import contract.
