"""Conservative path selection: unknown and shared inputs require a full build."""
import json
import os
import subprocess
import sys


def classify(paths):
    result = set()
    for path in paths:
        if path.startswith(("docs/", "request/")) or path.endswith(".md"):
            result.add("docs")
        elif path.startswith("web/"):
            result.add("web")
        elif path.startswith("tools/firmware_server/") or path == "scripts/firmware_server_smoke.py":
            result.add("firmware_server")
        elif path.startswith(("plugins/", "sdk/")):
            result.add("plugins")
        elif path.startswith(("config/scripts/", "crates/emaster-lua/")):
            result.add("lua")
        else:
            result.add("core")
    return result


if __name__ == "__main__":
    paths = subprocess.check_output(["git", "diff", "--name-only", "--no-renames", sys.argv[1], sys.argv[2]], text=True).splitlines()
    components = classify(paths)
    result = {"components": json.dumps(sorted(components)), "build": str(bool(components - {"docs", "web", "firmware_server"})).lower()}
    print(json.dumps(result))
    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
        for key, value in result.items():
            output.write(f"{key}={value}\n")
