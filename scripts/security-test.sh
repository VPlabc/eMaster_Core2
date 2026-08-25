#!/usr/bin/env bash
#
# Automated security tests for Phase 1 (request/AdvanceUpdate.md, "Phase 1
# Testing"). Starts a gateway against a throwaway config directory, reads the
# one-time admin password it prints, and exercises the checklist end to end.
#
# This is a black-box test against the real HTTP surface on purpose. The
# properties worth testing here -- "an unauthenticated caller gets 401", "a
# viewer cannot pulse a relay", "the sixth bad password locks the account" --
# are properties of the whole pipeline, and a unit test that called
# SecurityStore directly would keep passing if the middleware stopped being
# installed.
#
# Usage:
#   scripts/security-test.sh [path-to-hsf_gateway] [port]
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GATEWAY="${1:-$REPO_ROOT/build/hsf_gateway}"
PORT="${2:-18080}"
BASE="http://127.0.0.1:${PORT}"

[[ -x "$GATEWAY" ]] || { echo "no gateway binary at $GATEWAY" >&2; exit 2; }
command -v curl >/dev/null || { echo "curl is required" >&2; exit 2; }

WORK="$(mktemp -d)"
GATEWAY_PID=""
cleanup() {
  if [[ -n "$GATEWAY_PID" ]]; then
    kill "$GATEWAY_PID" 2>/dev/null
    # Give it a moment to close its SQLite handles, or the removal below trips
    # over open WAL files and prints a wall of noise after the results.
    wait "$GATEWAY_PID" 2>/dev/null
    sleep 1
  fi
  rm -rf "$WORK" 2>/dev/null
}
trap cleanup EXIT

PASSED=0
FAILED=0

# `expected` may be a single code or a |-separated set, because some checks
# have more than one correct answer (a rate-limited request is 429 whether or
# not it would also have been 401).
check() {
  local label="$1" expected="$2" actual="$3"
  if [[ "|$expected|" == *"|$actual|"* ]]; then
    printf '  \033[32mok\033[0m   %-58s %s\n' "$label" "$actual"
    PASSED=$((PASSED + 1))
  else
    printf '  \033[31mFAIL\033[0m %-58s got %s, want %s\n' "$label" "$actual" "$expected"
    FAILED=$((FAILED + 1))
  fi
}

status() { curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$@"; }

# --- start ------------------------------------------------------------------

echo "Starting gateway on port $PORT with a fresh config in $WORK"
# The web port has to be set before the first request, and config.db does not
# exist yet -- so seed it from the legacy-JSON import path ConfigManager
# supports, which is exactly what that path is for.
#
# The login rate limit is set generously OUT OF THE WAY here. It and the
# account lockout defend the same thing, and whichever trips first hides the
# other -- the first version of this script drained the login bucket during the
# RBAC phase and then watched the lockout test get 429s instead of 423s. The
# lockout is tested at its real settings; the limiter is re-tightened over
# /api/config at the end and tested on its own.
#
# The API bucket is deliberately TINY, for the opposite reason: each curl is a
# process spawn, so 250 sequential requests take ~12 s, and at the production
# 40/s the bucket refills faster than a shell loop can drain it.
cat > "$WORK/config.json" <<JSON
{ "web": { "port": ${PORT}, "bind_address": "127.0.0.1" },
  "auth": { "enabled": true,
            "max_failed_attempts": 5,
            "lockout_seconds": 900,
            "login_rate_per_sec": 1000, "login_burst": 1000,
            "api_rate_per_sec": 1000, "api_burst": 1000 } }
JSON

"$GATEWAY" "$WORK/config.db" > "$WORK/gateway.log" 2>&1 &
GATEWAY_PID=$!

for _ in $(seq 1 40); do
  [[ "$(status "$BASE/api/auth/mode")" == "200" ]] && break
  sleep 0.5
done
if [[ "$(status "$BASE/api/auth/mode")" != "200" ]]; then
  echo "gateway did not come up; log follows" >&2
  cat "$WORK/gateway.log" >&2
  exit 1
fi

ADMIN_PW="$(sed -n 's/.*password:  *\([^ ]*\).*/\1/p' "$WORK/gateway.log" | head -1)"
[[ -n "$ADMIN_PW" ]] || { echo "could not read the seeded admin password" >&2; cat "$WORK/gateway.log" >&2; exit 1; }

login() { # username password -> token on stdout ("" on failure)
  curl -s --max-time 10 -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' \
    -d "{\"username\":\"$1\",\"password\":\"$2\"}" | sed -n 's/.*"token":"\([^"]*\)".*/\1/p'
}

echo
echo "== Public surface =="
check "GET /login.html is public"            200 "$(status "$BASE/login.html")"
check "GET /js/auth.js is public"            200 "$(status "$BASE/js/auth.js")"
check "GET /api/auth/mode is public"         200 "$(status "$BASE/api/auth/mode")"

echo
echo "== Authentication =="
check "no token is rejected"                 401 "$(status "$BASE/api/status")"
check "malformed Authorization is rejected"  401 "$(status -H 'Authorization: NotBearer x' "$BASE/api/status")"
check "unknown token is rejected"            401 "$(status -H 'Authorization: Bearer deadbeef' "$BASE/api/status")"
check "empty bearer is rejected"             401 "$(status -H 'Authorization: Bearer ' "$BASE/api/status")"
check "bad password"                         401 "$(status -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' -d '{"username":"admin","password":"nope"}')"
check "unknown user"                         401 "$(status -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' -d '{"username":"ghost","password":"nope"}')"

ADMIN_TOKEN="$(login admin "$ADMIN_PW")"
if [[ -z "$ADMIN_TOKEN" ]]; then
  echo "  FAIL admin login did not return a token" >&2
  FAILED=$((FAILED + 1))
else
  PASSED=$((PASSED + 1))
  printf '  \033[32mok\033[0m   %-58s\n' "admin login returns a token"
fi
AUTH=(-H "Authorization: Bearer $ADMIN_TOKEN")
check "authenticated request succeeds"       200 "$(status "${AUTH[@]}" "$BASE/api/status")"

echo
echo "== Authorization (RBAC) =="
curl -s --max-time 10 -o /dev/null -X POST "$BASE/api/users" "${AUTH[@]}" -H 'Content-Type: application/json' \
  -d '{"username":"viewer","password":"viewer-password-1","role":"USER"}'
curl -s --max-time 10 -o /dev/null -X POST "$BASE/api/users" "${AUTH[@]}" -H 'Content-Type: application/json' \
  -d '{"username":"oper","password":"operator-password-1","role":"OPERATOR"}'
VIEWER_TOKEN="$(login viewer viewer-password-1)"
OPER_TOKEN="$(login oper operator-password-1)"
VIEW=(-H "Authorization: Bearer $VIEWER_TOKEN")
OPER=(-H "Authorization: Bearer $OPER_TOKEN")

check "viewer may read status"               200 "$(status "${VIEW[@]}" "$BASE/api/status")"
check "viewer may NOT write config"          403 "$(status -X POST "${VIEW[@]}" -H 'Content-Type: application/json' -d '{}' "$BASE/api/config")"
check "viewer may NOT drive a relay"         403 "$(status -X POST "${VIEW[@]}" -H 'Content-Type: application/json' -d '{}' "$BASE/api/test/relay/pulse")"
check "viewer may NOT list users"            403 "$(status "${VIEW[@]}" "$BASE/api/users")"
check "viewer may NOT read the audit log"    403 "$(status "${VIEW[@]}" "$BASE/api/audit")"
check "viewer may NOT install an update"     403 "$(status -X POST "${VIEW[@]}" -H 'Content-Type: application/json' -d '{"version":"9.9.9"}' "$BASE/api/update/install")"
check "viewer may NOT write Lua"             403 "$(status -X POST "${VIEW[@]}" -H 'Content-Type: application/json' -d '{"code":""}' "$BASE/api/lua/scripts/file?name=x.lua")"
check "operator MAY drive a relay"           "200|400|500|503" "$(status -X POST "${OPER[@]}" -H 'Content-Type: application/json' -d '{}' "$BASE/api/test/relay/pulse")"
check "operator may NOT write config"        403 "$(status -X POST "${OPER[@]}" -H 'Content-Type: application/json' -d '{}' "$BASE/api/config")"
check "operator may NOT manage users"        403 "$(status "${OPER[@]}" "$BASE/api/users")"
check "admin MAY read config"                200 "$(status "${AUTH[@]}" "$BASE/api/config")"

echo
echo "== Session handling =="
LOGOUT_TOKEN="$(login viewer viewer-password-1)"
check "fresh token works"                    200 "$(status -H "Authorization: Bearer $LOGOUT_TOKEN" "$BASE/api/status")"
curl -s -o /dev/null --max-time 5 -X POST -H "Authorization: Bearer $LOGOUT_TOKEN" "$BASE/api/auth/logout"
check "token is dead after logout"           401 "$(status -H "Authorization: Bearer $LOGOUT_TOKEN" "$BASE/api/status")"

REVOKE_TOKEN="$(login viewer viewer-password-1)"
VIEWER_ID="$(curl -s --max-time 5 "${AUTH[@]}" "$BASE/api/users" | sed -n 's/.*{"created_at":[0-9]*,"display_name":"viewer","enabled":[a-z]*,"id":\([0-9]*\).*/\1/p')"
if [[ -n "$VIEWER_ID" ]]; then
  curl -s -o /dev/null --max-time 5 -X PATCH "${AUTH[@]}" -H 'Content-Type: application/json' \
    -d '{"enabled":false}' "$BASE/api/users/$VIEWER_ID"
  check "disabling a user kills its sessions" 401 "$(status -H "Authorization: Bearer $REVOKE_TOKEN" "$BASE/api/status")"
  curl -s -o /dev/null --max-time 5 -X PATCH "${AUTH[@]}" -H 'Content-Type: application/json' \
    -d '{"enabled":true}' "$BASE/api/users/$VIEWER_ID"
fi

echo
echo "== Changing your own password =="
# The current password must be proved, and there must be no second route that
# skips proving it.
check "wrong current password refused"       401 "$(status -X POST "${AUTH[@]}" -H 'Content-Type: application/json' -d '{"current_password":"not-the-password","new_password":"a-brand-new-password"}' "$BASE/api/auth/password")"
check "missing current password refused"     400 "$(status -X POST "${AUTH[@]}" -H 'Content-Type: application/json' -d '{"new_password":"a-brand-new-password"}' "$BASE/api/auth/password")"
check "empty current password refused"       400 "$(status -X POST "${AUTH[@]}" -H 'Content-Type: application/json' -d '{"current_password":"","new_password":"a-brand-new-password"}' "$BASE/api/auth/password")"
check "short new password refused"           400 "$(status -X POST "${AUTH[@]}" -H 'Content-Type: application/json' -d "{\"current_password\":\"$ADMIN_PW\",\"new_password\":\"short\"}" "$BASE/api/auth/password")"
check "old password still works after those" 200 "$(status -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' -d "{\"username\":\"admin\",\"password\":\"$ADMIN_PW\"}")"
# The admin reset route must not be usable as a way around the check above.
ADMIN_ID="$(curl -s --max-time 5 "${AUTH[@]}" "$BASE/api/users" |
            tr '{' '\n' | grep '"username":"admin"' | sed -n 's/.*"id":\([0-9]*\).*/\1/p' | head -1)"
if [[ -n "$ADMIN_ID" ]]; then
  check "admin cannot reset OWN password here" 400 "$(status -X PATCH "${AUTH[@]}" -H 'Content-Type: application/json' -d '{"password":"sneaky-new-password"}' "$BASE/api/users/$ADMIN_ID")"
  check "and the old password still works"     200 "$(status -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' -d "{\"username\":\"admin\",\"password\":\"$ADMIN_PW\"}")"
fi

echo
echo "== Input validation and injection =="
check "malformed JSON"                       400 "$(status -X POST "${AUTH[@]}" -H 'Content-Type: application/json' -d '{not json' "$BASE/api/users")"
head -c 200000 /dev/zero | tr '\0' 'x' > "$WORK/big.txt"
check "oversized body"                       413 "$(status -X POST "${AUTH[@]}" -H 'Content-Type: application/json' --data-binary "@$WORK/big.txt" "$BASE/api/users")"
check "short password refused"               400 "$(status -X POST "${AUTH[@]}" -H 'Content-Type: application/json' -d '{"username":"weakling","password":"short","role":"USER"}' "$BASE/api/users")"
check "bad role refused"                     400 "$(status -X POST "${AUTH[@]}" -H 'Content-Type: application/json' -d '{"username":"roletest","password":"a-long-enough-password","role":"ROOT"}' "$BASE/api/users")"
check "username charset enforced"            400 "$(status -X POST "${AUTH[@]}" -H 'Content-Type: application/json' -d '{"username":"bad name!","password":"a-long-enough-password","role":"USER"}' "$BASE/api/users")"
# SQL injection: if the username were concatenated into a query this would
# either error or authenticate. It must simply be a failed login. The payload
# goes through a FILE because it contains the single quotes that are the whole
# point of the test, and inlining it in a single-quoted shell string silently
# splits it into several curl arguments.
printf '%s' '{"username":"admin'"'"' OR '"'"'1'"'"'='"'"'1","password":"x"}' > "$WORK/sqli.json"
check "SQLi in username is inert"            401 "$(status -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' --data-binary "@$WORK/sqli.json")"
printf '%s' '{"username":"admin\"--","password":"x"}' > "$WORK/sqli2.json"
check "SQL comment in username is inert"     401 "$(status -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' --data-binary "@$WORK/sqli2.json")"
check "path traversal on script read"        400 "$(status "${AUTH[@]}" "$BASE/api/lua/scripts/file?name=../../../../etc/passwd")"
check "traversal with encoding"              "400|404" "$(status "${AUTH[@]}" "$BASE/api/lua/scripts/file?name=..%2f..%2fetc%2fpasswd")"
check "absolute path rejected"               400 "$(status "${AUTH[@]}" "$BASE/api/lua/scripts/file?name=/etc/passwd")"
check "non-.lua rejected"                    400 "$(status "${AUTH[@]}" "$BASE/api/lua/scripts/file?name=passwd")"

echo
echo "== Security headers =="
HEADERS="$(curl -s -D - -o /dev/null --max-time 5 "$BASE/api/auth/mode")"
for header in "Content-Security-Policy" "X-Content-Type-Options" "X-Frame-Options" "Referrer-Policy"; do
  if grep -qi "^$header:" <<<"$HEADERS"; then
    printf '  \033[32mok\033[0m   %-58s present\n' "$header"; PASSED=$((PASSED + 1))
  else
    printf '  \033[31mFAIL\033[0m %-58s missing\n' "$header"; FAILED=$((FAILED + 1))
  fi
done
if grep -qi "^Access-Control-Allow-Origin: \*" <<<"$HEADERS"; then
  printf '  \033[31mFAIL\033[0m %-58s wildcard CORS\n' "CORS not wildcarded"; FAILED=$((FAILED + 1))
else
  printf '  \033[32mok\033[0m   %-58s no wildcard\n' "CORS not wildcarded"; PASSED=$((PASSED + 1))
fi

echo
echo "== Brute-force lockout =="
# A username of its own, so the admin account stays usable for the rest of the
# run -- the lockout is per-account and lasts 15 minutes.
curl -s -o /dev/null --max-time 10 -X POST "$BASE/api/users" "${AUTH[@]}" -H 'Content-Type: application/json' \
  -d '{"username":"locktest","password":"locktest-password-1","role":"USER"}'
LOCK_CODES=""
for i in $(seq 1 6); do
  LOCK_CODES="$LOCK_CODES $(status -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' -d '{"username":"locktest","password":"wrong"}')"
done
echo "  attempt codes:$LOCK_CODES"
LAST_CODE="${LOCK_CODES##* }"
check "6th attempt is locked out"            423 "$LAST_CODE"
check "correct password still refused"       423 "$(status -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' -d '{"username":"locktest","password":"locktest-password-1"}')"
check "a different account is unaffected"    401 "$(status -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' -d '{"username":"someoneelse","password":"x"}')"

#
# Audit BEFORE the rate-limit flood. Each unauthenticated request writes an
# ACCESS_DENIED row, so a few hundred of them push every earlier event out of
# any window this test could read. Querying per-event with ?event= is also
# immune to volume, which is why it is done that way rather than by grepping
# one big page.
echo
echo "== Audit trail =="
for event in LOGIN_SUCCESS LOGIN_FAILED ACCOUNT_LOCKED ACCESS_DENIED USER_CREATED; do
  COUNT="$(curl -s --max-time 5 "${AUTH[@]}" "$BASE/api/audit?limit=1&event=$event" |
           sed -n 's/.*"total":\([0-9]*\).*/\1/p')"
  if [[ "${COUNT:-0}" -gt 0 ]]; then
    printf '  \033[32mok\033[0m   %-58s %s recorded\n' "$event" "$COUNT"; PASSED=$((PASSED + 1))
  else
    printf '  \033[31mFAIL\033[0m %-58s not recorded\n' "$event"; FAILED=$((FAILED + 1))
  fi
done

# Secrets must not reach either log. Tested with a password this script sent
# over the API in a request body -- not the seeded one, which the gateway
# prints to the console once by design and which would make this check fail for
# the wrong reason.
AUDIT_ALL="$(curl -s --max-time 5 "${AUTH[@]}" "$BASE/api/audit?limit=500")"
SENT_PASSWORD="locktest-password-1"
if grep -q "$SENT_PASSWORD" "$WORK/gateway.log" || grep -q "$SENT_PASSWORD" <<<"$AUDIT_ALL"; then
  printf '  \033[31mFAIL\033[0m %-58s a submitted password was logged\n' "no passwords logged"; FAILED=$((FAILED + 1))
else
  printf '  \033[32mok\033[0m   %-58s absent from both logs\n' "no passwords logged"; PASSED=$((PASSED + 1))
fi
if grep -q "$ADMIN_TOKEN" "$WORK/gateway.log" || grep -q "$ADMIN_TOKEN" <<<"$AUDIT_ALL"; then
  printf '  \033[31mFAIL\033[0m %-58s a bearer token was logged\n' "no tokens logged"; FAILED=$((FAILED + 1))
else
  printf '  \033[32mok\033[0m   %-58s absent from both logs\n' "no tokens logged"; PASSED=$((PASSED + 1))
fi

#
# Rate limiting last, and with the limits tightened over the API first -- the
# run so far needed them out of the way (see the config note at the top).
echo
echo "== Rate limiting =="
curl -s -o /dev/null --max-time 5 -X POST "${AUTH[@]}" -H 'Content-Type: application/json' \
  -d '{"auth":{"api_rate_per_sec":2,"api_burst":5,"login_rate_per_sec":0.2,"login_burst":3}}' \
  "$BASE/api/config"

SAW_429=0
for i in $(seq 1 40); do
  [[ "$(status "${AUTH[@]}" "$BASE/api/status")" == "429" ]] && { SAW_429=1; break; }
done
check "API traffic is rate limited"          1 "$SAW_429"

SAW_LOGIN_429=0
for i in $(seq 1 20); do
  [[ "$(status -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' -d '{"username":"nobody","password":"x"}')" == "429" ]] && { SAW_LOGIN_429=1; break; }
done
check "login endpoint is rate limited"       1 "$SAW_LOGIN_429"

echo
echo "============================================================"
echo "  passed: $PASSED   failed: $FAILED"
echo "============================================================"
[[ $FAILED -eq 0 ]]
