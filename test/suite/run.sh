#!/usr/bin/env bash
#
# AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# Every project's tests, in concert: this node's own, then the admin console's and
# AthenaPhone's against a live node, once with direct media and once relayed through coturn.
#
#   test/suite/run.sh              everything that runs unattended
#   test/suite/run.sh --no-sipp    without the sipp harnesses, which take longest
#   test/suite/run.sh --device     also what needs the A85 and a person (ATHENA_SUITE_DEVICE=1)
#
# A sibling project takes part by providing `npm run test:athenasip`, which reads the
# fixture from ATHENA_INTEROP_* (test/interop/generated/fixture.env), never starts or
# stops a container except by $ATHENA_SUITE_RESTART_CMD, and writes
# $ATHENA_SUITE_RESULTS/<project>/summary.json. One that does not provide it is reported,
# not failed.

set -u

cd "$(dirname "$0")/../.."
ROOT="$(pwd)"

SIPP="yes"
export ATHENA_SUITE_DEVICE="${ATHENA_SUITE_DEVICE:-0}"
for argument in "$@"; do
  case "$argument" in
    --no-sipp) SIPP="no" ;;
    --device) ATHENA_SUITE_DEVICE=1 ;;
    *) echo "Unknown argument: $argument" >&2; exit 1 ;;
  esac
done

ADMIN_REPO="${ATHENA_SUITE_ADMIN_REPO:-$ROOT/../athenasip-admin}"
PHONE_REPO="${ATHENA_SUITE_PHONE_REPO:-$ROOT/../athenaphone}"
RESULTS="$ROOT/test/suite/results/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RESULTS"

declare -a NAMES=() OUTCOMES=()
record() { NAMES+=("$1"); OUTCOMES+=("$2"); echo "  -> $1: $2"; }

# Runs a step, keeping its output in the results; records passed or failed.
step() {
  local name="$1"
  shift
  echo
  echo "=== ${name} ==="
  local log="$RESULTS/$(echo "$name" | tr ' /' '--').log"
  if "$@" >"$log" 2>&1; then record "$name" passed; else record "$name" "failed (see ${log#$ROOT/})"; fi
}

# A sibling project's suite against the fixture now up, if it has one.
project() {
  local name="$1" repo="$2" phase="$3"
  if [[ ! -f "$repo/package.json" ]] || ! grep -q '"test:athenasip"' "$repo/package.json"; then
    record "$name ($phase)" "not provided (no npm run test:athenasip in ${repo})"
    return
  fi
  export ATHENA_SUITE_RESULTS="$RESULTS/$phase"
  mkdir -p "$ATHENA_SUITE_RESULTS"
  step "$name ($phase)" bash -c "cd '$repo' && npm run test:athenasip"
}

# --- This node's own -----------------------------------------------------------------

unit_tests() {
  cmake --preset tests >/dev/null && cmake --build build-tests -j8 && ./build-tests/athenasip_tests
}

# The Redis and MQTT tests skip without a server, so give them one where there is.
if [[ -z "${ATHENA_TEST_REDIS_URL:-}" ]] && command -v redis-server >/dev/null; then
  redis-cli -p 6399 ping >/dev/null 2>&1 || redis-server --port 6399 --save '' --daemonize yes >/dev/null
  export ATHENA_TEST_REDIS_URL=redis://127.0.0.1:6399
fi
if [[ -z "${ATHENA_TEST_MQTT_URL:-}" ]] && nc -z 127.0.0.1 1883 2>/dev/null; then
  export ATHENA_TEST_MQTT_URL=mqtt://127.0.0.1:1883
fi

step "server unit tests" unit_tests

if [[ "$SIPP" == "yes" ]]; then
  step "sipp, one node" test/e2e/run.sh
  step "sipp, one node with rtpengine" test/e2e/run.sh --rtpengine
  step "sipp, two nodes" test/e2e/cluster.sh
fi

# --- Against a live node ---------------------------------------------------------------

export ATHENA_INTEROP_ADMIN_REPO="$ADMIN_REPO"
[[ -f "$ADMIN_REPO/build/softphone.html" ]] && export ATHENA_INTEROP_ADMIN_DIR="$ADMIN_REPO/build"

for phase in direct relay; do
  echo
  echo "=== The fixture, ${phase} media ==="
  advertise=""
  [[ "$phase" == "relay" ]] && advertise="${ATHENA_INTEROP_RTPENGINE_ADDRESS:-172.32.0.30}"

  admin_flag=""
  [[ -n "${ATHENA_INTEROP_ADMIN_DIR:-}" ]] && admin_flag="--admin"

  if ! ATHENA_INTEROP_RTPENGINE_ADVERTISE="$advertise" test/interop/up.sh --rtpengine $admin_flag >"$RESULTS/fixture-${phase}.log" 2>&1; then
    record "fixture (${phase})" "failed to come up (see ${RESULTS#$ROOT/}/fixture-${phase}.log)"
    test/interop/up.sh down >/dev/null 2>&1
    continue
  fi

  set -a
  source test/interop/generated/fixture.env
  set +a
  export ATHENA_SUITE_PHASE="$phase"

  # A project may restart the node mid-suite, to test registering again; only through this.
  export ATHENA_SUITE_RESTART_CMD="docker restart ${ATHENA_INTEROP_NAME:-athenasip-interop}"

  step "smoke, every transport (${phase})" test/interop/smoke.py --host "$ATHENA_INTEROP_PUBLIC_ADDRESS" --realm "$ATHENA_INTEROP_REALM" --user 1001 \
    --password "$ATHENA_INTEROP_PASSWORD" --sip-port "$ATHENA_INTEROP_SIP_PORT" --tls-port "$ATHENA_INTEROP_TLS_PORT" --ws-port "$ATHENA_INTEROP_WS_PORT" \
    --ca tls/ca/snakeca.crt

  project "admin console" "$ADMIN_REPO" "$phase"
  project "AthenaPhone" "$PHONE_REPO" "$phase"

  test/interop/up.sh down >/dev/null 2>&1
done

# --- Summary -----------------------------------------------------------------------------

echo
echo "=== Summary (results in ${RESULTS#$ROOT/}) ==="
failed=0
for i in "${!NAMES[@]}"; do
  printf '  %-45s %s\n' "${NAMES[$i]}" "${OUTCOMES[$i]}"
  [[ "${OUTCOMES[$i]}" == failed* ]] && failed=$((failed + 1))
done
for summary in "$RESULTS"/*/*/summary.json; do
  [[ -f "$summary" ]] && echo "  ${summary#$RESULTS/}: $(cat "$summary")"
done

exit $((failed > 0))
