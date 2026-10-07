#!/bin/sh
#
# AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# The end-to-end harness. Brings up one node, provisions a realm and three subscribers
# over the admin API, and runs each sipp scenario against it.
#
#   test/e2e/run.sh              every scenario
#   test/e2e/run.sh register     only the ones whose name contains "register"
#   test/e2e/run.sh --rtpengine  the same, with rtpengine on the media path

set -eu

cd "$(dirname "$0")/../.."

COMPOSE="docker compose -f docker-compose.test.yml"
ENGINE="builtin"

# The media engine is a compose overlay: the scenarios, provisioning and node are the
# same, so a scenario that passes on one engine and fails on the other is an engine fault.
if [ "${1:-}" = "--rtpengine" ]; then
  COMPOSE="${COMPOSE} -f docker-compose.rtpengine.yml"
  ENGINE="rtpengine"
  shift
fi

NODE="172.31.0.10"
API="http://${NODE}:8080/api/v1"
# The administrator the node creates as it starts (docker-compose.test.yml).
ADMIN_USER="e2e-admin"
ADMIN_PASSWORD="e2e-admin-password"
ADMIN_TOKEN=""
FILTER="${1:-}"

RESULTS="test/e2e/results"
mkdir -p "${RESULTS}"
rm -f "${RESULTS}"/*.log "${RESULTS}"/*.err 2>/dev/null || true

passed=0
failed=0
failures=""

# Every sipp run gets its own port. sipp derives its branch from the pid and call number,
# so two runs from one port send the same branch and sent-by, which the node rightly
# treats as a retransmission (RFC 3261 17.2.3).
next_port=5100

# Not a function that echoes: $(...) is a subshell, and the counter would not advance.
allocate_port() { next_port=$((next_port + 2)); }

cleanup() {
  # The node's log, saved before the containers go.
  docker logs athenasip-e2e >"${RESULTS}/node.log" 2>&1 || true

  # And the engine's: its counters are the only evidence that it anchored a call.
  if [ "${ENGINE}" = "rtpengine" ]; then
    docker logs athenasip-e2e-rtpengine >"${RESULTS}/rtpengine.log" 2>&1 || true
    docker logs athenasip-e2e-rtpengine-b >"${RESULTS}/rtpengine-b.log" 2>&1 || true
  fi

  ${COMPOSE} --profile e2e down --remove-orphans >/dev/null 2>&1 || true
}
trap cleanup EXIT

# curl from inside the network: the node is not published to the host.
api() {
  ${COMPOSE} run --rm --no-deps --entrypoint curl sipp-uac \
    -fsS -H "Authorization: Bearer ${ADMIN_TOKEN}" -H "Content-Type: application/json" "$@"
}

echo "Building and starting the node..."
echo "Media engine: ${ENGINE}"
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

echo "Signing in..."
ADMIN_TOKEN=$(${COMPOSE} run --rm --no-deps --entrypoint curl sipp-uac -fsS -H "Content-Type: application/json" \
  -d "{\"username\":\"${ADMIN_USER}\",\"password\":\"${ADMIN_PASSWORD}\"}" "${API}/auth/login" |
  sed -n 's/.*"token":"\([0-9a-f]*\)".*/\1/p')

if [ -z "${ADMIN_TOKEN}" ]; then
  echo "Could not sign in as ${ADMIN_USER}."
  exit 1
fi

echo "Provisioning..."
api -X POST "${API}/realms" -d '{"name":"example.com"}' >/dev/null
api -X POST "${API}/realms/example.com/subscribers" -d '{"user":"alice","password":"alice-secret"}' >/dev/null
api -X POST "${API}/realms/example.com/subscribers" -d '{"user":"bob","password":"bob-secret"}' >/dev/null
api -X POST "${API}/realms/example.com/subscribers" -d '{"user":"carol","password":"carol-secret"}' >/dev/null

# One end only: a scenario that registers and asserts on what came back.
#
# Credentials go on the command line: sipp expands a [field] before it reads the
# [authentication] keyword around it, so they cannot be nested in the scenario.
run_one() {
  name="$1"
  scenario="$2"
  csv="$3"
  user="$4"
  password="$5"
  timeout="$6"

  case "${name}" in
    *${FILTER}*) ;;
    *) return 0 ;;
  esac

  echo "  ${name}"

  allocate_port
  uac_port="${next_port}"

  if ${COMPOSE} run --rm sipp-uac \
      -sf "/e2e/scenarios/${scenario}" -inf "/e2e/${csv}" \
      -au "${user}" -ap "${password}" \
      -p "${uac_port}" -cid_str "${name}-%u-%p@%s" \
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
  uas_user="$4"
  uac_scenario="$5"
  uac_csv="$6"
  uac_user="$7"
  timeout="$8"

  case "${name}" in
    *${FILTER}*) ;;
    *) return 0 ;;
  esac

  echo "  ${name}"

  allocate_port
  uas_port="${next_port}"

  allocate_port
  uac_port="${next_port}"

  # A port of its own, so the deregistration is not matched to the UAS run's transaction
  # (RFC 3261 17.2.3).
  allocate_port
  dereg_port="${next_port}"

  # Clear the callee's bindings first with Contact: * and Expires: 0 (RFC 3261 10.2.2).
  # Each scenario registers from a new port, which adds a binding rather than replacing
  # one (10.3 step 7), and the node would fork to the dead ones.
  ${COMPOSE} run --rm sipp-uas \
    -sf "/e2e/scenarios/deregister.xml" -inf "/e2e/${uas_csv}" \
    -au "${uas_user}" -ap "${uas_user}-secret" \
    -p "${dereg_port}" -cid_str "${name}-uas-dereg-%u-%p@%s" \
    -m 1 -r 1 -timeout 20s -timeout_error \
    -nostdin "${NODE}:5060" >"${RESULTS}/${name}-uas-deregister.log" 2>&1 || true

  # The callee registers in a run of its own, on the port the UAS run then listens on.
  # sipp binds a scenario to one call, so a scenario that sent the REGISTER could not
  # accept the INVITE, which arrives with the caller's Call-ID.
  ${COMPOSE} run --rm sipp-uas \
    -sf "/e2e/scenarios/register.xml" -inf "/e2e/${uas_csv}" \
    -au "${uas_user}" -ap "${uas_user}-secret" \
    -p "${uas_port}" -cid_str "${name}-uas-reg-%u-%p@%s" \
    -m 1 -r 1 -timeout 20s -timeout_error \
    -nostdin "${NODE}:5060" >"${RESULTS}/${name}-uas-register.log" 2>&1 || {
      failed=$((failed + 1))
      failures="${failures} ${name}"
      echo "    failed to register the callee - see ${RESULTS}/${name}-uas-register.log"
      return 0
    }

  ${COMPOSE} run -d --name "e2e-uas-${name}" sipp-uas \
    -sf "/e2e/scenarios/${uas_scenario}" -inf "/e2e/${uas_csv}" \
    -au "${uas_user}" -ap "${uas_user}-secret" \
    -p "${uas_port}" -cid_str "${name}-uas-%u-%p@%s" \
    -m 1 -r 1 -timeout "${timeout}" -timeout_error \
    -trace_err -error_file "/results/${name}-uas.err" \
    -nostdin "${NODE}:5060" >/dev/null 2>&1

  # The UAS run has to have the port open before the caller dials.
  sleep 2

  if ${COMPOSE} run --rm sipp-uac \
      -sf "/e2e/scenarios/${uac_scenario}" -inf "/e2e/${uac_csv}" \
      -au "${uac_user}" -ap "${uac_user}-secret" \
      -p "${uac_port}" -cid_str "${name}-%u-%p@%s" \
      -m 1 -r 1 -timeout "${timeout}" -timeout_error \
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

# A media scenario passes whether or not the engine relayed the call, because declined
# media flows directly between the two sipp containers. Only the engine's counters tell:
# rtpengine's are in its log, the builtin relay's total is on /metrics.

# The builtin relay's running total, from /metrics. Empty when the node does not say.
relayed_total() {
  ${COMPOSE} run --rm --no-deps --entrypoint curl sipp-uac -fsS -H "Authorization: Bearer ${ADMIN_TOKEN}" "http://${NODE}:8080/metrics" 2>/dev/null |
    awk '/^athenasip_media_packets_relayed_total /{print $2}'
}

# What one rtpengine says it relayed, and rejected, over its life.
engine_stats() { docker logs "$1" 2>&1 | grep -E '^\[.*Port .*<>.*[0-9]+ p,' || true; }
engine_relayed() { engine_stats "$1" | awk -F'SSRC [^,]*, ' '{print $2}' | awk -F' p,' '{s+=$1} END {print s+0}'; }
engine_errors() { engine_stats "$1" | awk -F'b, ' '{print $2}' | awk -F' e,' '{s+=$1} END {print s+0}'; }

assert_media_relayed() {
  case "media" in
    *${FILTER}*) ;;
    *) return 0 ;;
  esac

  # The builtin relay's packet count is on /metrics.
  if [ "${ENGINE}" = "builtin" ]; then
    after=$(relayed_total)
    relayed=$(( ${after:-0} - ${relayed_before:-0} ))

    echo "  media-relayed (${relayed} packets through the builtin relay)"

    if [ -z "${after}" ] || [ "${relayed}" -le 0 ]; then
      failed=$((failed + 1))
      failures="${failures} media-relayed"
      echo "    failed - the builtin relay sent nothing on, so the call did not go through it"
      return 0
    fi

    passed=$((passed + 1))
    return 0
  fi

  # Either engine of the pool may have taken the call.
  relayed=$(( $(engine_relayed athenasip-e2e-rtpengine) + $(engine_relayed athenasip-e2e-rtpengine-b) ))
  errors=$(( $(engine_errors athenasip-e2e-rtpengine) + $(engine_errors athenasip-e2e-rtpengine-b) ))

  echo "  media-relayed (${relayed} packets, ${errors} errors)"

  if [ "${relayed}" -eq 0 ]; then
    failed=$((failed + 1))
    failures="${failures} media-relayed"
    echo "    failed - rtpengine relayed nothing, so the call did not go through it"
    return 0
  fi

  if [ "${errors}" -ne 0 ]; then
    failed=$((failed + 1))
    failures="${failures} media-relayed"
    echo "    failed - rtpengine rejected ${errors} packet(s); the offer and what was sent disagree"
    return 0
  fi

  passed=$((passed + 1))
}

echo "Running scenarios..."

run_one register                register.xml              alice.csv alice alice-secret       20s
run_one register-wrong-password register_unauthorised.xml alice.csv alice not-the-password  20s
run_one register-retransmit     register_retransmit.xml   alice.csv alice alice-secret      20s
run_one relay-refused           invite_relay_refused.xml  alice.csv alice alice-secret      20s

run_pair invite-bye     uas.xml         bob.csv   bob   invite_bye.xml       alice.csv          alice 30s
run_pair cancel-ringing uas_ringing.xml bob.csv   bob   cancel_after_180.xml alice.csv          alice 30s
run_pair busy           uas_busy.xml    bob.csv   bob   invite_busy.xml      alice.csv          alice 30s
relayed_before=$(relayed_total)
run_pair media          uas_media.xml   bob.csv   bob   invite_media.xml     alice.csv          alice 40s
assert_media_relayed

# The node ends a call over the admin API: both ends must be sent a BYE. Asked until the
# call is answered; a ringing call is a 409. From inside the node's container, because a
# sipp-uac container for curl would take the caller's address.
node_api() {
  docker exec athenasip-e2e curl -fsS -H "Authorization: Bearer ${ADMIN_TOKEN}" "$@"
}

hang_up_when_answered() {
  for _ in $(seq 1 30); do
    id=$(node_api "http://127.0.0.1:8080/api/v1/calls" 2>/dev/null | grep -o '"id":"hung-up-[^"]*"' | head -1 | sed 's/"id":"//; s/"$//')
    if [ -n "${id}" ] && node_api -X DELETE "http://127.0.0.1:8080/api/v1/calls/$(printf '%s' "${id}" | sed 's/@/%40/g')" >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  return 1
}

case "hung-up" in
  *${FILTER}*)
    hang_up_when_answered &
    hanger=$!
    run_pair hung-up uas.xml bob.csv bob invite_hung_up.xml alice.csv alice 40s
    wait "${hanger}" || echo "    the DELETE never succeeded"
    ;;
esac

run_pair delayed-offer  uas_delayed_offer.xml bob.csv bob invite_delayed_offer.xml alice.csv alice 30s
run_pair hold-resume    uas_hold.xml          bob.csv bob invite_hold.xml          alice.csv alice 30s

# The pool: with the first engine gone, a new call is anchored by the second, after one
# timeout at most.
if [ "${ENGINE}" = "rtpengine" ]; then
  case "engine-failover" in
    *${FILTER}*)
      docker stop athenasip-e2e-rtpengine >/dev/null
      before=$(engine_relayed athenasip-e2e-rtpengine-b)
      run_pair engine-failover uas_media.xml bob.csv bob invite_media.xml alice.csv alice 40s
      sleep 2
      after=$(engine_relayed athenasip-e2e-rtpengine-b)
      echo "  engine-failover-relayed ($((after - before)) packets through the second engine)"
      if [ "$((after - before))" -gt 0 ]; then
        passed=$((passed + 1))
      else
        failed=$((failed + 1))
        failures="${failures} engine-failover-relayed"
        echo "    failed - the second engine relayed nothing, so the call did not move to it"
      fi
      docker start athenasip-e2e-rtpengine >/dev/null
      ;;
  esac
fi

# Last, because timer B is 64*T1 and this one waits it out.
run_pair invite-timeout uas_silent.xml  carol.csv carol invite_timeout.xml   alice-to-carol.csv alice 60s

echo
echo "${passed} passed, ${failed} failed"
[ "${failed}" -eq 0 ] || { echo "failed:${failures}"; exit 1; }
