#!/usr/bin/env bash
#
# Automated tests for Phase 2 (request/AdvanceUpdate.md, "Phase 2 Testing"):
# compile, package, verify, deploy, run and roll back production Lua.
#
# The test that matters most is the last group: it MOVES THE SOURCE TREE AWAY
# and restarts, because everything else passes just as happily when require()
# is quietly falling through to the plaintext .lua next door. That is not a
# hypothetical -- it is the bug this suite was written after finding.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GATEWAY="${1:-$REPO_ROOT/build/hsf_gateway}"
PORT="${2:-18090}"
BASE="http://127.0.0.1:${PORT}"

[[ -x "$GATEWAY" ]] || { echo "no gateway binary at $GATEWAY" >&2; exit 2; }

WORK="$(mktemp -d)"
GATEWAY_PID=""
start_gateway() {
  "$GATEWAY" "$WORK/config.db" >> "$WORK/gateway.log" 2>&1 &
  GATEWAY_PID=$!
  for _ in $(seq 1 40); do
    [[ "$(curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$BASE/api/auth/mode")" == "200" ]] && return 0
    sleep 0.5
  done
  return 1
}
stop_gateway() {
  [[ -n "$GATEWAY_PID" ]] && kill "$GATEWAY_PID" 2>/dev/null && wait "$GATEWAY_PID" 2>/dev/null
  GATEWAY_PID=""
  sleep 1
}
cleanup() { stop_gateway; rm -rf "$WORK" 2>/dev/null; }
trap cleanup EXIT

PASSED=0
FAILED=0
check() {
  local label="$1" expected="$2" actual="$3"
  if [[ "|$expected|" == *"|$actual|"* ]]; then
    printf '  \033[32mok\033[0m   %-56s %s\n' "$label" "$actual"; PASSED=$((PASSED + 1))
  else
    printf '  \033[31mFAIL\033[0m %-56s got [%s] want [%s]\n' "$label" "$actual" "$expected"; FAILED=$((FAILED + 1))
  fi
}

# --- a three-module application, so require() is actually exercised ---------
mkdir -p "$WORK/scripts/demoapp/util"
cat > "$WORK/scripts/demoapp/main.lua" <<'LUA'
local greet = require("demoapp.greeting")
local numbers = require("demoapp.util.numbers")
SetVariable("PkgGreeting", greet.hello("world"))
SetVariable("PkgSum", tostring(numbers.add(20, 22)))
LUA
cat > "$WORK/scripts/demoapp/greeting.lua" <<'LUA'
local M = {}
function M.hello(who) return "hello, " .. who end
return M
LUA
cat > "$WORK/scripts/demoapp/util/numbers.lua" <<'LUA'
local M = {}
function M.add(a, b) return a + b end
return M
LUA

cat > "$WORK/config.json" <<JSON
{ "web": { "port": ${PORT}, "bind_address": "127.0.0.1" },
  "lua": { "script_path": "" },
  "auth": { "enabled": true, "login_rate_per_sec": 1000, "login_burst": 1000,
            "api_rate_per_sec": 1000, "api_burst": 1000 } }
JSON

echo "Starting gateway on port $PORT (workspace $WORK)"
start_gateway || { echo "gateway did not start" >&2; cat "$WORK/gateway.log" >&2; exit 1; }

ADMIN_PW="$(sed -n 's/.*password:  *\([^ ]*\).*/\1/p' "$WORK/gateway.log" | head -1)"
TOKEN="$(curl -s --max-time 10 -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' \
  -d "{\"username\":\"admin\",\"password\":\"$ADMIN_PW\"}" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')"
[[ -n "$TOKEN" ]] || { echo "could not log in" >&2; exit 1; }
AUTH=(-H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json')

status() { curl -s -o /dev/null -w '%{http_code}' --max-time 30 "$@"; }
body()   { curl -s --max-time 30 "$@"; }
variable() { body "${AUTH[@]}" "$BASE/api/variables" | sed -n "s/.*\"$1\":\"\([^\"]*\)\".*/\1/p"; }

echo
echo "== Keys =="
check "build refused with no signing key"    400 "$(status -X POST "${AUTH[@]}" -d '{"app_id":"demoapp","version":"1.0.0","entry":"main.lua","source_dir":"demoapp"}' "$BASE/api/lua/packages/build")"
check "signing key can be created"           201 "$(status -X POST "${AUTH[@]}" "$BASE/api/lua/packages/keys")"
check "creating a second key is refused"     409 "$(status -X POST "${AUTH[@]}" "$BASE/api/lua/packages/keys")"

echo
echo "== Build & Test =="
BUILD="$(body -X POST "${AUTH[@]}" -d '{"app_id":"demoapp","version":"1.0.0","entry":"main.lua","source_dir":"demoapp"}' "$BASE/api/lua/packages/build")"
check "build succeeds"                       true "$(sed -n 's/.*"ok":\(true\|false\).*/\1/p' <<<"$BUILD")"
check "all three modules compiled"           3 "$(sed -n 's/.*"modules_compiled":\([0-9]*\).*/\1/p' <<<"$BUILD")"
for stage in compile sign verify load write; do
  if grep -q "\"$stage\"" <<<"$BUILD"; then
    printf '  \033[32mok\033[0m   %-56s passed\n' "stage: $stage"; PASSED=$((PASSED + 1))
  else
    printf '  \033[31mFAIL\033[0m %-56s missing\n' "stage: $stage"; FAILED=$((FAILED + 1))
  fi
done

echo
echo "== Rejections =="
printf 'function broken(\n' > "$WORK/scripts/demoapp/broken.lua"
BAD="$(body -X POST "${AUTH[@]}" -d '{"app_id":"demoapp","version":"1.0.1","entry":"main.lua","source_dir":"demoapp"}' "$BASE/api/lua/packages/build")"
check "syntax error fails at the compile stage" compile "$(sed -n 's/.*"failed_stage":"\([a-z]*\)".*/\1/p' <<<"$BAD")"
check "and reports a line number"            true "$(grep -q '"error_line"' <<<"$BAD" && echo true || echo false)"
rm -f "$WORK/scripts/demoapp/broken.lua"
check "traversal in source_dir refused"      400 "$(status -X POST "${AUTH[@]}" -d '{"app_id":"x","version":"1.0.0","entry":"main.lua","source_dir":"../../etc"}' "$BASE/api/lua/packages/build")"
check "non-semver version refused"           400 "$(status -X POST "${AUTH[@]}" -d '{"app_id":"demoapp","version":"latest","entry":"main.lua","source_dir":"demoapp"}' "$BASE/api/lua/packages/build")"
check "missing entry refused"                400 "$(status -X POST "${AUTH[@]}" -d '{"app_id":"demoapp","version":"1.0.2","entry":"nope.lua","source_dir":"demoapp"}' "$BASE/api/lua/packages/build")"

echo
echo "== The artifact does not contain plaintext =="
PKG="$WORK/lua_packages/demoapp-1.0.0.pkg"
check "package file exists"                  true "$([[ -f "$PKG" ]] && echo true || echo false)"
check "no source string in the artifact"     false "$(grep -qa 'hello, ' "$PKG" && echo true || echo false)"
check "no module name in the clear"          false "$(grep -qa 'function M.hello' "$PKG" && echo true || echo false)"

echo
echo "== Deploy and run =="
check "deploy succeeds"                      200 "$(status -X POST "${AUTH[@]}" -d '{"file":"demoapp-1.0.0.pkg"}' "$BASE/api/lua/packages/deploy")"
sleep 2
check "packaged app ran (require #1)"        "hello, world" "$(variable PkgGreeting)"
check "packaged app ran (require #2)"        "42" "$(variable PkgSum)"

echo
echo "== Runs with NO source on disk =="
# The point of the whole exercise. With the sources present, a package whose
# module names are wrong still works, because require() falls back to
# package.path -- which is exactly how a broken package reaches a device.
stop_gateway
mv "$WORK/scripts/demoapp" "$WORK/demoapp-sources-moved"
start_gateway || { echo "gateway did not restart" >&2; exit 1; }
TOKEN="$(curl -s --max-time 10 -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' \
  -d "{\"username\":\"admin\",\"password\":\"$ADMIN_PW\"}" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')"
AUTH=(-H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json')
sleep 2
check "auto-started from the package alone"  "hello, world" "$(variable PkgGreeting)"
check "in-memory require resolved"           "42" "$(variable PkgSum)"
check "package.path was emptied"             true "$(grep -q "package.path is empty" "$WORK/gateway.log" && echo true || echo false)"
mv "$WORK/demoapp-sources-moved" "$WORK/scripts/demoapp"

echo
echo "== Tampering =="
stop_gateway
# Flip a byte in the ciphertext of a COPY, deploy that, and confirm it is
# refused rather than executed.
cp "$PKG" "$WORK/lua_packages/demoapp-9.9.9.pkg"
# Flipped inside the CIPHERTEXT -- ten bytes before the 64-byte trailing
# signature. Not at the midpoint: that lands in the plaintext JSON header,
# which is rejected as malformed JSON before the signature is ever checked, so
# it exercises the parser rather than the crypto.
TAMPER_AT=$(( $(wc -c < "$WORK/lua_packages/demoapp-9.9.9.pkg") - 64 - 10 ))
printf '\xff' | dd of="$WORK/lua_packages/demoapp-9.9.9.pkg" bs=1 seek="$TAMPER_AT" count=1 \
  conv=notrunc status=none
start_gateway || { echo "gateway did not restart" >&2; exit 1; }
TOKEN="$(curl -s --max-time 10 -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' \
  -d "{\"username\":\"admin\",\"password\":\"$ADMIN_PW\"}" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')"
AUTH=(-H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json')
check "tampered package refused at deploy"   400 "$(status -X POST "${AUTH[@]}" -d '{"file":"demoapp-9.9.9.pkg"}' "$BASE/api/lua/packages/deploy")"
check "refusal is logged as a signature failure" true "$(grep -q "REFUSED Lua package" "$WORK/gateway.log" && echo true || echo false)"
check "unknown package refused"              400 "$(status -X POST "${AUTH[@]}" -d '{"file":"nope.pkg"}' "$BASE/api/lua/packages/deploy")"

echo
echo "== Versioning and rollback =="
sed -i 's/hello, /greetings, /' "$WORK/scripts/demoapp/greeting.lua"
check "second version builds"                200 "$(status -X POST "${AUTH[@]}" -d '{"app_id":"demoapp","version":"1.1.0","entry":"main.lua","source_dir":"demoapp"}' "$BASE/api/lua/packages/build")"
check "second version deploys"               200 "$(status -X POST "${AUTH[@]}" -d '{"file":"demoapp-1.1.0.pkg"}' "$BASE/api/lua/packages/deploy")"
sleep 2
check "the new version is what runs"         "greetings, world" "$(variable PkgGreeting)"
check "deployed package cannot be deleted"   400 "$(status -X POST "${AUTH[@]}" -d '{"file":"demoapp-1.1.0.pkg"}' "$BASE/api/lua/packages/delete")"
check "rollback target cannot be deleted"    400 "$(status -X POST "${AUTH[@]}" -d '{"file":"demoapp-1.0.0.pkg"}' "$BASE/api/lua/packages/delete")"
check "rollback succeeds"                    200 "$(status -X POST "${AUTH[@]}" "$BASE/api/lua/packages/rollback")"
sleep 2
check "the previous version is running again" "hello, world" "$(variable PkgGreeting)"

echo
echo "== Authorization =="
curl -s -o /dev/null --max-time 10 -X POST "${AUTH[@]}" \
  -d '{"username":"viewer","password":"viewer-password-1","role":"USER"}' "$BASE/api/users"
VT="$(curl -s --max-time 10 -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' \
  -d '{"username":"viewer","password":"viewer-password-1"}' | sed -n 's/.*"token":"\([^"]*\)".*/\1/p')"
VIEW=(-H "Authorization: Bearer $VT" -H 'Content-Type: application/json')
check "viewer may list packages"             200 "$(status "${VIEW[@]}" "$BASE/api/lua/packages")"
check "viewer may NOT build"                 403 "$(status -X POST "${VIEW[@]}" -d '{"app_id":"demoapp","version":"2.0.0","entry":"main.lua","source_dir":"demoapp"}' "$BASE/api/lua/packages/build")"
check "viewer may NOT deploy"                403 "$(status -X POST "${VIEW[@]}" -d '{"file":"demoapp-1.0.0.pkg"}' "$BASE/api/lua/packages/deploy")"
check "viewer may NOT roll back"             403 "$(status -X POST "${VIEW[@]}" "$BASE/api/lua/packages/rollback")"
check "viewer may NOT create signing keys"   403 "$(status -X POST "${VIEW[@]}" "$BASE/api/lua/packages/keys")"

echo
echo "============================================================"
echo "  passed: $PASSED   failed: $FAILED"
echo "============================================================"
[[ $FAILED -eq 0 ]]
