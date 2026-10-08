#!/bin/sh
#
# AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# The trunk harness. One node on the standard trunk script (athenasip.trunks), sipp as a
# carrier at 172.31.0.20 and the subscribers at 172.31.0.21. The node registers to the
# carrier and answers its challenge, calls out through it with the trunk's caller ID, tries
# a second trunk when the first fails, and is called in through it.
#
#   test/e2e/trunk.sh            every scenario
#   test/e2e/trunk.sh failover   only those whose name contains "failover"

set -eu

cd "$(dirname "$0")/../.."

. test/e2e/lua.sh
lua_begin
lua_node athenasip test/e2e/config
mkdir -p "${LUA_DIR}/athenasip/scripts"
printf 'authorize, route, on_failure, register = require("athenasip.trunks").hooks()\n' >"${LUA_DIR}/athenasip/scripts/main.lua"
printf '  lua:\n    path: [/root/.athenasip/scripts]\n' >>"${LUA_DIR}/athenasip/config.yaml"

COMPOSE="docker compose -f docker-compose.test.yml -f ${LUA_DIR}/compose.yml"
NODE="172.31.0.10"
API="http://${NODE}:8080/api/v1"
ADMIN_TOKEN=""
FILTER="${1:-}"

RESULTS="test/e2e/results"
mkdir -p "${RESULTS}"
rm -f "${RESULTS}"/trunk-*.log "${RESULTS}"/trunk-*.err 2>/dev/null || true

passed=0
failed=0
failures=""

cleanup() {
  docker logs athenasip-e2e >"${RESULTS}/trunk-node.log" 2>&1 || true
  for name in e2e-carrier e2e-callee; do docker rm -f "${name}" >/dev/null 2>&1 || true; done
  ${COMPOSE} --profile e2e down --remove-orphans >/dev/null 2>&1 || true
}
trap cleanup EXIT

curl_in() { ${COMPOSE} run --rm --no-deps --entrypoint curl sipp-uac -fsS "$@"; }
api() { curl_in -H "Authorization: Bearer ${ADMIN_TOKEN}" -H "Content-Type: application/json" "$@"; }

pass() {
  passed=$((passed + 1))
}

fail() {
  failed=$((failed + 1))
  failures="${failures} $1"
  echo "    failed - $2"
}

wanted() {
  case "$1" in
    *${FILTER}*) echo "  $1" ;;
    *) return 1 ;;
  esac
}

# sipp on one side, detached, so the other side can run against it.
#   start_side <container name> <service> <scenario> <port> [sipp arguments...]
start_side() {
  container="$1"
  service="$2"
  scenario="$3"
  port="$4"
  shift 4
  docker rm -f "${container}" >/dev/null 2>&1 || true
  ${COMPOSE} run -d --name "${container}" "${service}" \
    -sf "/e2e/scenarios/${scenario}" -p "${port}" -m 1 -r 1 -timeout 60s -timeout_error \
    -cid_str "${container}-%u-%p@%s" -trace_err -error_file "/results/${container}-${scenario%.xml}.err" "$@" -nostdin "${NODE}:5060" >/dev/null 2>&1
  sleep 2
}

# Whether a detached side finished its scenario, with its log kept.
side_passed() {
  status=$(docker wait "$1" 2>/dev/null || echo 1)
  docker logs "$1" >"${RESULTS}/trunk-$2.log" 2>&1 || true
  docker rm -f "$1" >/dev/null 2>&1 || true
  [ "${status}" = "0" ]
}

# A subscriber's call from 172.31.0.21.
caller() {
  ${COMPOSE} run --rm sipp-uac \
    -sf "/e2e/scenarios/$1" -inf "/e2e/$2" -au "$3" -ap "$3-secret" \
    -p "$4" -cid_str "trunk-$5-%u-%p@%s" -m 1 -r 1 -timeout 30s -timeout_error \
    -trace_err -error_file "/results/trunk-$5.err" -nostdin "${NODE}:5060" >"${RESULTS}/trunk-$5.log" 2>&1
}

echo "Building and starting the node, policy: lua, athenasip.trunks..."
${COMPOSE} up -d --build athenasip

attempt=0
until curl_in "${API}/health" >/dev/null 2>&1; do
  attempt=$((attempt + 1))
  if [ "${attempt}" -ge 60 ]; then
    echo "The node never became healthy."
    ${COMPOSE} logs athenasip
    exit 1
  fi
  sleep 1
done

ADMIN_TOKEN=$(curl_in -H "Content-Type: application/json" -d '{"username":"e2e-admin","password":"e2e-admin-password"}' "${API}/auth/login" |
  sed -n 's/.*"token":"\([0-9a-f]*\)".*/\1/p')
[ -n "${ADMIN_TOKEN}" ] || { echo "Could not sign in."; exit 1; }

echo "Provisioning..."
api -X POST "${API}/realms" -d '{"name":"example.com"}' >/dev/null
api -X POST "${API}/realms/example.com/subscribers" -d '{"user":"alice","password":"alice-secret"}' >/dev/null
api -X POST "${API}/realms/example.com/subscribers" -d '{"user":"bob","password":"bob-secret"}' >/dev/null

echo "Running scenarios..."

# The carrier's registrar is listening before the trunk exists, so the first REGISTER finds it. Always run: the
# other scenarios need the trunks, and a registration the carrier never granted would be retried into them.
echo "  trunk-register"
if true; then
  start_side e2e-carrier sipp-uas carrier_register.xml 5060
  api -X POST "${API}/trunks" -d '{
    "name": "acme", "uri": "sip:172.31.0.20:5060", "username": "4420", "password": "carrier-secret",
    "register": {"enabled": true, "expires": 3600}, "inbound_addresses": ["172.31.0.20/32"],
    "attributes": {"prefixes": ["+44"], "country": "44", "priority": 10, "caller_id": "+442071234567",
                   "numbers": {"+442071234567": "sip:bob@example.com"}}}' >/dev/null
  api -X POST "${API}/trunks" -d '{
    "name": "backup", "uri": "sip:172.31.0.20:5060",
    "attributes": {"prefixes": ["+44"], "country": "44", "priority": 20, "dial_format": "digits"}}' >/dev/null

  if side_passed e2e-carrier register; then
    state=""
    for _ in 1 2 3 4 5 6 7 8 9 10; do
      state=$(api "${API}/trunks/acme" | sed -n 's/.*"state":"\([a-z]*\)".*/\1/p')
      [ "${state}" = "registered" ] && break
      sleep 1
    done
    if [ "${state}" = "registered" ]; then pass; else fail trunk-register "the API says the trunk is '${state}'"; fi
  else
    fail trunk-register "see ${RESULTS}/trunk-register.log"
  fi
fi

# Out: Alice dials a national number; it leaves by acme, which challenges.
if wanted trunk-out; then
  start_side e2e-carrier sipp-uas carrier_call.xml 5060
  if caller invite_bye.xml alice-to-number.csv alice 5102 out; then out_ok=yes; else out_ok=no; fi
  if side_passed e2e-carrier out-carrier && [ "${out_ok}" = yes ]; then pass; else fail trunk-out "see ${RESULTS}/trunk-out.log and trunk-out-carrier.log"; fi
fi

# Failover: acme answers 503, backup carries the call.
if wanted trunk-failover; then
  start_side e2e-carrier sipp-uas carrier_failover.xml 5060
  if caller invite_bye.xml alice-to-number.csv alice 5104 failover; then failover_ok=yes; else failover_ok=no; fi
  if side_passed e2e-carrier failover-carrier && [ "${failover_ok}" = yes ]; then
    pass
  else
    fail trunk-failover "see ${RESULTS}/trunk-failover.log and trunk-failover-carrier.log"
  fi
fi

# In: the carrier calls the number that names Bob.
if wanted trunk-in; then
  ${COMPOSE} run --rm sipp-uac -sf /e2e/scenarios/register.xml -inf /e2e/bob.csv -au bob -ap bob-secret \
    -p 5106 -m 1 -r 1 -timeout 20s -timeout_error -nostdin "${NODE}:5060" >"${RESULTS}/trunk-in-register.log" 2>&1 || true
  docker rm -f e2e-callee >/dev/null 2>&1 || true
  ${COMPOSE} run -d --name e2e-callee sipp-uac -sf /e2e/scenarios/uas.xml -inf /e2e/bob.csv -au bob -ap bob-secret \
    -p 5106 -m 1 -r 1 -timeout 30s -timeout_error -nostdin "${NODE}:5060" >/dev/null 2>&1
  sleep 2

  if ${COMPOSE} run --rm sipp-uas -sf /e2e/scenarios/carrier_inbound.xml -p 5060 -m 1 -r 1 -timeout 30s -timeout_error \
      -trace_err -error_file /results/trunk-in.err -nostdin "${NODE}:5060" >"${RESULTS}/trunk-in.log" 2>&1 && side_passed e2e-callee in-callee; then
    pass
  else
    side_passed e2e-callee in-callee || true
    fail trunk-in "see ${RESULTS}/trunk-in.log and trunk-in-callee.log"
  fi
fi

echo
echo "${passed} passed, ${failed} failed"
[ "${failed}" -eq 0 ] || { echo "Failed:${failures}"; exit 1; }
