import hashlib
from concurrent.futures import ThreadPoolExecutor
import io
import json
import os
from pathlib import Path
import tempfile
import threading
import subprocess
import sys
import time
import unittest
from urllib.request import urlopen

from werkzeug.serving import make_server

from scripts.firmware_server_smoke import simulate
from tools.firmware_server.app import create_app
from tools.firmware_server.store import Conflict, Store, token_hash, version_key


ADMIN = "test-admin-" + "a" * 40


class FirmwareTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.app = create_app(self.temp.name, ADMIN)
        self.app.testing = True
        self.client = self.app.test_client()
        self.store = self.app.extensions["firmware_store"]
        self.identity = dict(device_id="GW001", product="eMaster", platform="linux", architecture="arm64")
        self.enrollment = self.store.enroll(self.identity, "stable")
        self.token = self.register().get_json()["token"]

    def tearDown(self):
        self.temp.cleanup()

    def auth(self, token=None):
        return {"Authorization": "Bearer " + (self.token if token is None else token)}

    def register(self, identity=None, enrollment=None):
        return self.client.post("/api/v1/device/register", json=identity or self.identity,
                                headers=self.auth(enrollment or self.enrollment))

    def manifest(self, payload, version="1.2.3", platform="linux", architecture="arm64", channel="stable"):
        return dict(schema_version=1, product="eMaster", version=version, platform=platform,
                    architecture=architecture, channel=channel, build_id="123.1", git_commit="a" * 40,
                    build_time="2026-09-11T00:00:00Z", actor="ci-test", filename=f"eMaster-{version}-{platform}-{architecture}.tar.gz",
                    sha256=hashlib.sha256(payload).hexdigest(), size_bytes=len(payload))

    def upload(self, payload=b"opaque CI firmware artifact", metadata=None, token=ADMIN):
        metadata = metadata or self.manifest(payload)
        return self.client.post("/api/v1/firmware/upload", headers=self.auth(token), data={
            "manifest": (io.BytesIO(json.dumps(metadata).encode()), "manifest.json"),
            "artifact": (io.BytesIO(payload), metadata["filename"]),
        })

    def check(self, current="1.0.0", **overrides):
        return self.client.post("/api/v1/firmware/check", headers=self.auth(), json=dict(self.identity, current_version=current, **overrides))

    def test_check_download_and_history(self):
        payload = b"CI-generated artifact bytes"
        upload = self.upload(payload)
        self.assertEqual(upload.status_code, 201)
        offered = self.check().get_json()
        self.assertTrue(offered["update_available"])
        response = self.client.get(offered["download_url"], headers=self.auth())
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.data, payload)
        self.assertEqual(hashlib.sha256(response.data).hexdigest(), offered["sha256"])
        response.close()
        self.assertFalse(self.check("1.2.3").get_json()["update_available"])
        self.assertFalse(self.check("2.0.0").get_json()["update_available"])
        with self.store.connect() as db:
            events = db.execute("SELECT action, status FROM update_history").fetchall()
        self.assertIn(("check", "update_available"), [tuple(row) for row in events])
        self.assertIn(("download", "download_started"), [tuple(row) for row in events])

    def test_registration_is_bound_and_one_time(self):
        self.assertEqual(self.register().status_code, 401)
        self.assertEqual(self.register(dict(self.identity, device_id="UNKNOWN")).status_code, 401)
        other = dict(self.identity, device_id="GW002")
        enrollment = self.store.enroll(other, "stable")
        denied = self.register(dict(other, architecture="x64"), enrollment)
        self.assertEqual(denied.status_code, 403)
        self.assertEqual(self.register(dict(other, update_channel="dev"), enrollment).status_code, 403)
        self.assertEqual(self.register(other, enrollment).status_code, 201)
        with self.assertRaises(Conflict):
            self.store.enroll(other, "dev")

    def test_tokens_are_not_persisted_as_plaintext(self):
        with self.store.connect() as db:
            row = db.execute("SELECT * FROM devices WHERE device_id='GW001'").fetchone()
        self.assertIsNone(row["enrollment_hash"])
        self.assertEqual(row["token_hash"], token_hash(self.token))
        self.assertNotIn(self.token, repr(dict(row)))
        self.assertNotIn(ADMIN, self.store.database.read_bytes().decode("latin1"))

    def test_unauthorized_check_and_download(self):
        self.upload()
        offered = self.check().get_json()
        for token in ("wrong-token-" + "x" * 40, self.enrollment, ADMIN):
            self.assertEqual(self.client.post("/api/v1/firmware/check", json=dict(self.identity, current_version="1.0.0"), headers=self.auth(token)).status_code, 401)
            self.assertEqual(self.client.get(offered["download_url"], headers=self.auth(token)).status_code, 401)
        self.assertEqual(self.client.post("/api/v1/firmware/check").status_code, 401)
        self.assertEqual(self.upload(token=self.token).status_code, 403)
        with self.store.connect() as db:
            rejected = db.execute("SELECT COUNT(*) FROM update_history WHERE http_status=401").fetchone()[0]
        self.assertEqual(rejected, 7)

    def test_identity_and_channel_cannot_be_spoofed(self):
        wrong = dict(self.identity, device_id="other", current_version="1.0.0")
        self.assertEqual(self.client.post("/api/v1/firmware/check", json=wrong, headers=self.auth()).status_code, 403)
        self.assertEqual(self.check(update_channel="beta").status_code, 403)
        beta = self.manifest(b"beta", version="9.0.0-beta.1", channel="beta")
        release = self.upload(b"beta", beta).get_json()["release_id"]
        self.assertFalse(self.check().get_json()["update_available"])
        self.assertEqual(self.client.get(f"/api/v1/firmware/download/{release}", headers=self.auth()).status_code, 403)

    def test_cross_architecture_download_denied(self):
        metadata = self.manifest(b"x64", architecture="x64")
        release = self.upload(b"x64", metadata).get_json()["release_id"]
        self.assertFalse(self.check().get_json()["update_available"])
        self.assertEqual(self.client.get(f"/api/v1/firmware/download/{release}", headers=self.auth()).status_code, 403)

    def test_numeric_semver_and_no_downgrade(self):
        for version in ("1.9.0", "1.10.0", "1.2.3"):
            self.assertEqual(self.upload(metadata=self.manifest(b"opaque CI firmware artifact", version=version)).status_code, 201)
        self.assertEqual(self.check("1.9.0").get_json()["version"], "1.10.0")
        self.assertFalse(self.check("1.10.0+build.2").get_json()["update_available"])
        chain = ["1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0"]
        self.assertEqual(sorted(reversed(chain), key=version_key), chain)

    def test_corrupted_upload_and_metadata_rejected(self):
        payload = b"correct bytes"
        metadata = self.manifest(payload)
        self.assertEqual(self.upload(b"corrupt bytes", metadata).status_code, 400)
        for field, value in [("filename", "../escape.tar.gz"), ("git_commit", "not-a-commit"), ("schema_version", 2), ("size_bytes", -1), ("build_time", "tomorrow"), ("version", "01.2.3"), ("architecture", "x86")]:
            with self.subTest(field=field):
                bad = dict(metadata)
                bad[field] = value
                self.assertEqual(self.upload(payload, bad).status_code, 400)
        self.assertEqual(list(self.store.artifacts.iterdir()), [])
        with self.store.connect() as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM firmware_releases").fetchone()[0], 0)

    def test_duplicate_release_is_immutable_and_manifest_preserved(self):
        metadata = self.manifest(b"first")
        first = self.upload(b"first", metadata)
        self.assertEqual(first.status_code, 201)
        self.assertEqual(self.upload(b"second", self.manifest(b"second")).status_code, 409)
        with self.store.connect() as db:
            row = db.execute("SELECT manifest_json FROM firmware_releases").fetchone()
        self.assertEqual(row[0], json.dumps(metadata))
        self.assertEqual(len(list(self.store.artifacts.glob("*.blob"))), 1)
        self.assertFalse(list(self.store.artifacts.glob("*.upload")))

    def test_corrupt_stored_blob_is_not_served(self):
        self.upload()
        offered = self.check().get_json()
        next(self.store.artifacts.glob("*.blob")).write_bytes(b"tampered")
        response = self.client.get(offered["download_url"], headers=self.auth())
        self.assertEqual(response.status_code, 503)
        self.assertNotIn(b"tampered", response.data)

    def test_state_survives_server_restart(self):
        self.upload()
        restarted = create_app(self.temp.name, ADMIN).test_client()
        response = restarted.post("/api/v1/firmware/check", headers=self.auth(), json=dict(self.identity, current_version="1.0.0"))
        self.assertTrue(response.get_json()["update_available"])

    def test_malformed_and_oversized_requests(self):
        for payload in ([], None, dict(self.identity, current_version="1.2"), dict(self.identity, current_version="1.0.0-01")):
            response = self.client.post("/api/v1/firmware/check", data=json.dumps(payload), content_type="application/json", headers=self.auth())
            self.assertEqual(response.status_code, 400)
        response = self.client.post("/api/v1/firmware/check", data=b" " * 70000, content_type="application/json", headers=self.auth())
        self.assertEqual(response.status_code, 413)
        self.assertEqual(self.client.post("/api/v1/firmware/upload", headers=self.auth(ADMIN), data={}).status_code, 400)

    def test_http_gateway_simulator(self):
        self.upload()
        server = make_server("127.0.0.1", 0, self.app, threaded=True)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            result = simulate(f"http://127.0.0.1:{server.server_port}", self.token,
                              dict(self.identity, current_version="1.0.0"), expect_update=True)
            self.assertTrue(result["verified"])
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)

    def test_missing_admin_secret_fails_startup(self):
        with self.assertRaises(ValueError):
            create_app(self.temp.name, "short")

    def test_concurrent_imports_publish_only_one_release(self):
        payload = b"concurrent upload"
        metadata = self.manifest(payload)
        raw = json.dumps(metadata).encode()
        def import_once(_):
            try:
                self.store.import_artifact(raw, io.BytesIO(payload), metadata["filename"])
                return "created"
            except Conflict:
                return "conflict"
        with ThreadPoolExecutor(max_workers=2) as executor:
            self.assertCountEqual(executor.map(import_once, range(2)), ["created", "conflict"])
        with self.store.connect() as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM firmware_releases").fetchone()[0], 1)
        self.assertEqual(len(list(self.store.artifacts.iterdir())), 1)

    def test_concurrent_registration_consumes_enrollment_once(self):
        identity = dict(self.identity, device_id="concurrent")
        enrollment = self.store.enroll(identity, "stable")
        with ThreadPoolExecutor(max_workers=2) as executor:
            results = list(executor.map(lambda _: self.store.register(identity, enrollment), range(2)))
        self.assertEqual(sum(result is not None for result in results), 1)

    def test_waitress_command_starts_with_persisted_state(self):
        # Bind port zero to avoid clashes with a developer's running gateway.
        # The startup message supplies the OS-assigned port, without printing
        # credentials or starting an interactive window on Windows.
        self.upload()
        environment = dict(os.environ, FW_ADMIN_TOKEN=ADMIN)
        command = [sys.executable, "-m", "tools.firmware_server", "--state-dir", self.temp.name, "serve", "--port", "0"]
        log_path = Path(self.temp.name) / "waitress.log"
        with log_path.open("w", encoding="utf-8") as log:
            process = subprocess.Popen(command, env=environment, stdout=log, stderr=log,
                                       creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            try:
                import re
                deadline = time.monotonic() + 15
                port = None
                while time.monotonic() < deadline:
                    self.assertIsNone(process.poll(), "Waitress exited during startup")
                    match = re.search(r"Serving on http://127\.0\.0\.1:(\d+)", log_path.read_text())
                    if match:
                        port = int(match[1])
                        break
                    time.sleep(0.1)
                self.assertIsNotNone(port, "Waitress did not report its listening port")
                with urlopen(f"http://127.0.0.1:{port}/healthz", timeout=5) as response:
                    self.assertEqual(json.load(response), {"status": "ok"})
                result = simulate(f"http://127.0.0.1:{port}", self.token,
                                  dict(self.identity, current_version="1.0.0"), expect_update=True)
                self.assertTrue(result["verified"])
            finally:
                if os.name == "nt" and process.poll() is None:
                    # Windows venv launchers can have a child interpreter;
                    # terminate this test's entire tree so files are released.
                    subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
                else:
                    process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
