import io
import json
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import release
from changes import classify


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.previous = Path.cwd()
        os.chdir(self.temp.name)
        Path("VERSION").write_text("1.2.3")
        self.dist = Path("dist")
        self.dist.mkdir()
        self.commit = "a" * 40
        self.metadata = dict(product="eMaster", version="1.2.3", platform="windows", architecture="x64", git_commit=self.commit)

    def tearDown(self):
        os.chdir(self.previous)
        self.temp.cleanup()

    def package(self, tar=False):
        payload = json.dumps(self.metadata).encode()
        if tar:
            archive = self.dist / "HSF-Gateway-v1.2.3-linux-armhf.tar.gz"
            with tarfile.open(archive, "w:gz") as output:
                member = tarfile.TarInfo("root/manifest.json")
                member.size = len(payload)
                output.addfile(member, io.BytesIO(payload))
        else:
            archive = self.dist / "HSF-Gateway-v1.2.3-windows-x64.zip"
            with zipfile.ZipFile(archive, "w") as output:
                output.writestr("root/manifest.json", payload)
        return archive

    def finalize(self):
        with patch.object(release, "git", return_value=self.commit):
            release.finalize(self.dist, "123.1", "stable")

    def verify(self, expected="windows-x64"):
        release.verify(self.dist, "1.2.3", self.commit, expected)

    def test_zip_round_trip(self):
        self.package()
        self.finalize()
        self.verify()
        self.assertIn("eMaster-1.2.3-windows-x64.zip", (self.dist / "SHA256SUMS").read_text())

    def test_tar_round_trip(self):
        self.metadata.update(platform="linux", architecture="armhf")
        self.package(tar=True)
        self.finalize()
        self.verify("linux-armhf")

    def test_tampered_archive_rejected(self):
        self.package()
        self.finalize()
        with next(self.dist.glob("*.zip")).open("ab") as output:
            output.write(b"tampered")
        with self.assertRaisesRegex(ValueError, "checksum"):
            self.verify()

    def test_wrong_commit_rejected(self):
        self.metadata["git_commit"] = "b" * 40
        self.package()
        with self.assertRaisesRegex(ValueError, "provenance"):
            self.finalize()

    def test_missing_target_rejected(self):
        self.package()
        self.finalize()
        with self.assertRaisesRegex(ValueError, "matrix mismatch"):
            self.verify("windows-x64,linux-arm64")

    def test_no_overwrite(self):
        self.package()
        self.finalize()
        self.package()
        with self.assertRaisesRegex(ValueError, "overwrite"):
            self.finalize()

    def test_incomplete_metadata_rejected(self):
        self.package()
        self.finalize()
        path = next(self.dist.glob("*.manifest.json"))
        data = json.loads(path.read_text())
        del data["build_id"]
        path.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, "schema"):
            self.verify()

    def test_traversal_rejected(self):
        self.package()
        self.finalize()
        path = next(self.dist.glob("*.manifest.json"))
        data = json.loads(path.read_text())
        data["filename"] = "../outside.zip"
        path.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, "filename"):
            self.verify()

    def test_semver_and_channels(self):
        for version, expected in [("1.2.3", "stable"), ("1.2.3+build.4", "stable"), ("1.2.3-rc.1", "beta"), ("1.2.3-alpha.1", "dev")]:
            self.assertEqual(release.channel(version), expected)
        for invalid in ("01.2.3", "1.2.3;echo", "1.2.3-01", "1.2", "-v1.2.3"):
            with self.assertRaises(ValueError):
                release.channel(invalid)

    def test_guard_branch_and_tag_checkout(self):
        def run(*args):
            return subprocess.check_output(["git", *args], stderr=subprocess.STDOUT, text=True).strip()
        run("init", "-b", "main")
        run("config", "user.name", "CI Test")
        run("config", "user.email", "ci-test@example.invalid")
        Path("CHANGELOG.md").write_text("## [1.2.3]\n- test\n")
        run("add", "VERSION", "CHANGELOG.md")
        run("commit", "-m", "test fixture")
        run("tag", "v1.2.3")
        run("update-ref", "refs/remotes/origin/main", "HEAD")
        with patch.dict(os.environ, {"GITHUB_OUTPUT": "outputs"}):
            release.guard("v1.2.3")
        run("commit", "--allow-empty", "-m", "untagged")
        with self.assertRaisesRegex(ValueError, "Checkout"):
            release.guard("v1.2.3")
        run("checkout", "v1.2.3")
        run("update-ref", "-d", "refs/remotes/origin/main")
        with self.assertRaisesRegex(ValueError, "main or release"):
            release.guard("v1.2.3")

    def test_changes_fail_closed(self):
        self.assertEqual(classify(["tools/firmware_server/app.py", "scripts/firmware_server_smoke.py"]), {"firmware_server"})
        self.assertEqual(classify(["docs/test.md", "web/js/app.js"]), {"docs", "web"})
        self.assertIn("core", classify(["unknown/new-input", "web/js/app.js"]))
        self.assertIn("core", classify(["CMakeLists.txt"]))


if __name__ == "__main__":
    unittest.main()
