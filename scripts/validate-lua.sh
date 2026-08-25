#!/usr/bin/env bash
#
# Syntax-checks every Lua script under config/scripts/ (request/release.md
# section 11, "Syntax validation").
#
# Uses `luac -p` (parse only, emit nothing) when available. That checks
# syntax, NOT that the gateway's Lua API calls inside are correct -- a script
# calling a function that does not exist parses fine and fails at runtime.
# The runtime checks in section 11 are still a manual step; see
# docs/release-checklist.md.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT_DIR="${1:-$REPO_ROOT/config/scripts}"

LUAC="$(command -v luac5.4 || command -v luac54 || command -v luac || true)"
if [[ -z "$LUAC" ]]; then
  echo "luac not found on PATH -- cannot syntax-check Lua scripts." >&2
  echo "Install Lua 5.4 (apt install lua5.4 / brew install lua / choco install lua)." >&2
  exit 127
fi

echo "[validate-lua] $("$LUAC" -v 2>&1 | head -n1)"
echo "[validate-lua] scanning $SCRIPT_DIR"

failed=0
checked=0
while IFS= read -r -d '' file; do
  checked=$((checked + 1))
  if ! output="$("$LUAC" -p "$file" 2>&1)"; then
    echo "FAIL  ${file#$REPO_ROOT/}"
    echo "      $output"
    failed=$((failed + 1))
  fi
done < <(find "$SCRIPT_DIR" -name '*.lua' -print0 | sort -z)

echo "[validate-lua] $checked script(s) checked, $failed failed"
[[ $failed -eq 0 ]]
