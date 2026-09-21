#!/bin/sh
#
# AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# The end-to-end harness. Brings up one node, provisions a realm and three accounts
# over the admin API, and runs each sipp scenario against it.
#
#   test/e2e/run.sh              every scenario
#   test/e2e/run.sh register     only the ones whose name contains "register"
#
# Principle 2 says compliance is proven rather than asserted, and this is where that
# happens: the unit tests say the code does what the RFC says, and this says a real
# client on a real socket agrees.

set -eu

cd "$(dirname "$0")/.."

COMPOSE="docker compose -f docker-compose.test.yml"
NODE="172.31.0.10"
API="http://${NODE}:8080/api/v1"
ADMIN_TOKEN="e2e-admin"
FILTER="${1:-}"

RESULTS="test/e2e/results"
mkdir -p "${RESULTS}"
rm -f "${RESULTS}"/*.log "${RESULTS}"/*.err 2>/dev/null || true

passed=0
failed=0
failures=""

cleanup() {
  ${COMPOSE} --profile e2e down --remove-orphans >/dev/null 2>&1 || true
}
trap cleanup EXIT

# curl from inside the network, because the node is not published to the host: the
# harness tests what a client on the same network sees.
api() {
  ${COMPOSE} run --rm --no-deps --entrypoint curl sipp-uac \
    -fsS -H "Authorization: Bearer ${ADMIN_TOKEN}" -H "Content-Type: application/json" "$@"
}

echo "Building and starting the node..."
${COMPOSE} up -d --build athenasip

echo "Waiting for it to serve..."
attempt=0
until ${COMPOSE} run --rm --no-deps --entrypoint curl sipp-uac -fsS "${API}/health" >/dev/null 2>&1; do
  attempt=$((attempt + 1))
  if [ "${attempt}" -ge 60 ]; then
    echo "The node never became healthy."
    ${COMPOSE} logs athenasip
    exit 1
  fi
  sleep 1
done

echo "Provisioning..."
api -X POST "${API}/realms" -d '{"name":"example.com"}' >/dev/null
api -X POST "${API}/realms/example.com/accounts" -d '{"user":"alice","password":"alice-secret"}' >/dev/null
api -X POST "${API}/realms/example.com/accounts" -d '{"user":"bob","password":"bob-secret"}' >/dev/null
api -X POST "${API}/realms/example.com/accounts" -d '{"user":"carol","password":"carol-secret"}' >/dev/null

# One end only: a scenario that registers and asserts on what came back.
run_one() {
  name="$1"
  scenario="$2"
  csv="$3"
  timeout="$4"

  case "${name}" in
    *${FILTER}*) ;;
    *) return 0 ;;
  esac

  echo "  ${name}"

  if ${COMPOSE} run --rm sipp-uac \
      -sf "/e2e/scenarios/${scenario}" -inf "/e2e/${csv}" \
      -m 1 -r 1 -timeout "${timeout}" -timeout_error \
      -trace_err -error_file "/results/${name}.err" \
      -nostdin "${NODE}:5060" >"${RESULTS}/${name}.log" 2>&1; then
    passed=$((passed + 1))
  else
    failed=$((failed + 1))
    failures="${failures} ${name}"
    echo "    failed - see ${RESULTS}/${name}.log"
  fi
}

# Two ends: the callee registers and waits, the caller calls it.
run_pair() {
  name="$1"
  uas_scenario="$2"
  uas_csv="$3"
  uac_scenario="$4"
  uac_csv="$5"
  timeout="$6"

  case "${name}" in
    *${FILTER}*) ;;
    *) return 0 ;;
  esac

  echo "  ${name}"

  ${COMPOSE} run -d --name "e2e-uas-${name}" sipp-uas \
    -sf "/e2e/scenarios/${uas_scenario}" -inf "/e2e/${uas_csv}" \
    -p 6000 -m 1 -r 1 -timeout "${timeout}" -timeout_error \
    -trace_err -error_file "/results/${name}-uas.err" \
    -nostdin "${NODE}:5060" >/dev/null 2>&1

  # The callee has to be registered before the caller dials it, and a registration is
  # two round trips through Digest.
  sleep 2

  if ${COMPOSE} run --rm sipp-uac \
      -sf "/e2e/scenarios/${uac_scenario}" -inf "/e2e/${uac_csv}" \
      -p 6001 -m 1 -r 1 -timeout "${timeout}" -timeout_error \
      -trace_err -error_file "/results/${name}.err" \
      -nostdin "${NODE}:5060" >"${RESULTS}/${name}.log" 2>&1; then
    passed=$((passed + 1))
  else
    failed=$((failed + 1))
    failures="${failures} ${name}"
    echo "    failed - see ${RESULTS}/${name}.log"
  fi

  docker logs "e2e-uas-${name}" >"${RESULTS}/${name}-uas.log" 2>&1 || true
  docker rm -f "e2e-uas-${name}" >/dev/null 2>&1 || true
}

echo "Running scenarios..."

run_one register              register.xml              alice.csv 20s
run_one register-wrong-password register_unauthorised.xml alice.csv 20s
run_one register-retransmit   register_retransmit.xml   alice.csv 20s

run_pair invite-bye     uas.xml         bob.csv   invite_bye.xml        alice.csv 30s
run_pair cancel-ringing uas_ringing.xml bob.csv   cancel_after_180.xml  alice.csv 30s
run_pair busy           uas_busy.xml    bob.csv   invite_busy.xml       alice.csv 30s
run_pair media          uas_media.xml   bob.csv   invite_media.xml      alice.csv 40s

# Last, because timer B is 64*T1 and this one waits it out.
run_pair invite-timeout uas_silent.xml  carol.csv invite_timeout.xml    alice-to-carol.csv 60s

echo
echo "${passed} passed, ${failed} failed"
[ "${failed}" -eq 0 ] || { echo "failed:${failures}"; exit 1; }
