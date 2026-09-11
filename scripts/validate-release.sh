#!/usr/bin/env bash
set -euo pipefail

STAGE_DIR="${1:?usage: validate-release.sh <stage-directory>}"
[[ -d "$STAGE_DIR/bin" && -d "$STAGE_DIR/web" && -d "$STAGE_DIR/config" ]] || { echo "release is missing bin/web/config" >&2; exit 1; }
[[ -f "$STAGE_DIR/config/config.example.json" ]] || { echo "release is missing config/config.example.json" >&2; exit 1; }

forbidden="$(find "$STAGE_DIR" -type f \( -name '*.db' -o -name '*.db-*' -o -name 'config.json' \) -print -quit)"
[[ -z "$forbidden" ]] || { echo "release contains forbidden file: $forbidden" >&2; exit 1; }
forbidden="$(find "$STAGE_DIR" -mindepth 2 -maxdepth 2 -type f \( -path "$STAGE_DIR/.git/*" -o -path "$STAGE_DIR/target/*" -o -path "$STAGE_DIR/build/*" -o -path "$STAGE_DIR/tests/*" -o -path "$STAGE_DIR/source/*" \) -print -quit)"
[[ -z "$forbidden" ]] || { echo "release contains forbidden developer artifact: $forbidden" >&2; exit 1; }
echo "[package] release validation passed: $STAGE_DIR"
