#!/usr/bin/env bash
#
# Lays out the managed install the gateway can update itself inside
# (request/CICD.md sections 3 and 9), from an extracted or archived release
# package. Run once per device; after that the gateway installs its own
# updates into the same tree.
#
# The layout it creates:
#
#     <root>/
#     |-- current -> releases/<version>
#     |-- releases/<version>/   bin/ web/ config/ docs/ ...
#     |-- data/                 config.db, clients.db, logs.db, logs/, keys
#     |-- downloads/            scratch space, safe to delete
#     `-- update-state.json
#
# The releases/ and data/ split is the load-bearing part: a release directory
# is replaced wholesale on every update, so anything the *installation* owns
# has to live outside one. That is why the service line below passes
# <root>/data/config.db explicitly -- main() derives every other state path
# from the config file's directory, so one argument keeps the databases, the
# logs and the structured log store out of the disposable tree.
#
# Usage:
#   sudo scripts/install-ota.sh --package HSF-Gateway-v1.0.0-linux-x64.tar.gz
#   sudo scripts/install-ota.sh --package ./dist/HSF-Gateway-v1.0.0-linux-x64 \
#                               --root /opt/hsf-gateway --user hsf --service
set -euo pipefail

PACKAGE=""
ROOT="/opt/hsf-gateway"
SERVICE_USER=""
INSTALL_SERVICE=0
PUBLIC_KEY=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --package)    PACKAGE="$2"; shift 2 ;;
    --root)       ROOT="${2%/}"; shift 2 ;;
    --user)       SERVICE_USER="$2"; shift 2 ;;
    --public-key) PUBLIC_KEY="$2"; shift 2 ;;
    --service)    INSTALL_SERVICE=1; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

[[ -n "$PACKAGE" ]] || { echo "--package is required (a .tar.gz, or an already-extracted directory)" >&2; exit 1; }

WORK=""
cleanup() { [[ -n "$WORK" && -d "$WORK" ]] && rm -rf "$WORK"; }
trap cleanup EXIT

# Accept either an archive or a directory, because both are things someone has
# in hand at this point: the downloaded release, or the output of package.sh.
if [[ -d "$PACKAGE" ]]; then
  SOURCE="$PACKAGE"
else
  [[ -f "$PACKAGE" ]] || { echo "no such package: $PACKAGE" >&2; exit 1; }
  WORK="$(mktemp -d)"
  tar -xzf "$PACKAGE" -C "$WORK"
  # The archive has one release directory between WORK and bin/. maxdepth 3
  # is required; maxdepth 2 silently rejected every valid packaged release.
  SOURCE="$(find "$WORK" -maxdepth 3 -type f -path '*/bin/hsf_gateway' -printf '%h\n' | head -n1)"
  SOURCE="${SOURCE%/bin}"
  [[ -n "$SOURCE" ]] || { echo "the package does not contain bin/hsf_gateway" >&2; exit 1; }
fi

[[ -x "$SOURCE/bin/hsf_gateway" ]] || chmod +x "$SOURCE/bin/hsf_gateway"

VERSION="$("$SOURCE/bin/hsf_gateway" --version | awk '/^Version:/ {print $2}')"
[[ -n "$VERSION" ]] || { echo "could not read the version out of the packaged binary" >&2; exit 1; }
echo "[install] eMaster Gateway $VERSION -> $ROOT"

mkdir -p "$ROOT/releases" "$ROOT/data" "$ROOT/downloads"

TARGET="$ROOT/releases/$VERSION"
if [[ -d "$TARGET" ]]; then
  echo "[install] $TARGET already exists; replacing it"
  rm -rf "$TARGET"
fi
cp -R "$SOURCE" "$TARGET"

# Seed the installation-owned files from what the release ships, but never
# overwrite: re-running this script to reinstall must not discard a config
# database or an edited log_definitions.json.
for file in "$TARGET"/config/*; do
  [[ -f "$file" ]] || continue
  name="$(basename "$file")"
  if [[ ! -e "$ROOT/data/$name" ]]; then
    cp "$file" "$ROOT/data/$name"
    echo "[install] seeded data/$name"
  fi
done

if [[ -n "$PUBLIC_KEY" ]]; then
  [[ -f "$PUBLIC_KEY" ]] || { echo "no such public key: $PUBLIC_KEY" >&2; exit 1; }
  cp "$PUBLIC_KEY" "$ROOT/data/update_public_key.pem"
  chmod 0644 "$ROOT/data/update_public_key.pem"
  echo "[install] installed the update signing public key"
fi

# Relative target, so the tree can be moved or bind-mounted without every link
# still pointing at the old absolute path. Same form UpdateInstaller writes.
ln -sfn "releases/$VERSION" "$ROOT/current.new"
mv -T "$ROOT/current.new" "$ROOT/current"
echo "[install] current -> releases/$VERSION"

if [[ -n "$SERVICE_USER" ]]; then
  if ! id "$SERVICE_USER" >/dev/null 2>&1; then
    echo "[install] creating system user $SERVICE_USER"
    useradd --system --no-create-home --shell /usr/sbin/nologin "$SERVICE_USER"
  fi
  # The whole root, not just data/: the gateway writes releases/ and
  # downloads/ when it installs an update, and repoints current. A tree it can
  # only read is a tree it can never update.
  chown -R "$SERVICE_USER:$SERVICE_USER" "$ROOT"
fi

if [[ $INSTALL_SERVICE -eq 1 ]]; then
  UNIT=/etc/systemd/system/hsf-gateway.service
  echo "[install] writing $UNIT"
  sed -e "s|@ROOT@|$ROOT|g" \
      -e "s|@USER@|${SERVICE_USER:-root}|g" \
      "$TARGET/deploy/hsf-gateway.service" > "$UNIT" 2>/dev/null \
    || sed -e "s|@ROOT@|$ROOT|g" -e "s|@USER@|${SERVICE_USER:-root}|g" \
           "$(dirname "${BASH_SOURCE[0]}")/../deploy/hsf-gateway.service" > "$UNIT"
  systemctl daemon-reload
  systemctl enable hsf-gateway.service
  echo "[install] systemctl start hsf-gateway   # when you are ready"
fi

cat <<EOF

[install] done.

  Run it:      $ROOT/current/bin/hsf_gateway $ROOT/data/config.db
  Web UI:      http://<device>:8080
  Updates:     Configuration > Software Update -- set the server URL and enable.

The config path argument is not optional in this layout: without it the
gateway would put its databases inside $ROOT/current/config, which the next
update replaces.
EOF
