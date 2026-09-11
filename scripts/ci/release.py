"""Release provenance and firmware metadata. Uses only the Python standard library."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile
import zipfile
from datetime import datetime, timezone


SEMVER = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-((?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*)(?:\.(?:0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*))*))?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?")


def git(*args):
    return subprocess.check_output(["git", *args], text=True).strip()


def channel(version):
    match = SEMVER.fullmatch(version)
    if not match:
        raise ValueError("Invalid semantic version")
    pre = match.group(4)
    return "dev" if pre and pre.split(".")[0] in ("dev", "alpha") else "beta" if pre else "stable"


def guard(tag):
    if not tag.startswith("v") or not SEMVER.fullmatch(tag[1:]):
        raise ValueError("Release tag must be vMAJOR.MINOR.PATCH with optional prerelease/metadata")
    commit = git("rev-parse", "--verify", f"refs/tags/{tag}^{{commit}}")
    if git("rev-parse", "HEAD") != commit:
        raise ValueError("Checkout does not match the release tag")
    version = Path("VERSION").read_text().strip()
    if version != tag[1:]:
        raise ValueError("Tag does not match VERSION")
    branches = git("for-each-ref", "--format=%(refname)", "refs/remotes/origin/main", "refs/remotes/origin/release/").splitlines()
    if not any(subprocess.run(["git", "merge-base", "--is-ancestor", commit, branch], check=False).returncode == 0 for branch in branches):
        raise ValueError("Tag commit must belong to main or release/*")
    if f"## [{version}]" not in Path("CHANGELOG.md").read_text(encoding="utf-8"):
        raise ValueError("CHANGELOG has no entry for the release")
    result = dict(version=version, commit=commit, tag=tag, channel=channel(version))
    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
            for key, value in result.items():
                output.write(f"{key}={value}\n")
    print(json.dumps(result))


def embedded_manifest(archive):
    if archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as package:
            names = [name for name in package.namelist() if len(name.split("/")) == 2 and name.endswith("/manifest.json")]
            if len(names) != 1:
                raise ValueError("Archive must have one root manifest")
            return json.loads(package.read(names[0]).decode("utf-8-sig"))
    with tarfile.open(archive, "r:gz") as package:
        members = [m for m in package.getmembers() if m.isfile() and len(m.name.split("/")) == 2 and m.name.endswith("/manifest.json")]
        if len(members) != 1:
            raise ValueError("Archive must have one root manifest")
        return json.load(package.extractfile(members[0]))


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def validate_metadata(data):
    required = {"schema_version", "product", "version", "platform", "architecture", "build_id", "git_commit", "build_time", "channel", "filename", "sha256", "size_bytes", "actor"}
    if set(data) != required or data["schema_version"] != 1:
        raise ValueError("Invalid manifest schema or fields")
    for field in required - {"schema_version", "size_bytes"}:
        if not isinstance(data[field], str) or not data[field]:
            raise ValueError(f"Invalid manifest field: {field}")
    channel(data["version"])
    if data["product"] != "eMaster" or data["channel"] not in ("dev", "beta", "stable"):
        raise ValueError("Invalid manifest product or channel")
    if data["platform"] not in ("windows", "linux", "macos") or data["architecture"] not in ("x86", "x64", "arm64", "armhf"):
        raise ValueError("Unsupported platform or architecture")
    if not re.fullmatch(r"[0-9a-f]{40}", data["git_commit"]) or not re.fullmatch(r"[0-9a-f]{64}", data["sha256"]):
        raise ValueError("Invalid commit or checksum")
    if type(data["size_bytes"]) is not int or data["size_bytes"] < 1:
        raise ValueError("Invalid archive size")
    datetime.strptime(data["build_time"], "%Y-%m-%dT%H:%M:%SZ")
    prefix = f'eMaster-{data["version"]}-{data["platform"]}-{data["architecture"]}'
    if data["filename"] not in (prefix + ".zip", prefix + ".tar.gz"):
        raise ValueError("Invalid artifact filename")


def finalize(directory, build_id, release_channel):
    commit = git("rev-parse", "HEAD")
    version = Path("VERSION").read_text().strip()
    channel(version)  # Validate even for development builds.
    archives = sorted(p for p in directory.glob("HSF-Gateway-v*.*") if p.is_file() and p.name.endswith((".zip", ".tar.gz")))
    if not archives:
        raise ValueError("No package archives found")
    for archive in archives:
        if not (archive.name.endswith(".tar.gz") or archive.suffix == ".zip"):
            continue
        metadata = embedded_manifest(archive)
        if metadata.get("product") != "eMaster" or metadata.get("version") != version or metadata.get("git_commit") != commit:
            raise ValueError("Package provenance does not match the checkout")
        platform, arch = metadata["platform"], metadata["architecture"]
        if platform not in ("windows", "linux", "macos") or arch not in ("x86", "x64", "arm64", "armhf"):
            raise ValueError("Unsupported platform or architecture")
        extension = ".zip" if archive.suffix == ".zip" else ".tar.gz"
        name = f"eMaster-{version}-{platform}-{arch}{extension}"
        target = directory / name
        sidecar = directory / (name + ".manifest.json")
        if target.exists() or sidecar.exists():
            raise ValueError("Refusing to overwrite an existing artifact")
        data = dict(schema_version=1, product="eMaster", version=version,
                    platform=platform, architecture=arch, build_id=build_id,
                    git_commit=commit, build_time=datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                    channel=release_channel, filename=name, sha256=digest(archive), size_bytes=archive.stat().st_size,
                    actor=os.environ.get("GITHUB_ACTOR", "local"))
        validate_metadata(data)
        archive.rename(target)
        sidecar.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
        print(name)


def verify(directory, version, commit, expected):
    seen = set()
    checksums = []
    for sidecar in sorted(directory.glob("*.manifest.json")):
        data = json.loads(sidecar.read_text(encoding="utf-8"))
        validate_metadata(data)
        name = data["filename"]
        if Path(name).name != name or "/" in name or "\\" in name:
            raise ValueError("Invalid artifact filename")
        target = f'{data["platform"]}-{data["architecture"]}'
        if target in seen or data["version"] != version or data["git_commit"] != commit or data["product"] != "eMaster":
            raise ValueError("Duplicate target or inconsistent provenance")
        archive = directory / name
        if digest(archive) != data["sha256"] or archive.stat().st_size != data["size_bytes"]:
            raise ValueError("Artifact checksum or size mismatch")
        embedded = embedded_manifest(archive)
        if any(embedded.get(field) != data[field] for field in ("product", "version", "git_commit", "platform", "architecture")):
            raise ValueError("Embedded and external metadata disagree")
        seen.add(target)
        checksums.append(f'{data["sha256"]}  {name}\n')
    if seen != set(expected.split(",")):
        raise ValueError(f"Artifact matrix mismatch: {sorted(seen)}")
    (directory / "SHA256SUMS").write_text("".join(checksums), encoding="ascii")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("guard").add_argument("tag")
    final = commands.add_parser("finalize")
    final.add_argument("directory", type=Path)
    final.add_argument("--build-id", required=True)
    final.add_argument("--channel", choices=("dev", "beta", "stable"), required=True)
    check = commands.add_parser("verify")
    check.add_argument("directory", type=Path)
    check.add_argument("--version", required=True)
    check.add_argument("--commit", required=True)
    check.add_argument("--expected", required=True)
    args = parser.parse_args()
    if args.command == "guard":
        guard(args.tag)
    elif args.command == "finalize":
        finalize(args.directory, args.build_id, args.channel)
    else:
        verify(args.directory, args.version, args.commit, args.expected)
