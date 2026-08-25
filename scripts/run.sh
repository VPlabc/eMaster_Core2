#!/usr/bin/env bash
#
# Starts the eMaster Gateway from an extracted release package.
#
# Works from either layout: <pkg>/scripts/run.sh in a package, or
# scripts/run.sh in the source tree (where it uses build-release/ if present).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

if [[ -x "$ROOT/bin/hsf_gateway" ]]; then
  EXE="$ROOT/bin/hsf_gateway"
elif [[ -x "$ROOT/build-release/hsf_gateway" ]]; then
  EXE="$ROOT/build-release/hsf_gateway"
elif [[ -x "$ROOT/build/hsf_gateway" ]]; then
  EXE="$ROOT/build/hsf_gateway"
else
  echo "hsf_gateway not found. Build it first (scripts/package.sh) or run this from an extracted package." >&2
  exit 1
fi

CONFIG_DIR="$ROOT/config"

# First run: seed config.json from the template so ConfigManager imports it
# into a fresh config.db. Never overwrite an existing one -- that would
# discard live settings, including the REST API key.
if [[ -d "$CONFIG_DIR" && ! -f "$CONFIG_DIR/config.db" && ! -f "$CONFIG_DIR/config.json" ]]; then
  if [[ -f "$CONFIG_DIR/config.example.json" ]]; then
    cp "$CONFIG_DIR/config.example.json" "$CONFIG_DIR/config.json"
    echo "[run] Seeded config/config.json from the template."
    echo "[run] Edit it (or the Configuration page) and replace the YOUR_* placeholders."
  fi
fi

echo "[run] $("$EXE" --version | head -n2 | tr '\n' ' ')"
exec "$EXE" "$@"
