"""Start a packaged gateway with isolated state and check HTTP readiness."""
import argparse
import json
from pathlib import Path
import platform
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


def smoke(stage, target, port):
    machine = platform.machine().lower()
    arch = {"amd64": "x64", "x86_64": "x64", "aarch64": "arm64", "arm64": "arm64", "armv7l": "armhf", "armv8l": "armhf"}.get(machine, machine)
    actual = f'{platform.system().lower()}-{arch}'
    if actual != target:
        raise ValueError(f"Native runner mismatch: expected {target}, found {actual}")
    exe = stage.resolve() / "bin" / ("hsf_gateway.exe" if platform.system() == "Windows" else "hsf_gateway")
    subprocess.run([str(exe), "--version"], check=True, timeout=30)
    with tempfile.TemporaryDirectory(prefix="emaster-smoke-") as work:
        root = Path(work)
        (root / "config.json").write_text(json.dumps({"web": {"bind_address": "127.0.0.1", "port": port}, "auth": {"enabled": True}}))
        with (root / "gateway.log").open("w", encoding="utf-8") as log:
            process = subprocess.Popen([str(exe), str(root / "config.db")], cwd=root, stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 60
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        raise RuntimeError("Gateway exited before readiness")
                    try:
                        with urllib.request.urlopen(f"http://127.0.0.1:{port}/api/auth/mode", timeout=2) as response:
                            if response.status == 200:
                                json.load(response)
                                time.sleep(2)
                                if process.poll() is not None:
                                    raise RuntimeError("Gateway exited after readiness")
                                print(f"{target}: native startup and HTTP readiness passed")
                                return
                    except (urllib.error.URLError, TimeoutError):
                        pass
                    time.sleep(0.5)
                raise RuntimeError("Gateway readiness timed out")
            finally:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", type=Path)
    parser.add_argument("--target", required=True)
    parser.add_argument("--port", type=int, default=18180)
    args = parser.parse_args()
    smoke(args.stage, args.target, args.port)
