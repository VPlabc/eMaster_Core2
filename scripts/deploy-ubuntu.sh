#!/usr/bin/env bash
#
# Remote deployment to an Ubuntu gateway (request/deloyToUbuntu.md sections
# 3.7-3.14).
#
#   ./scripts/deploy-ubuntu.sh --host 192.168.3.209 --user ad
#   ./scripts/deploy-ubuntu.sh --host 192.168.3.209 --user ad --dry-run
#
# NO PASSWORD IS ACCEPTED, ANYWHERE. Not as an argument, not from a file, not
# from the environment. SSH key authentication only, and `BatchMode=yes` makes
# ssh fail rather than fall back to an interactive prompt. That is section
# 3.15's requirement made structural: there is no code path here that could put
# a credential into shell history, a process listing, or this repository.
#
#   ssh-keygen -t ed25519
#   ssh-copy-id ad@192.168.3.209
#
# IT REUSES THE OTA LAYOUT. /opt/hsf-gateway/{releases,current,data} and the
# systemd unit already exist for over-the-air updates (docs/ota-update.md), and
# section 3.9's design is the same thing. Deploying into that layout means one
# rollback mechanism, not two that disagree about which release is live.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

HOST=""
USER_NAME=""
SSH_PORT=22
SSH_KEY=""
REMOTE_ROOT="/opt/hsf-gateway"
SERVICE="hsf-gateway"
HEALTH_PORT=""
PACKAGE=""
DRY_RUN=0
SKIP_BUILD=0
HEALTH_TIMEOUT=60

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host)          HOST="$2"; shift 2 ;;
    --user)          USER_NAME="$2"; shift 2 ;;
    --port)          SSH_PORT="$2"; shift 2 ;;
    --key)           SSH_KEY="$2"; shift 2 ;;
    --root)          REMOTE_ROOT="${2%/}"; shift 2 ;;
    --service)       SERVICE="$2"; shift 2 ;;
    --health-port)   HEALTH_PORT="$2"; shift 2 ;;
    --package)       PACKAGE="$2"; shift 2 ;;
    --health-timeout) HEALTH_TIMEOUT="$2"; shift 2 ;;
    --skip-build)    SKIP_BUILD=1; shift ;;
    --dry-run)       DRY_RUN=1; shift ;;
    --password|--pass|-p)
      echo "Refusing a password argument. This script is SSH-key only -- see the header." >&2
      exit 2 ;;
    -h|--help) sed -n '2,25p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

[[ -n "$HOST" ]] || { echo "--host is required" >&2; exit 2; }
[[ -n "$USER_NAME" ]] || { echo "--user is required" >&2; exit 2; }

say()  { printf '\033[1m[deploy]\033[0m %s\n' "$*"; }
warn() { printf '\033[33m[deploy] %s\033[0m\n' "$*"; }
die()  { printf '\033[31m[deploy] %s\033[0m\n' "$*" >&2; exit 1; }

SSH_OPTS=(-o BatchMode=yes -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10 -p "$SSH_PORT")
[[ -n "$SSH_KEY" ]] && SSH_OPTS+=(-i "$SSH_KEY")
TARGET="$USER_NAME@$HOST"

remote() { ssh "${SSH_OPTS[@]}" "$TARGET" "$@"; }
# Health is probed from ON the target via its loopback: the web port is often
# bound to an interface a developer machine cannot reach, and "the deploy
# succeeded but my laptop cannot see it" is a firewall question, not a
# deployment failure.
remote_health() { remote "curl -s -o /dev/null -w '%{http_code}' --max-time 5 http://127.0.0.1:$1/api/health || true"; }

VERSION="$(head -n1 VERSION | tr -d '[:space:]')"
GIT_COMMIT="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
STARTED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

# --- 1. preflight -----------------------------------------------------------
say "Target $TARGET:$SSH_PORT  root=$REMOTE_ROOT  service=$SERVICE"
say "Checking SSH key authentication"
remote 'echo ok' >/dev/null 2>&1 || die "cannot authenticate to $TARGET with a key.
  Set one up first:
      ssh-keygen -t ed25519
      ssh-copy-id -p $SSH_PORT $TARGET
  This script will not accept a password (see the header)."

REMOTE_OS="$(remote '. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME" || uname -s')"
REMOTE_ARCH="$(remote 'uname -m')"
say "Remote: $REMOTE_OS ($REMOTE_ARCH)"

case "$REMOTE_ARCH" in
  x86_64) WANT_PLATFORM="linux-x64" ;;
  aarch64|arm64) WANT_PLATFORM="linux-arm64" ;;
  *) die "unsupported target architecture $REMOTE_ARCH" ;;
esac

# --- 2. build + package -----------------------------------------------------
if [[ -z "$PACKAGE" ]]; then
  PACKAGE="$REPO_ROOT/dist/HSF-Gateway-v${VERSION}-${WANT_PLATFORM}.tar.gz"
  if [[ $SKIP_BUILD -eq 0 ]]; then
    say "Building and packaging for $WANT_PLATFORM"
    ./scripts/build-ubuntu.sh
    ./scripts/package-ubuntu.sh
  fi
fi
[[ -f "$PACKAGE" ]] || die "no package at $PACKAGE (build first, or pass --package)"

# The package name encodes the architecture it was built for. Shipping an x64
# build to an arm64 board produces "cannot execute binary file" three steps
# later, at the restart, after the old release has already been stopped.
case "$(basename "$PACKAGE")" in
  *"$WANT_PLATFORM"*) ;;
  *) die "package $(basename "$PACKAGE") does not match the target's $WANT_PLATFORM" ;;
esac

SHA="$(sha256sum "$PACKAGE" | cut -d' ' -f1)"
say "Package $(basename "$PACKAGE")"
say "sha256  $SHA"

if [[ $DRY_RUN -eq 1 ]]; then
  say "--dry-run: stopping before anything is uploaded or changed"
  exit 0
fi

# --- 3. upload --------------------------------------------------------------
REMOTE_TMP="/tmp/hsf-deploy-$VERSION-$$"
say "Uploading to $REMOTE_TMP"
remote "mkdir -p '$REMOTE_TMP'"
if command -v rsync >/dev/null 2>&1; then
  rsync -az --info=progress2 -e "ssh ${SSH_OPTS[*]}" "$PACKAGE" "$TARGET:$REMOTE_TMP/"
else
  scp "${SSH_OPTS[@]}" "$PACKAGE" "$TARGET:$REMOTE_TMP/"
fi

# --- 4. verify the upload ---------------------------------------------------
say "Verifying the uploaded package"
REMOTE_SHA="$(remote "sha256sum '$REMOTE_TMP/$(basename "$PACKAGE")' | cut -d' ' -f1")"
[[ "$REMOTE_SHA" == "$SHA" ]] || {
  remote "rm -rf '$REMOTE_TMP'"
  die "checksum mismatch after upload (local $SHA, remote $REMOTE_SHA)"
}
say "Checksum matches"

# --- 5. what is live now, so we can put it back -----------------------------
PREVIOUS="$(remote "readlink '$REMOTE_ROOT/current' 2>/dev/null | xargs -r basename || true")"
FIRST_INSTALL=0
[[ -z "$PREVIOUS" ]] && FIRST_INSTALL=1
if [[ $FIRST_INSTALL -eq 1 ]]; then
  say "No existing install detected -- this is a first install"
else
  say "Currently live: $PREVIOUS"
fi

# Read the port from the target's OWN configuration rather than guessing
# (section 3.12). Falls back to the documented default only when there is no
# config yet, i.e. on a first install.
if [[ -z "$HEALTH_PORT" ]]; then
  HEALTH_PORT="$(remote "
    if command -v sqlite3 >/dev/null 2>&1 && [ -f '$REMOTE_ROOT/data/config.db' ]; then
      sqlite3 '$REMOTE_ROOT/data/config.db' \"SELECT json_extract(data,'\\\$.port') FROM config WHERE section='web';\" 2>/dev/null
    fi" | tr -d '[:space:]')"
  [[ -n "$HEALTH_PORT" && "$HEALTH_PORT" != "null" ]] || HEALTH_PORT=8080
fi
say "Health check will use port $HEALTH_PORT"

# --- 6. install -------------------------------------------------------------
#
# Delegated to install-ota.sh, which already lays out releases/ + current +
# data/, seeds the data directory without overwriting, and installs the unit.
# Deploying through it keeps this script and over-the-air updates using one
# mechanism.
say "Installing on the target"
remote "set -e
  cd '$REMOTE_TMP'
  tar -xzf '$(basename "$PACKAGE")'
  DIR=\$(find . -maxdepth 1 -type d -name 'HSF-Gateway-*' | head -1)
  [ -n \"\$DIR\" ] || { echo 'package did not unpack as expected' >&2; exit 1; }
  chmod +x \"\$DIR/scripts/install-ota.sh\" 2>/dev/null || true
  sudo -n bash \"\$DIR/scripts/install-ota.sh\" --package \"\$DIR\" --root '$REMOTE_ROOT' --user hsf --service
" || {
  remote "rm -rf '$REMOTE_TMP'"
  die "installation failed on the target.
  If this was a sudo prompt, give '$USER_NAME' passwordless sudo for the installer, e.g. in /etc/sudoers.d/hsf-deploy:
      $USER_NAME ALL=(root) NOPASSWD: /bin/bash /opt/hsf-gateway/releases/*/scripts/install-ota.sh, /bin/systemctl * $SERVICE"
}

# --- 7. restart -------------------------------------------------------------
say "Restarting $SERVICE"
remote "sudo -n systemctl daemon-reload && sudo -n systemctl restart '$SERVICE'" \
  || warn "could not restart via systemd -- health check will decide"

# --- 8. health check (section 3.11) -----------------------------------------
say "Waiting up to ${HEALTH_TIMEOUT}s for /api/health"
HEALTHY=0
for _ in $(seq 1 "$HEALTH_TIMEOUT"); do
  CODE="$(remote_health "$HEALTH_PORT")"
  if [[ "$CODE" == "200" ]]; then HEALTHY=1; break; fi
  sleep 1
done

DEPLOYED="$(remote "readlink '$REMOTE_ROOT/current' 2>/dev/null | xargs -r basename || true")"

if [[ $HEALTHY -eq 1 ]]; then
  say "Healthy: $(remote "curl -s --max-time 5 http://127.0.0.1:$HEALTH_PORT/api/health")"
  RESULT="SUCCESS"
  ROLLBACK="not needed"
else
  warn "Health check FAILED after ${HEALTH_TIMEOUT}s"
  remote "sudo -n systemctl status '$SERVICE' --no-pager -n 20 || true" || true
  remote "sudo -n journalctl -u '$SERVICE' -n 40 --no-pager || true" || true

  # --- 9. automatic rollback (section 3.13) ---------------------------------
  if [[ $FIRST_INSTALL -eq 1 || -z "$PREVIOUS" ]]; then
    RESULT="FAILED"
    ROLLBACK="impossible -- no previous release to return to"
    warn "$ROLLBACK"
  else
    warn "Rolling back to $PREVIOUS"
    # Relative symlink, replaced atomically -- the same form install-ota.sh and
    # UpdateInstaller write, so the three cannot disagree about the layout.
    remote "sudo -n ln -sfn 'releases/$PREVIOUS' '$REMOTE_ROOT/current.rollback' \
            && sudo -n mv -T '$REMOTE_ROOT/current.rollback' '$REMOTE_ROOT/current' \
            && sudo -n systemctl restart '$SERVICE'" || warn "the rollback command itself failed"
    sleep 5
    if [[ "$(remote_health "$HEALTH_PORT")" == "200" ]]; then
      RESULT="FAILED"
      ROLLBACK="rolled back to $PREVIOUS, which is healthy"
      warn "$ROLLBACK"
    else
      RESULT="FAILED"
      ROLLBACK="rolled back to $PREVIOUS, but it is NOT healthy -- manual intervention needed"
      warn "$ROLLBACK"
    fi
    DEPLOYED="$PREVIOUS"
  fi
fi

remote "rm -rf '$REMOTE_TMP'"

# --- 10. deployment log (section 3.14) --------------------------------------
#
# No credential of any kind is recorded, because none exists in this process to
# record.
LOG_DIR="$REPO_ROOT/dist/deploy-logs"
mkdir -p "$LOG_DIR"
LOG="$LOG_DIR/deploy-$(date -u +%Y%m%dT%H%M%SZ)-$HOST.log"
{
  echo "Started:        $STARTED_AT"
  echo "Finished:       $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "Version:        $VERSION"
  echo "Git commit:     $GIT_COMMIT"
  echo "Build machine:  $(uname -srm) / $(hostname 2>/dev/null || echo unknown)"
  echo "Target:         $TARGET:$SSH_PORT"
  echo "Target OS:      $REMOTE_OS ($REMOTE_ARCH)"
  echo "Package:        $(basename "$PACKAGE")"
  echo "Package SHA256: $SHA"
  echo "Previous:       ${PREVIOUS:-<none>}"
  echo "Now live:       ${DEPLOYED:-<unknown>}"
  echo "Health check:   $([[ $HEALTHY -eq 1 ]] && echo PASS || echo FAIL) (port $HEALTH_PORT)"
  echo "Rollback:       $ROLLBACK"
  echo "Result:         $RESULT"
} | tee "$LOG"
say "Deployment log: $LOG"

[[ "$RESULT" == "SUCCESS" ]]
