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
#   test/e2e/run.sh --rtpengine  the same, with rtpengine on the media path
#
# Principle 2 says compliance is proven rather than asserted, and this is where that
# happens: the unit tests say the code does what the RFC says, and this says a real
# client on a real socket agrees.

set -eu

cd "$(dirname "$0")/../.."

COMPOSE="docker compose -f docker-compose.test.yml"
ENGINE="builtin"

# The media engine is an overlay rather than a second harness: the scenarios, the
# provisioning and the node are the same, so a scenario that passes against one engine
# and fails against the other has found something in the engine.
if [ "${1:-}" = "--rtpengine" ]; then
  COMPOSE="${COMPOSE} -f docker-compose.rtpengine.yml"
  ENGINE="rtpengine"
  shift
fi

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

# Every scenario gets a port of its own. sipp runs as PID 1 in a container and derives
# its branch from the pid and the call number, so two scenarios in a row produce the
# same branch from the same address - which is a retransmission as far as RFC 3261
# 17.2.3 is concerned, and the node correctly replays its last answer instead of
# treating it as a new request. A different source port is a different sent-by, and the
# transactions stop colliding.
next_port=5100

# Not a function that echoes: $(...) is a subshell, and a counter incremented in one
# stays there. Every scenario would have come back with the same port, which is the
# collision this exists to avoid.
allocate_port() { next_port=$((next_port + 2)); }

cleanup() {
  # The node's own log, before the containers go. Without it a failing scenario is two
  # sipp traces and a guess about what the node in the middle made of them.
  docker logs athenasip-e2e >"${RESULTS}/node.log" 2>&1 || true

  # And the engine's, when there is one of its own. A media scenario passes whether the
  # engine anchored the call or declined it - a declined description travels on
  # untouched and the two ends reach each other directly - so the only thing that says
  # which happened is what the engine has to say for itself.
  if [ "${ENGINE}" = "rtpengine" ]; then
    docker logs athenasip-e2e-rtpengine >"${RESULTS}/rtpengine.log" 2>&1 || true
  fi

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

echo "Provisioning..."
api -X POST "${API}/realms" -d '{"name":"example.com"}' >/dev/null
api -X POST "${API}/realms/example.com/accounts" -d '{"user":"alice","password":"alice-secret"}' >/dev/null
api -X POST "${API}/realms/example.com/accounts" -d '{"user":"bob","password":"bob-secret"}' >/dev/null
api -X POST "${API}/realms/example.com/accounts" -d '{"user":"carol","password":"carol-secret"}' >/dev/null

# One end only: a scenario that registers and asserts on what came back.
#
# The credentials go on the command line rather than into the scenario. sipp expands a
# [field] before it reads the [authentication] keyword around it, so the nested form
# produces a header line that is not a header line at all.
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

  # A port of its own for the deregistration. It shares nothing with the UAS run but the
  # account, and sharing the port would make them the same transaction: sipp derives its
  # branch from the call number and message index, so two runs in a row send
  # z9hG4bK-1-1-0, and branch plus sent-by plus method is exactly what RFC 3261 17.2.3
  # matches on. The second REGISTER came back answered with the first one's response.
  # The star Contact removes every binding whatever address it was made from (10.2.2),
  # so this run has no reason to be on the UAS port.
  allocate_port
  dereg_port="${next_port}"

  # Every scenario gives its callee a port of its own, and a REGISTER from a new port is
  # a new binding rather than a replacement (RFC 3261 10.3 step 7). Left alone, the second
  # scenario to run has the node forking to the first one's port, which nothing is
  # listening on any more, and the call fails on a timeout that has nothing to do with
  # what was being tested. A Contact of "*" with Expires 0 clears the lot (10.2.2).
  ${COMPOSE} run --rm sipp-uas \
    -sf "/e2e/scenarios/deregister.xml" -inf "/e2e/${uas_csv}" \
    -au "${uas_user}" -ap "${uas_user}-secret" \
    -p "${dereg_port}" -cid_str "${name}-uas-dereg-%u-%p@%s" \
    -m 1 -r 1 -timeout 20s -timeout_error \
    -nostdin "${NODE}:5060" >"${RESULTS}/${name}-uas-deregister.log" 2>&1 || true

  # The callee registers in a run of its own, on the port the UAS run then listens on.
  # It cannot be part of the UAS scenario: sipp binds a scenario to a single call, so the
  # INVITE - which arrives with the caller's Call-ID - could not be mapped to the call
  # that sent the REGISTER, and sipp discarded it as unmappable. That is what kept every
  # two-ended scenario failing.
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

# A media scenario passes whether the engine relayed the call or declined it: a
# declined description travels on untouched and the two sipp containers reach each
# other directly, so the call completes and nothing says the engine did anything. The
# engine's own counters are the only thing that does.
#
# Only rtpengine keeps counters this harness can read. The builtin relay's bridging is
# proven by a unit test that sends real packets through it from two sockets.
assert_media_relayed() {
  [ "${ENGINE}" = "rtpengine" ] || return 0

  case "media" in
    *${FILTER}*) ;;
    *) return 0 ;;
  esac

  stats=$(docker logs athenasip-e2e-rtpengine 2>&1 | grep -E '^\[.*Port .*<>.*[0-9]+ p,' || true)

  relayed=$(echo "${stats}" | awk -F'SSRC [^,]*, ' '{print $2}' | awk -F' p,' '{s+=$1} END {print s+0}')
  errors=$(echo "${stats}" | awk -F'b, ' '{print $2}' | awk -F' e,' '{s+=$1} END {print s+0}')

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
run_pair media          uas_media.xml   bob.csv   bob   invite_media.xml     alice.csv          alice 40s
assert_media_relayed

run_pair delayed-offer  uas_delayed_offer.xml bob.csv bob invite_delayed_offer.xml alice.csv alice 30s
run_pair hold-resume    uas_hold.xml          bob.csv bob invite_hold.xml          alice.csv alice 30s

# Last, because timer B is 64*T1 and this one waits it out.
run_pair invite-timeout uas_silent.xml  carol.csv carol invite_timeout.xml   alice-to-carol.csv alice 60s

echo
echo "${passed} passed, ${failed} failed"
[ "${failed}" -eq 0 ] || { echo "failed:${failures}"; exit 1; }
