#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 3 ]]; then
  echo "usage: $0 <archive.tar.gz> [expected-version] [expected-sha256]" >&2
  exit 2
fi

ARCHIVE="$1"
EXPECTED="${2:-}"
EXPECTED_SHA256="${3:-}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/emaster-package.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

[[ -f "$ARCHIVE" ]] || { echo "archive not found: $ARCHIVE" >&2; exit 1; }
ACTUAL_SHA256="$(sha256sum "$ARCHIVE" 2>/dev/null | awk '{print $1}' || shasum -a 256 "$ARCHIVE" | awk '{print $1}')"
if [[ -n "$EXPECTED_SHA256" && "${ACTUAL_SHA256,,}" != "${EXPECTED_SHA256,,}" ]]; then
  echo "archive checksum '$ACTUAL_SHA256' does not match expected '$EXPECTED_SHA256'" >&2
  exit 1
fi
tar -xzf "$ARCHIVE" -C "$TMP"

mapfile -t DIRS < <(find "$TMP" -mindepth 1 -maxdepth 1 -type d -print)
[[ "${#DIRS[@]}" -eq 1 ]] || { echo "archive must contain exactly one release directory" >&2; exit 1; }
STAGE="${DIRS[0]}"

for required in manifest.json VERSION bin/hsf_gateway; do
  [[ -f "$STAGE/$required" || -x "$STAGE/$required" ]] || {
    echo "package is missing required file: $required" >&2
    exit 1
  }
done

VERSION="$(head -n1 "$STAGE/VERSION" | tr -d '[:space:]')"
MANIFEST_VERSION="$(sed -n 's/^[[:space:]]*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$STAGE/manifest.json" | head -n1)"
[[ -n "$MANIFEST_VERSION" && "$MANIFEST_VERSION" == "$VERSION" ]] || {
  echo "manifest version does not match VERSION" >&2
  exit 1
}
for field in product platform architecture build_type git_commit; do
  grep -Eq "^[[:space:]]*\"$field\"[[:space:]]*:[[:space:]]*\"[^\"]+\"" "$STAGE/manifest.json" || {
    echo "manifest is missing required field: $field" >&2
    exit 1
  }
done
grep -Eq '^[[:space:]]*"product"[[:space:]]*:[[:space:]]*"eMaster"' "$STAGE/manifest.json" || {
  echo "unsupported manifest product" >&2
  exit 1
}
grep -Eq '^[[:space:]]*"build_type"[[:space:]]*:[[:space:]]*"Release"' "$STAGE/manifest.json" || {
  echo "package build type must be Release" >&2
  exit 1
}
if [[ -n "$EXPECTED" && "$VERSION" != "$EXPECTED" ]]; then
  echo "package version '$VERSION' does not match expected '$EXPECTED'" >&2
  exit 1
fi

if find "$STAGE" -type f \( -name '*.db' -o -name 'config.json' -o -name '*.key' \) -print -quit | grep -q .; then
  echo "package contains excluded credential/database files" >&2
  exit 1
fi

bash "$ROOT/scripts/validate-release.sh" "$STAGE"
echo "[verify] package is valid: version=$VERSION sha256=$ACTUAL_SHA256 archive=$ARCHIVE"
