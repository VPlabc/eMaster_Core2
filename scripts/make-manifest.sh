#!/usr/bin/env bash
#
# Builds the update manifest (version.json) the gateway's UpdateManager polls,
# and signs each package listed in it (request/CICD.md sections 1, 2 and 7).
#
# Run after every package for a tag has been built and collected into one
# directory -- the release workflow does exactly that in its publish job.
#
# WHAT GETS SIGNED. Not the archive: the three fields that identify one build,
# newline separated, in this order and with a trailing newline --
#
#     <version>\n<platform>\n<sha256-hex>\n
#
# This must stay byte-identical to SignatureVerifier::BuildSignedMessage in
# src/update/SignatureVerifier.cpp. Signing the digest rather than the archive
# keeps verification cheap on an ARM board while still binding the bytes; the
# version and platform are in there because a signature over a bare digest can
# be replayed to serve a genuinely-signed OLDER package and roll a gateway back
# onto a known hole.
#
# Usage:
#   scripts/make-manifest.sh --version 1.2.3 \
#                            --dir dist \
#                            --base-url https://github.com/OWNER/REPO/releases/download/v1.2.3 \
#                            [--key private.pem] [--notes RELEASE_NOTES.md] \
#                            [--mandatory] [--min-version 1.1.0] [--channel stable]
#
# Without --key (or with the key unreadable) the manifest is still written, but
# every entry lacks a `signature` and a gateway with update.require_signature on
# -- the default -- will refuse it. That is the intended behaviour for a build
# where the signing secret is not available: produce something honest and
# unusable, rather than something usable and unsigned.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

VERSION=""
DIR="$REPO_ROOT/dist"
BASE_URL=""
KEY=""
NOTES=""
MANDATORY="false"
MIN_VERSION=""
CHANNEL="stable"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --version)     VERSION="$2"; shift 2 ;;
    --dir)         DIR="$2"; shift 2 ;;
    --base-url)    BASE_URL="${2%/}"; shift 2 ;;
    --key)         KEY="$2"; shift 2 ;;
    --notes)       NOTES="$2"; shift 2 ;;
    --min-version) MIN_VERSION="$2"; shift 2 ;;
    --channel)     CHANNEL="$2"; shift 2 ;;
    --mandatory)   MANDATORY="true"; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

[[ -n "$VERSION" ]] || VERSION="$(head -n1 "$REPO_ROOT/VERSION" | tr -d '[:space:]')"
if ! [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+ ]]; then
  echo "--version must be a semantic version like 1.2.3, got '$VERSION'" >&2
  exit 1
fi
[[ -d "$DIR" ]] || { echo "package directory $DIR does not exist" >&2; exit 1; }
[[ -n "$BASE_URL" ]] || { echo "--base-url is required (where the packages will be served from)" >&2; exit 1; }

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -d' ' -f1
  else
    shasum -a 256 "$1" | cut -d' ' -f1   # macOS
  fi
}

# Ed25519 is the default the docs recommend, but this works for any key type
# OpenSSL can verify with -- an RSA or EC key just takes the digest path in
# SignatureVerifier instead. `pkeyutl -rawin` is what makes the pure-EdDSA
# one-shot form work; for RSA/EC it hashes with SHA-256 the same way the
# gateway does.
#
# Takes the three fields SEPARATELY and assembles the message here, rather than
# accepting an already-built string. That is not stylistic: `msg="$(printf
# '%s\n%s\n%s\n' ...)"` silently drops the trailing newline, because command
# substitution strips them -- so the script signed 63 bytes while
# SignatureVerifier::BuildSignedMessage verified 64, and every package failed
# on the device with "signature does not match" after a clean download and a
# matching checksum. Keeping the assembly on this side of the call means there
# is no substitution for a newline to disappear into.
#
# The message also goes through a temp FILE rather than a pipe: `pkeyutl
# -rawin` is a one-shot operation that asks its input for a size up front, and
# a pipe fails with "unable to determine file size for oneshot operation".
sign_message() {
  local version="$1" platform="$2" hash="$3"
  local key_type message_file signature
  key_type="$(openssl pkey -in "$KEY" -noout -text 2>/dev/null | head -n1 || true)"
  message_file="$(mktemp)"
  printf '%s\n%s\n%s\n' "$version" "$platform" "$hash" > "$message_file"
  if printf '%s' "$key_type" | grep -qi 'ED25519\|ED448'; then
    signature="$(openssl pkeyutl -sign -inkey "$KEY" -rawin -in "$message_file" | openssl base64 -A)"
  else
    signature="$(openssl dgst -sha256 -sign "$KEY" -binary "$message_file" | openssl base64 -A)"
  fi
  rm -f "$message_file"
  printf '%s' "$signature"
}

RELEASE_DATE="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

# Release notes as a JSON array of strings: one entry per bullet in the
# CHANGELOG section for this version. The popup shows the first five, so the
# order in CHANGELOG.md is the order the operator reads.
#
# Built with awk rather than a json module, because this script has to run
# wherever a release is cut -- a CI runner, a maintainer's laptop, a build
# container -- and "python3 is on PATH" is not something a release step should
# depend on. (It was, briefly: on Windows `command -v python3` finds the
# Microsoft Store's placeholder, which prints an advert and exits 9009, and the
# notes silently came out as an empty array.)
NOTES_JSON="[]"
if [[ -n "$NOTES" && -f "$NOTES" ]]; then
  NOTES_JSON="$(
    awk '
      # CHANGELOG bullets only; prose paragraphs and sub-headings are not notes.
      /^[[:space:]]*[-*][[:space:]]+/ {
        line = $0
        sub(/^[[:space:]]*[-*][[:space:]]+/, "", line)
        sub(/[[:space:]]+$/, "", line)
        if (line == "") next
        gsub(/\\/, "\\\\", line)   # backslash first, or it re-escapes the ones below
        gsub(/"/,  "\\\"", line)
        gsub(/\t/, " ", line)
        gsub(/\r/, "", line)
        notes[++count] = line
      }
      END {
        if (count > 20) count = 20
        printf "["
        for (i = 1; i <= count; i++) printf "%s\"%s\"", (i > 1 ? ", " : ""), notes[i]
        printf "]"
      }
    ' "$NOTES"
  )"
fi

MANIFEST="$DIR/version.json"
SIGNED_COUNT=0
UNSIGNED_COUNT=0

{
  printf '{\n'
  printf '  "version": %s,\n' "\"$VERSION\""
  printf '  "channel": %s,\n' "\"$CHANNEL\""
  printf '  "release_date": %s,\n' "\"$RELEASE_DATE\""
  printf '  "mandatory": %s,\n' "$MANDATORY"
  [[ -n "$MIN_VERSION" ]] && printf '  "min_version": %s,\n' "\"$MIN_VERSION\""
  printf '  "release_notes": %s,\n' "$NOTES_JSON"
  printf '  "platforms": {\n'

  FIRST=1
  # Both extensions: Linux/macOS ship .tar.gz, Windows ships .zip, and the
  # installer picks the unpack mode from the suffix.
  for package in "$DIR"/HSF-Gateway-v"$VERSION"-*.tar.gz "$DIR"/HSF-Gateway-v"$VERSION"-*.zip "$DIR"/eMaster-"$VERSION"-*.tar.gz "$DIR"/eMaster-"$VERSION"-*.zip; do
    [[ -e "$package" ]] || continue
    filename="$(basename "$package")"

    # HSF-Gateway-v1.2.3-linux-x64.tar.gz -> linux-x64. This label has to match
    # HSF_PLATFORM as CMake computes it, or a gateway looks itself up in the
    # platforms map and finds nothing.
    platform="${filename#HSF-Gateway-v$VERSION-}"
    platform="${platform#eMaster-$VERSION-}"
    platform="${platform%.tar.gz}"
    platform="${platform%.zip}"

    hash="$(sha256_of "$package")"
    size="$(wc -c < "$package" | tr -d '[:space:]')"

    signature=""
    if [[ -n "$KEY" && -r "$KEY" ]]; then
      signature="$(sign_message "$VERSION" "$platform" "$hash")"
      SIGNED_COUNT=$((SIGNED_COUNT + 1))
    else
      UNSIGNED_COUNT=$((UNSIGNED_COUNT + 1))
    fi

    [[ $FIRST -eq 1 ]] || printf ',\n'
    FIRST=0
    printf '    "%s": {\n' "$platform"
    printf '      "platform": "%s",\n' "$platform"
    printf '      "url": "%s/%s",\n' "$BASE_URL" "$filename"
    printf '      "sha256": "%s",\n' "$hash"
    printf '      "size_bytes": %s' "$size"
    [[ -n "$signature" ]] && printf ',\n      "signature": "%s"' "$signature"
    printf '\n    }'
  done

  printf '\n  }\n}\n'
} > "$MANIFEST"

echo "[manifest] $MANIFEST"
if [[ $((SIGNED_COUNT + UNSIGNED_COUNT)) -eq 0 ]]; then
  echo "No artifacts found for version $VERSION" >&2
  exit 1
fi
echo "[manifest] $SIGNED_COUNT signed, $UNSIGNED_COUNT unsigned"
if [[ $UNSIGNED_COUNT -gt 0 ]]; then
  echo "[manifest] WARNING: unsigned entries will be REFUSED by any gateway with" >&2
  echo "[manifest]          update.require_signature on (the default)." >&2
fi

# Parse it back before anyone publishes it: a manifest that is not valid JSON
# fails on every device in the fleet simultaneously, and the cheapest place to
# find that out is here. Whichever parser the machine has -- and `python3 -c`
# is probed by running it, not by `command -v`, which on Windows finds the
# Store placeholder and reports success for a binary that does nothing.
if command -v jq >/dev/null 2>&1; then
  jq empty < "$MANIFEST"
  echo "[manifest] JSON is valid (jq)"
elif python3 -c 'pass' >/dev/null 2>&1; then
  python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$MANIFEST"
  echo "[manifest] JSON is valid (python3)"
else
  echo "[manifest] no jq or python3 here; manifest NOT syntax-checked" >&2
fi
