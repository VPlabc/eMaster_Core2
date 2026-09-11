"""Simulate a registered gateway: check, download, and verify CI SHA256."""
import argparse
import hashlib
import json
import os
from urllib.parse import urljoin, urlsplit
from urllib.request import Request, urlopen


def simulate(base_url, token, identity, expect_update=False):
    base_url = base_url.rstrip("/") + "/"
    headers = {"Authorization": "Bearer " + token, "Content-Type": "application/json"}
    request = Request(urljoin(base_url, "api/v1/firmware/check"), data=json.dumps(identity).encode(), headers=headers)
    with urlopen(request, timeout=30) as response:
        result = json.load(response)
    if not result["update_available"]:
        if expect_update:
            raise ValueError("Expected a newer firmware release")
        return {"update_available": False}
    download_url = urljoin(base_url, result["download_url"])
    # Never forward a device token to an artifact URL on another origin.
    if urlsplit(download_url)[:2] != urlsplit(base_url)[:2]:
        raise ValueError("Download URL must remain on the firmware server origin")
    digest = hashlib.sha256()
    size = 0
    with urlopen(Request(download_url, headers={"Authorization": "Bearer " + token}), timeout=60) as response:
        while block := response.read(1024 * 1024):
            digest.update(block)
            size += len(block)
    if digest.hexdigest() != result["sha256"] or size != result["size"]:
        raise ValueError("Downloaded firmware checksum/size mismatch")
    return dict(update_available=True, release_id=result["release_id"], version=result["version"], sha256=digest.hexdigest(), size=size, verified=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://127.0.0.1:8092")
    parser.add_argument("--device-id", required=True)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--architecture", required=True)
    parser.add_argument("--current-version", required=True)
    parser.add_argument("--expect-update", action="store_true")
    args = parser.parse_args()
    token = os.environ.get("FW_DEVICE_TOKEN")
    if not token:
        parser.error("Set FW_DEVICE_TOKEN to the registered device token")
    identity = dict(device_id=args.device_id, product="eMaster", platform=args.platform,
                    architecture=args.architecture, current_version=args.current_version)
    print(json.dumps(simulate(args.url, token, identity, args.expect_update), indent=2))
