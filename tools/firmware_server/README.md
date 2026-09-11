# Firmware Server MVP

This service consumes immutable eMaster archives and sidecar manifests from CI. It does not build artifacts or invent release metadata. The MVP uses SQLite for device and release state and a filesystem directory for exact archive bytes.

Install dependencies with `python -m pip install -r tools/firmware_server/requirements.txt`, set `FW_ADMIN_TOKEN` to a random value of at least 32 characters, then run:

```text
python -m tools.firmware_server --state-dir build-firmware-server/state serve --host 127.0.0.1 --port 8092
```

Provision a device with the one-time enrollment command:

```text
python -m tools.firmware_server --state-dir build-firmware-server/state enroll --device-id GW001 --platform linux --architecture arm64 --channel stable
```

The device uses the returned enrollment token once at `POST /api/v1/device/register`; the returned bearer token is required for checks and downloads. Tokens are stored only as SHA256 hashes.

Administrators upload the CI manifest and matching archive as multipart parts named `manifest` and `artifact` to `POST /api/v1/firmware/upload`. The server validates `config/release-manifest.schema.json`, recomputes SHA256 and size, rejects duplicate target releases, fsyncs the blob, and records the import. It never overwrites a published release.

Devices call `POST /api/v1/firmware/check` with their identity and current version. A newer matching release returns its ID, version, download URL, SHA256, and size. Identity, architecture, channel, and bearer token are bound together. Checks and downloads are recorded in `update_history`.

Run `python -m unittest tools.firmware_server.test_server -v` for the 17 API tests, or use `scripts/firmware_server_smoke.py` for the real HTTP check/download/SHA256 flow. The existing gateway currently consumes the static `version.json` protocol documented in `docs/ota-update.md`; a later client adapter can consume this POST API directly or expose a compatibility endpoint.
