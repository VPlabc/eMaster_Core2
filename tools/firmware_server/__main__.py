"""Run the MVP service or provision a one-time device enrollment token."""
import argparse
import json
import logging
import os

from .store import ARTIFACT_LIMIT, Store


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state-dir", default=os.environ.get("FW_STATE_DIR", "build-firmware-server/state"))
    commands = parser.add_subparsers(dest="command", required=True)
    serve = commands.add_parser("serve")
    serve.add_argument("--host", default="127.0.0.1")
    serve.add_argument("--port", type=int, default=8092)
    enroll = commands.add_parser("enroll")
    enroll.add_argument("--device-id", required=True)
    enroll.add_argument("--platform", choices=("windows", "linux", "macos"), required=True)
    enroll.add_argument("--architecture", choices=("x86", "x64", "arm64", "armhf"), required=True)
    enroll.add_argument("--channel", choices=("dev", "beta", "stable"), default="stable")
    args = parser.parse_args()
    if args.command == "enroll":
        identity = dict(device_id=args.device_id, product="eMaster", platform=args.platform, architecture=args.architecture)
        token = Store(args.state_dir).enroll(identity, args.channel)
        print(json.dumps(dict(identity, update_channel=args.channel, enrollment_token=token)))
    else:
        logging.basicConfig(level=logging.INFO)
        from waitress import serve
        from .app import create_app
        serve(create_app(args.state_dir), host=args.host, port=args.port, threads=4,
              max_request_body_size=ARTIFACT_LIMIT + 1024 * 1024,
              channel_timeout=60, clear_untrusted_proxy_headers=True)


if __name__ == "__main__":
    main()
