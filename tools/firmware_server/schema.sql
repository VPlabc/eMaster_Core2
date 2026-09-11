PRAGMA foreign_keys = ON;
CREATE TABLE IF NOT EXISTS devices (
    device_id TEXT PRIMARY KEY,
    product TEXT NOT NULL,
    platform TEXT NOT NULL,
    architecture TEXT NOT NULL,
    update_channel TEXT NOT NULL CHECK (update_channel IN ('dev', 'beta', 'stable')),
    enrollment_hash TEXT UNIQUE,
    token_hash TEXT UNIQUE,
    created_at TEXT NOT NULL,
    registered_at TEXT
);
CREATE TABLE IF NOT EXISTS firmware_releases (
    release_id TEXT PRIMARY KEY,
    product TEXT NOT NULL,
    version TEXT NOT NULL,
    platform TEXT NOT NULL,
    architecture TEXT NOT NULL,
    channel TEXT NOT NULL,
    status TEXT NOT NULL CHECK (status IN ('available', 'withdrawn')),
    sha256 TEXT NOT NULL,
    manifest_json TEXT NOT NULL,
    created_at TEXT NOT NULL,
    UNIQUE (product, version, platform, architecture)
);
CREATE TABLE IF NOT EXISTS firmware_artifacts (
    release_id TEXT PRIMARY KEY REFERENCES firmware_releases(release_id),
    storage_url TEXT NOT NULL UNIQUE,
    size INTEGER NOT NULL CHECK (size > 0),
    filename TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS release_lookup ON firmware_releases
    (product, platform, architecture, channel, status);
CREATE TABLE IF NOT EXISTS update_history (
    history_id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT REFERENCES devices(device_id),
    release_id TEXT REFERENCES firmware_releases(release_id),
    action TEXT NOT NULL,
    status TEXT NOT NULL,
    http_status INTEGER NOT NULL,
    timestamp TEXT NOT NULL
);
PRAGMA user_version = 1;
