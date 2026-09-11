# Update safety verification

CI now checks the packaging and OTA contract on every Rust cross-platform run.
The check requires both package flows to invoke release validation, documents
database/credential exclusion, and confirms that the installer retains a
versioned release/rollback layout. It also checks that the release manifest
builder emits platform, checksum, and signature metadata.

This is a static contract check; it does not download or activate a release.
Runtime health checks and rollback tests still require a managed installation
environment.
