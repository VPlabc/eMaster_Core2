"""Authenticated firmware import, enrollment, update checks and downloads."""
import hashlib
import hmac
import os
import re
import sqlite3

from flask import Flask, g, jsonify, request, send_file, url_for
from werkzeug.exceptions import HTTPException

from .store import ARTIFACT_LIMIT, MANIFEST_LIMIT, Conflict, Store, token_hash, validate_identity


def create_app(state_dir=None, admin_token=None):
    admin_token = admin_token if admin_token is not None else os.environ.get("FW_ADMIN_TOKEN", "")
    if len(admin_token) < 32:
        raise ValueError("FW_ADMIN_TOKEN must contain at least 32 characters")
    admin_hash = token_hash(admin_token)
    store = Store(state_dir or os.environ.get("FW_STATE_DIR", "build-firmware-server/state"))
    app = Flask(__name__)
    app.config.update(MAX_CONTENT_LENGTH=ARTIFACT_LIMIT + 1024 * 1024,
                      MAX_FORM_MEMORY_SIZE=MANIFEST_LIMIT, MAX_FORM_PARTS=4)
    app.extensions["firmware_store"] = store

    def error(message, status):
        return jsonify(error=message), status

    @app.before_request
    def authenticate():
        g.device = None
        g.release_id = None
        g.audit_status = None
        if request.endpoint != "upload":
            request.max_content_length = MANIFEST_LIMIT
        if request.endpoint in ("check", "download", "upload", "register"):
            scheme, _, token = request.headers.get("Authorization", "").partition(" ")
            if scheme.lower() != "bearer" or not 32 <= len(token) <= 256:
                return error("Bearer token required", 401)
            g.token = token
            if request.endpoint == "upload":
                if not hmac.compare_digest(token_hash(token), admin_hash):
                    return error("Administrator authentication required", 403)
            elif request.endpoint != "register":
                g.device = store.authenticate(token)
                if g.device is None:
                    return error("Device authentication failed", 401)

    @app.after_request
    def record_history(response):
        response.headers["Cache-Control"] = "no-store"
        response.headers["X-Content-Type-Options"] = "nosniff"
        if request.endpoint in ("check", "download"):
            # A download entry records the request/response, not installation or
            # successful receipt by the device. Those reports belong to Phase 2.
            store.history(g.device["device_id"] if g.get("device") else None,
                          g.get("release_id"), request.endpoint,
                          g.get("audit_status") or f"http_{response.status_code}", response.status_code)
        return response

    @app.errorhandler(HTTPException)
    def http_error(exc):
        return error(exc.name, exc.code)

    @app.errorhandler(ValueError)
    def validation_error(exc):
        return error(str(exc), 400)

    @app.errorhandler(Conflict)
    def conflict_error(exc):
        return error(str(exc), 409)

    @app.errorhandler(PermissionError)
    def permission_error(exc):
        return error(str(exc), 403)

    @app.errorhandler(sqlite3.OperationalError)
    def database_error(exc):
        app.logger.error("Firmware database unavailable")
        return error("Firmware database unavailable", 503)

    @app.get("/healthz")
    def health():
        with store.connect() as db:
            db.execute("SELECT 1").fetchone()
        return jsonify(status="ok")

    @app.post("/api/v1/device/register")
    def register():
        identity = request.get_json()
        result = store.register(identity, g.token)
        if result is None:
            return error("Invalid or already used enrollment token", 401)
        return jsonify(result), 201

    @app.post("/api/v1/firmware/upload")
    def upload():
        if set(request.files) != {"manifest", "artifact"} or request.form:
            raise ValueError("Upload requires manifest and artifact file parts")
        if any(len(request.files.getlist(key)) != 1 for key in request.files):
            raise ValueError("Duplicate upload parts")
        raw = request.files["manifest"].stream.read(MANIFEST_LIMIT + 1)
        artifact = request.files["artifact"]
        release_id, metadata = store.import_artifact(raw, artifact.stream, artifact.filename)
        return jsonify(release_id=release_id, manifest=metadata), 201

    @app.post("/api/v1/firmware/check")
    def check():
        data = request.get_json()
        validate_identity(data)
        if any(data[field] != g.device[field] for field in ("device_id", "product", "platform", "architecture")):
            raise PermissionError("Request identity does not match registered device")
        if "update_channel" in data and data["update_channel"] != g.device["update_channel"]:
            raise PermissionError("Device channel cannot be overridden")
        release = store.latest(g.device, data.get("current_version"))
        if release is None:
            g.audit_status = "no_update"
            return jsonify(update_available=False)
        g.release_id = release["release_id"]
        g.audit_status = "update_available"
        return jsonify(update_available=True, release_id=release["release_id"],
                       version=release["version"], download_url=url_for("download", release_id=release["release_id"]),
                       sha256=release["sha256"], size=release["size"],
                       platform=release["platform"], architecture=release["architecture"], channel=release["channel"])

    @app.get("/api/v1/firmware/download/<release_id>")
    def download(release_id):
        if not re.fullmatch(r"[0-9a-f]{32}", release_id):
            return error("Release not found", 404)
        release = store.artifact(release_id)
        if release is None:
            return error("Release not found", 404)
        g.release_id = release_id
        if any(release[field] != g.device[field] for field in ("product", "platform", "architecture")) or release["channel"] != g.device["update_channel"]:
            raise PermissionError("Release is not available for this device")
        # Use one open handle for hashing and serving, so a path replacement
        # between verification and transfer cannot substitute different bytes.
        stream = None
        try:
            stream = (store.artifacts / release["storage_url"]).open("rb")
            actual = hashlib.file_digest(stream, "sha256").hexdigest()
            if actual != release["sha256"] or os.fstat(stream.fileno()).st_size != release["size"]:
                stream.close()
                g.audit_status = "artifact_corrupt"
                return error("Stored artifact failed verification", 503)
            stream.seek(0)
            response = send_file(stream, mimetype="application/octet-stream", as_attachment=True,
                                 download_name=release["filename"], conditional=False, etag=False)
            response.content_length = release["size"]
            response.call_on_close(stream.close)
            g.audit_status = "download_started"
            return response
        except OSError:
            if stream:
                stream.close()
            g.audit_status = "artifact_unavailable"
            return error("Stored artifact unavailable", 503)

    return app
