# Installation

Applies to a released package. To build from source instead, see
[building.md](building.md).

## Packages

| Package | ZK card reader | Notes |
|---|---|---|
| `HSF-Gateway-v1.0.0-windows-x86.zip` | **yes** | The only package with ZKTeco support |
| `HSF-Gateway-v1.0.0-windows-x64.zip` | no | |
| `HSF-Gateway-v1.0.0-linux-x64.tar.gz` | no | |
| `HSF-Gateway-v1.0.0-linux-arm64.tar.gz` | no | |
| `HSF-Gateway-v1.0.0-macos-arm64.tar.gz` | no | |

ZKTeco access-controller support needs the PullSDK (`plcommpro.dll`), which
ZKTeco ships only as a 32-bit Windows binary. Nothing in this project can port
it, so **if you use a ZK controller you must run the Windows x86 package.**
Every other package builds and runs normally; `zk.*` calls fail with
`NotSupportedOnThisPlatform` and a warning is logged once at first use. All
other hardware — Modbus PLC, serial card reader, LED display, TCP RFID, the
Card Reader Client API — works on every platform.

## Verify the download

```bash
sha256sum -c SHA256SUMS
```

```powershell
(Get-FileHash HSF-Gateway-v1.0.0-windows-x86.zip -Algorithm SHA256).Hash
```

## Extract and run

Extract anywhere. The gateway locates `web/` and `config/` relative to its own
executable, so no installation step or environment variable is needed.

**Linux / macOS**

```bash
tar -xzf HSF-Gateway-v1.0.0-linux-x64.tar.gz
cd HSF-Gateway-v1.0.0-linux-x64
./scripts/run.sh
```

**Windows**

```powershell
Expand-Archive HSF-Gateway-v1.0.0-windows-x86.zip -DestinationPath .
cd HSF-Gateway-v1.0.0-windows-x86
.\scripts\run.ps1
```

On first start `run.sh` / `run.ps1` copies `config/config.example.json` to
`config/config.json`, which the gateway imports once into a new
`config/config.db`. From then on `config.db` is authoritative — edit settings
from the Configuration page in the web UI.

Then open <http://localhost:8080>.

## Package layout

```text
HSF-Gateway-v1.0.0-<platform>/
├── bin/            hsf_gateway (+ runtime DLLs on Windows)
├── web/            dashboard, served by the gateway
├── config/
│   ├── config.example.json
│   └── scripts/    Lua scripts
├── docs/
├── scripts/        run.sh / run.ps1
├── BUILD_INFO.txt  version, commit, build date, platform
├── CHANGELOG.md
├── LICENSE
├── README.md
└── VERSION
```

Packages deliberately contain no `config.db`, `config.json` or `clients.db` —
those hold the REST API key and generated card-client keys.

## Running as a service

The package is relocatable, so the layout below is a convention rather than a
requirement. Keep runtime state (`config.db`, `clients.db`, logs) outside the
application directory so an upgrade is a straight replacement.

```text
/opt/hsf-gateway/          application files (replaced on upgrade)
/etc/hsf-gateway/          config.db, clients.db
/var/log/hsf-gateway/      gateway.log
```

Pass the config path explicitly when it is not beside the binary:

```ini
# /etc/systemd/system/hsf-gateway.service
[Unit]
Description=eMaster Gateway
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/opt/hsf-gateway/bin/hsf_gateway /etc/hsf-gateway/config.db
Restart=on-failure
RestartSec=5
User=hsf
Group=hsf

[Install]
WantedBy=multi-user.target
```

```bash
sudo useradd --system --no-create-home hsf
sudo usermod -aG dialout hsf     # serial port access
sudo systemctl enable --now hsf-gateway
```

`dialout` (or `uucp` on some distributions) is what lets a non-root service
open `/dev/ttyUSB*`. Without it the card reader and LED display fail to open
while everything else works — see [troubleshooting.md](troubleshooting.md).

## Upgrading

1. Stop the gateway.
2. Back up `config.db` and `clients.db`.
3. Replace the application directory with the new package.
4. Restore or re-point at the config path.
5. Start, then confirm the version on the dashboard's System Info tab, or:

```bash
./bin/hsf_gateway --version
```

Configuration carries forward: `ConfigManager` adds unknown sections with
defaults and leaves existing values alone. Check [CHANGELOG.md](../CHANGELOG.md)
for anything needing manual attention.
