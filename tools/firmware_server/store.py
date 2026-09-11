"""SQLite state and immutable imports; firmware metadata always comes from CI."""
from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import sqlite3
import tempfile
import uuid

from jsonschema import Draft202012Validator, FormatChecker
from jsonschema.exceptions import ValidationError
from scripts.ci.release import SEMVER, channel, validate_metadata


SCHEMA = Path(__file__).resolve().parents[2] / "config/release-manifest.schema.json"
MANIFEST_LIMIT = 64 * 1024
ARTIFACT_LIMIT = 512 * 1024 * 1024


def now():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def token_hash(token):
    return hashlib.sha256(token.encode("utf-8")).hexdigest()


def version_key(version):
    if not isinstance(version, str) or len(version) > 200:
        raise ValueError("Invalid semantic version")
    match = SEMVER.fullmatch(version)
    if not match:
        raise ValueError("Invalid semantic version")
    prerelease = match.group(4)
    identifiers = tuple((0, int(p)) if p.isdigit() else (1, p) for p in prerelease.split(".")) if prerelease else ()
    return (int(match[1]), int(match[2]), int(match[3]), prerelease is None, identifiers)


def validate_identity(data):
    if not isinstance(data, dict):
        raise ValueError("Expected a JSON object")
    if not isinstance(data.get("device_id"), str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}", data["device_id"]):
        raise ValueError("Invalid device_id")
    if data.get("product") != "eMaster":
        raise ValueError("Unsupported product")
    targets = {"windows": {"x86", "x64", "arm64"}, "linux": {"x64", "arm64", "armhf"}, "macos": {"x64", "arm64"}}
    platform, architecture = data.get("platform"), data.get("architecture")
    if not isinstance(platform, str) or not isinstance(architecture, str) or architecture not in targets.get(platform, set()):
        raise ValueError("Unsupported platform/architecture")


class Conflict(Exception):
    pass


class Store:
    def __init__(self, root):
        self.root = Path(root).resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.artifacts = self.root / "artifacts"
        self.artifacts.mkdir(exist_ok=True)
        self.database = self.root / "firmware.db"
        schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
        Draft202012Validator.check_schema(schema)
        self.validator = Draft202012Validator(schema, format_checker=FormatChecker())
        with self.connect() as db:
            version = db.execute("PRAGMA user_version").fetchone()[0]
            if version not in (0, 1):
                raise ValueError("Unsupported firmware database schema version")
            db.execute("PRAGMA journal_mode=WAL")
            db.executescript(Path(__file__).with_name("schema.sql").read_text())

    @contextmanager
    def connect(self):
        db = sqlite3.connect(self.database, timeout=30)
        db.row_factory = sqlite3.Row
        db.execute("PRAGMA foreign_keys=ON")
        try:
            with db:
                yield db
        finally:
            db.close()

    def enroll(self, identity, update_channel):
        validate_identity(identity)
        if update_channel not in ("dev", "beta", "stable"):
            raise ValueError("Invalid update channel")
        token = secrets.token_urlsafe(32)
        try:
            with self.connect() as db:
                db.execute("INSERT INTO devices (device_id, product, platform, architecture, update_channel, enrollment_hash, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
                           (identity["device_id"], identity["product"], identity["platform"], identity["architecture"], update_channel, token_hash(token), now()))
        except sqlite3.IntegrityError as error:
            raise Conflict("Device already provisioned") from error
        return token

    def register(self, identity, enrollment):
        validate_identity(identity)
        token = secrets.token_urlsafe(32)
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT * FROM devices WHERE device_id=? AND enrollment_hash=? AND token_hash IS NULL", (identity["device_id"], token_hash(enrollment))).fetchone()
            if row is None:
                return None
            if any(row[field] != identity[field] for field in ("product", "platform", "architecture")):
                raise PermissionError("Enrollment identity mismatch")
            if "update_channel" in identity and identity["update_channel"] != row["update_channel"]:
                raise PermissionError("Enrollment channel mismatch")
            db.execute("UPDATE devices SET token_hash=?, enrollment_hash=NULL, registered_at=? WHERE device_id=?", (token_hash(token), now(), row["device_id"]))
            return dict(device_id=row["device_id"], token=token, update_channel=row["update_channel"])

    def authenticate(self, token):
        with self.connect() as db:
            return db.execute("SELECT * FROM devices WHERE token_hash=? AND registered_at IS NOT NULL", (token_hash(token),)).fetchone()

    def parse_manifest(self, raw):
        if len(raw) > MANIFEST_LIMIT:
            raise ValueError("Manifest exceeds 64 KiB")
        try:
            metadata = json.loads(raw.decode("utf-8-sig"))
            self.validator.validate(metadata)
            validate_metadata(metadata)
            version_key(metadata["version"])
            # A plain version may be published to dev by PR CI. Prerelease
            # versions must never be advertised as stable.
            if metadata["channel"] == "stable" and channel(metadata["version"]) != "stable":
                raise ValueError("Stable releases cannot contain prerelease versions")
            validate_identity(dict(metadata, device_id="manifest"))
        except (UnicodeError, ValueError, TypeError, KeyError, ValidationError) as error:
            raise ValueError("Invalid CI manifest") from error
        return metadata

    def import_artifact(self, raw, stream, filename):
        metadata = self.parse_manifest(raw)
        if filename != metadata["filename"]:
            raise ValueError("Upload filename does not match manifest")
        if metadata["size_bytes"] > ARTIFACT_LIMIT:
            raise ValueError("Artifact exceeds 512 MiB")
        release_id = uuid.uuid4().hex
        target = self.artifacts / (release_id + ".blob")
        temporary = None
        moved = False
        try:
            with tempfile.NamedTemporaryFile(dir=self.artifacts, suffix=".upload", delete=False) as output:
                temporary = Path(output.name)
                digest = hashlib.sha256()
                size = 0
                while block := stream.read(1024 * 1024):
                    size += len(block)
                    if size > metadata["size_bytes"]:
                        raise ValueError("Artifact size mismatch")
                    output.write(block)
                    digest.update(block)
                if size != metadata["size_bytes"] or digest.hexdigest() != metadata["sha256"]:
                    raise ValueError("Artifact checksum or size mismatch")
                output.flush()
                os.fsync(output.fileno())
            # Serialize publication with the DB uniqueness constraint. A crash
            # before commit can leave an unreachable blob, never a visible
            # release pointing at a partially copied upload.
            with self.connect() as db:
                db.execute("BEGIN IMMEDIATE")
                db.execute("INSERT INTO firmware_releases VALUES (?, ?, ?, ?, ?, ?, 'available', ?, ?, ?)",
                           (release_id, metadata["product"], metadata["version"], metadata["platform"], metadata["architecture"], metadata["channel"], metadata["sha256"], raw.decode("utf-8-sig"), now()))
                # UUID names are server controlled; an exclusive target prevents
                # accidental replacement even if an identifier collides.
                with target.open("xb") as output, temporary.open("rb") as source:
                    moved = True
                    shutil.copyfileobj(source, output, 1024 * 1024)
                    output.flush()
                    os.fsync(output.fileno())
                db.execute("INSERT INTO firmware_artifacts VALUES (?, ?, ?, ?)", (release_id, target.name, size, filename))
                db.execute("INSERT INTO update_history (release_id, action, status, http_status, timestamp) VALUES (?, 'upload', 'imported', 201, ?)", (release_id, now()))
            moved = False  # Committed blobs must be retained.
        except sqlite3.IntegrityError as error:
            raise Conflict("Release already exists; artifacts are immutable") from error
        finally:
            if temporary:
                temporary.unlink(missing_ok=True)
            if moved:
                target.unlink(missing_ok=True)
        return release_id, metadata

    def latest(self, device, current):
        current_key = version_key(current)
        with self.connect() as db:
            rows = db.execute("SELECT r.*, a.size, a.filename FROM firmware_releases r JOIN firmware_artifacts a USING (release_id) WHERE r.product=? AND r.platform=? AND r.architecture=? AND r.channel=? AND r.status='available' ORDER BY r.created_at, r.release_id", tuple(device[field] for field in ("product", "platform", "architecture", "update_channel"))).fetchall()
        newer = [row for row in rows if version_key(row["version"]) > current_key]
        return max(newer, key=lambda row: version_key(row["version"]), default=None)

    def artifact(self, release_id):
        with self.connect() as db:
            return db.execute("SELECT r.*, a.storage_url, a.size, a.filename FROM firmware_releases r JOIN firmware_artifacts a USING (release_id) WHERE release_id=? AND status='available'", (release_id,)).fetchone()

    def history(self, device_id, release_id, action, status, http_status):
        with self.connect() as db:
            db.execute("INSERT INTO update_history (device_id, release_id, action, status, http_status, timestamp) VALUES (?, ?, ?, ?, ?, ?)", (device_id, release_id, action, status, http_status, now()))
