#!/usr/bin/env bash
#
# AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# The two-node harness. Brings up two nodes on one Redis and one broker, makes them a
# cluster with certificates from the node's own --ca-init and --ca-node, and calls across
# them with sipp.
#
#   test/e2e/cluster.sh            every scenario
#   test/e2e/cluster.sh across     only those whose name contains "across"
#
# It checks that a node reads a binding whose flow another node holds, forwards the call
# over mutual TLS, and that the ACK and BYE cross back. The first scenario is the control:
# both ends on one node of the cluster.

set -u

cd "$(dirname "$0")/../.."

COMPOSE="docker compose -f docker-compose.cluster.yml"
IMAGE="athenasip-e2e-cluster-node"

NODE_A="172.31.0.10"
NODE_B="172.31.0.11"
ADMIN_USER="e2e-admin"
ADMIN_PASSWORD="e2e-admin-password"
ADMIN_TOKEN=""
FILTER="${1:-}"

RESULTS="test/e2e/results"
CERTIFICATES="test/e2e/generated-cluster"
mkdir -p "${RESULTS}"
rm -f "${RESULTS}"/cluster-*.log "${RESULTS}"/cluster-*.err 2>/dev/null || true

passed=0
failed=0
failures=""

# Every sipp run gets its own port, as in run.sh: two runs from one port are one
# transaction.
next_port=5100
allocate_port() { next_port=$((next_port + 2)); }

cleanup() {
  docker logs athenasip-cluster-node-a >"${RESULTS}/cluster-node-a.log" 2>&1 || true
  docker logs athenasip-cluster-node-b >"${RESULTS}/cluster-node-b.log" 2>&1 || true
  ${COMPOSE} --profile e2e down --remove-orphans >/dev/null 2>&1 || true
}
trap cleanup EXIT

curl_in() { ${COMPOSE} run --rm --no-deps --entrypoint curl sipp-uac -fsS "$@"; }

api() { curl_in -H "Authorization: Bearer ${ADMIN_TOKEN}" -H "Content-Type: application/json" "$@"; }

echo "Building..."
${COMPOSE} --profile e2e build >"${RESULTS}/cluster-build.log" 2>&1 || {
  echo "The build failed - see ${RESULTS}/cluster-build.log"
  exit 1
}

# The cluster's authority and a certificate for each node (docs/certificates.md). Fresh
# every run: --ca-init refuses to overwrite an existing authority.
echo "Making the cluster's certificates..."
rm -rf "${CERTIFICATES}"
mkdir -p "${CERTIFICATES}"
ca() { docker run --rm -v "$(pwd)/${CERTIFICATES}:/cluster" "${IMAGE}" --ca-dir /cluster "$@"; }

ca --ca-init >"${RESULTS}/cluster-ca.log" 2>&1 &&
  ca --ca-node node-a --san "${NODE_A}" >>"${RESULTS}/cluster-ca.log" 2>&1 &&
  ca --ca-node node-b --san "${NODE_B}" >>"${RESULTS}/cluster-ca.log" 2>&1 || {
  echo "Could not make the certificates - see ${RESULTS}/cluster-ca.log"
  exit 1
}

echo "Starting Redis and the broker..."
${COMPOSE} up -d redis mosquitto >/dev/null 2>&1

# The first administrator. With redis://, --add-user writes the user and exits, and both
# nodes read it from the shared store.
echo "Making the administrator..."
${COMPOSE} run --rm --entrypoint /bin/sh node-a -c \
  "printf '%s\n' '${ADMIN_PASSWORD}' | /usr/local/bin/athenasip --add-user ${ADMIN_USER} --role manage-realms --role manage-realm-subscribers --role view-cluster-status" \
  >"${RESULTS}/cluster-add-user.log" 2>&1 || {
  echo "Could not make the administrator - see ${RESULTS}/cluster-add-user.log"
  exit 1
}

echo "Starting the nodes..."
${COMPOSE} up -d node-a node-b >/dev/null 2>&1

echo "Waiting for both to serve..."
for node in "${NODE_A}" "${NODE_B}"; do
  attempt=0
  until curl_in "http://${node}:8080/api/v1/health" >/dev/null 2>&1; do
    attempt=$((attempt + 1))
    if [ "${attempt}" -ge 60 ]; then
      echo "The node at ${node} never became healthy."
      exit 1
    fi
    sleep 1
  done
done

echo "Signing in..."
ADMIN_TOKEN=$(curl_in -H "Content-Type: application/json" \
  -d "{\"username\":\"${ADMIN_USER}\",\"password\":\"${ADMIN_PASSWORD}\"}" "http://${NODE_A}:8080/api/v1/auth/login" |
  sed -n 's/.*"token":"\([0-9a-f]*\)".*/\1/p')

if [ -z "${ADMIN_TOKEN}" ]; then
  echo "Could not sign in as ${ADMIN_USER}."
  exit 1
fi

# Each node must have learned where to reach the other before it can forward. A session
# made on node A is valid on node B: the store is shared.
echo "Waiting for the nodes to find each other..."
knows() { api "http://$1:8080/api/v1/nodes" 2>/dev/null | grep -q "\"id\":\"$2\".*\"cluster\""; }
attempt=0
until knows "${NODE_A}" node-b && knows "${NODE_B}" node-a; do
  attempt=$((attempt + 1))
  if [ "${attempt}" -ge 30 ]; then
    echo "The nodes never learned where to reach each other."
    api "http://${NODE_A}:8080/api/v1/nodes" || true
    echo
    api "http://${NODE_B}:8080/api/v1/nodes" || true
    echo
    exit 1
  fi
  sleep 1
done

echo "Provisioning, through node A..."
API="http://${NODE_A}:8080/api/v1"
api -X POST "${API}/realms" -d '{"name":"example.com"}' >/dev/null
api -X POST "${API}/realms/example.com/subscribers" -d '{"user":"alice","password":"alice-secret"}' >/dev/null
api -X POST "${API}/realms/example.com/subscribers" -d '{"user":"bob","password":"bob-secret"}' >/dev/null
api -X POST "${API}/realms/example.com/subscribers" -d '{"user":"carol","password":"carol-secret"}' >/dev/null

# The callee registers with one node and waits; the caller registers with, and calls
# through, another - or the same one, for the control.
run_pair() {
  name="$1"
  uas_node="$2"
  uas_scenario="$3"
  uac_node="$4"
  uac_scenario="$5"
  timeout="$6"
  uas_csv="${7:-bob.csv}"
  uas_user="${8:-bob}"
  uac_csv="${9:-alice.csv}"

  case "${name}" in
    *${FILTER}*) ;;
    *) return 0 ;;
  esac

  echo "  ${name}"

  allocate_port
  uas_port="${next_port}"
  allocate_port
  uac_port="${next_port}"
  allocate_port
  dereg_port="${next_port}"

  # Every binding of the callee's, from whichever node took it: the store is shared.
  ${COMPOSE} run --rm sipp-uas \
    -sf /e2e/scenarios/deregister.xml -inf "/e2e/${uas_csv}" -au "${uas_user}" -ap "${uas_user}-secret" \
    -p "${dereg_port}" -cid_str "${name}-uas-dereg-%u-%p@%s" \
    -m 1 -r 1 -timeout 20s -timeout_error \
    -nostdin "${uas_node}:5060" >"${RESULTS}/cluster-${name}-uas-deregister.log" 2>&1 || true

  ${COMPOSE} run --rm sipp-uas \
    -sf /e2e/scenarios/register.xml -inf "/e2e/${uas_csv}" -au "${uas_user}" -ap "${uas_user}-secret" \
    -p "${uas_port}" -cid_str "${name}-uas-reg-%u-%p@%s" \
    -m 1 -r 1 -timeout 20s -timeout_error \
    -nostdin "${uas_node}:5060" >"${RESULTS}/cluster-${name}-uas-register.log" 2>&1 || {
    failed=$((failed + 1))
    failures="${failures} ${name}"
    echo "    failed to register the callee - see ${RESULTS}/cluster-${name}-uas-register.log"
    return 0
  }

  ${COMPOSE} run -d --name "cluster-uas-${name}" sipp-uas \
    -sf "/e2e/scenarios/${uas_scenario}" -inf "/e2e/${uas_csv}" -au "${uas_user}" -ap "${uas_user}-secret" \
    -p "${uas_port}" -cid_str "${name}-uas-%u-%p@%s" \
    -m 1 -r 1 -timeout "${timeout}" -timeout_error \
    -trace_err -error_file "/results/cluster-${name}-uas.err" \
    -nostdin "${uas_node}:5060" >/dev/null 2>&1

  sleep 2

  if ${COMPOSE} run --rm sipp-uac \
    -sf "/e2e/scenarios/${uac_scenario}" -inf "/e2e/${uac_csv}" -au alice -ap alice-secret \
    -p "${uac_port}" -cid_str "${name}-%u-%p@%s" \
    -m 1 -r 1 -timeout "${timeout}" -timeout_error \
    -trace_err -error_file "/results/cluster-${name}.err" \
    -nostdin "${uac_node}:5060" >"${RESULTS}/cluster-${name}.log" 2>&1; then
    passed=$((passed + 1))
  else
    failed=$((failed + 1))
    failures="${failures} ${name}"
    echo "    failed - see ${RESULTS}/cluster-${name}.log"
  fi

  docker logs "cluster-uas-${name}" >"${RESULTS}/cluster-${name}-uas.log" 2>&1 || true
  docker rm -f "cluster-uas-${name}" >/dev/null 2>&1 || true
}

# The callee holds a connection to one node and is reachable only down it; the caller
# calls through another node, which must forward.
run_flow() {
  name="$1"
  transport="$2"
  callee_node="$3"
  caller_node="$4"

  case "${name}" in
    *${FILTER}*) ;;
    *) return 0 ;;
  esac

  echo "  ${name}"

  allocate_port
  uac_port="${next_port}"
  allocate_port
  dereg_port="${next_port}"

  ${COMPOSE} run --rm sipp-uas \
    -sf /e2e/scenarios/deregister.xml -inf /e2e/bob.csv -au bob -ap bob-secret \
    -p "${dereg_port}" -cid_str "${name}-uas-dereg-%u-%p@%s" \
    -m 1 -r 1 -timeout 20s -timeout_error \
    -nostdin "${callee_node}:5060" >"${RESULTS}/cluster-${name}-uas-deregister.log" 2>&1 || true

  docker rm -f "cluster-callee-${name}" >/dev/null 2>&1 || true
  ${COMPOSE} run -d --name "cluster-callee-${name}" flow-callee \
    --host "${callee_node}" --transport "${transport}" --user bob --password bob-secret >/dev/null 2>&1

  # Wait for the binding to be visible from the caller's node.
  attempt=0
  until api "http://${caller_node}:8080/api/v1/registrations" 2>/dev/null | grep -q "\"flow_id\":\"${transport}://172.31.0.22"; do
    attempt=$((attempt + 1))
    if [ "${attempt}" -ge 15 ]; then
      failed=$((failed + 1))
      failures="${failures} ${name}"
      echo "    failed - the callee never registered over ${transport}"
      docker logs "cluster-callee-${name}" >"${RESULTS}/cluster-${name}-callee.log" 2>&1 || true
      docker rm -f "cluster-callee-${name}" >/dev/null 2>&1 || true
      return 0
    fi
    sleep 1
  done

  caller_ok=0
  ${COMPOSE} run --rm sipp-uac \
    -sf /e2e/scenarios/invite_bye.xml -inf /e2e/alice.csv -au alice -ap alice-secret \
    -p "${uac_port}" -cid_str "${name}-%u-%p@%s" \
    -m 1 -r 1 -timeout 30s -timeout_error \
    -trace_err -error_file "/results/cluster-${name}.err" \
    -nostdin "${caller_node}:5060" >"${RESULTS}/cluster-${name}.log" 2>&1 || caller_ok=1

  # The callee exits on BYE, or after thirty idle seconds, with a status saying whether
  # it saw the whole call.
  callee_status=$(docker wait "cluster-callee-${name}" 2>/dev/null || echo 1)
  docker logs "cluster-callee-${name}" >"${RESULTS}/cluster-${name}-callee.log" 2>&1 || true
  docker rm -f "cluster-callee-${name}" >/dev/null 2>&1 || true

  if [ "${caller_ok}" -eq 0 ] && [ "${callee_status}" = "0" ]; then
    passed=$((passed + 1))
  else
    failed=$((failed + 1))
    failures="${failures} ${name}"
    echo "    failed - see ${RESULTS}/cluster-${name}.log and cluster-${name}-callee.log"
  fi
}

relayed_total() {
  api "http://$1:8080/metrics" 2>/dev/null | awk '/^athenasip_media_packets_relayed_total /{print $2}'
}

echo "Running scenarios..."

# The control: both ends on node A, which is the single-node call on the cluster's stack.
run_pair one-node-invite-bye "${NODE_A}" uas.xml "${NODE_A}" invite_bye.xml 30s

# Bob is held by node B, and Alice calls through node A.
run_pair across-invite-bye "${NODE_B}" uas.xml "${NODE_A}" invite_bye.xml 30s

# And the reverse, so each node is both first and second hop.
run_pair across-reversed-invite-bye "${NODE_A}" uas.xml "${NODE_B}" invite_bye.xml 30s

# Media across the two: the first node anchors and the second does not, so node A's relay
# carries the packets and node B's carries none.
case "across-media" in
  *${FILTER}*)
    a_before=$(relayed_total "${NODE_A}")
    b_before=$(relayed_total "${NODE_B}")

    run_pair across-media "${NODE_B}" uas_media.xml "${NODE_A}" invite_media.xml 40s

    a_relayed=$(($(relayed_total "${NODE_A}") - ${a_before:-0}))
    b_relayed=$(($(relayed_total "${NODE_B}") - ${b_before:-0}))

    echo "  across-media-anchored-once (${a_relayed} packets through node A, ${b_relayed} through node B)"

    if [ "${a_relayed}" -gt 0 ] && [ "${b_relayed}" -eq 0 ]; then
      passed=$((passed + 1))
    else
      failed=$((failed + 1))
      failures="${failures} across-media-anchored-once"
      echo "    failed - the first node anchors the media and the second leaves it alone"
    fi
    ;;
esac

# The single-node scenarios with the callee on the other node: cancel while ringing,
# busy, an INVITE with no offer, hold and resume, and no answer.
run_pair across-cancel-ringing "${NODE_B}" uas_ringing.xml       "${NODE_A}" cancel_after_180.xml     30s
run_pair across-busy           "${NODE_B}" uas_busy.xml          "${NODE_A}" invite_busy.xml          30s
run_pair across-delayed-offer  "${NODE_B}" uas_delayed_offer.xml "${NODE_A}" invite_delayed_offer.xml 30s
run_pair across-hold-resume    "${NODE_B}" uas_hold.xml          "${NODE_A}" invite_hold.xml          30s

# A callee on a connection: on the caller's own node as the control, then on the other
# node, over TCP and over WebSocket.
run_flow one-node-tcp-flow  tcp "${NODE_A}" "${NODE_A}"
run_flow across-tcp-flow    tcp "${NODE_B}" "${NODE_A}"
run_flow one-node-ws-flow   ws  "${NODE_A}" "${NODE_A}"
run_flow across-ws-flow     ws  "${NODE_B}" "${NODE_A}"

# Call records: one per call, written by the caller's node to the shared store, so both
# nodes list the same records and a call that crossed names both nodes.
case "across-call-records" in
  *${FILTER}*)
    from_a=$(api "http://${NODE_A}:8080/api/v1/call-records?limit=1000" 2>/dev/null)
    from_b=$(api "http://${NODE_B}:8080/api/v1/call-records?limit=1000" 2>/dev/null)

    count_a=$(printf '%s' "${from_a}" | grep -o '"id":' | wc -l | tr -d ' ')
    count_b=$(printf '%s' "${from_b}" | grep -o '"id":' | wc -l | tr -d ' ')
    crossed=$(printf '%s' "${from_b}" | grep -o '"nodes":\["node-a","node-b"\]' | wc -l | tr -d ' ')

    echo "  across-call-records (${count_a} from node A, ${count_b} from node B, ${crossed} naming both nodes)"

    if [ "${count_a}" -gt 0 ] && [ "${count_a}" = "${count_b}" ] && [ "${crossed}" -gt 0 ]; then
      passed=$((passed + 1))
    else
      failed=$((failed + 1))
      failures="${failures} across-call-records"
      echo "    failed - both nodes should list the same records, and a call across them should name both"
    fi
    ;;
esac

# Last, because timer B is 64*T1 and this one waits it out.
run_pair across-invite-timeout "${NODE_B}" uas_silent.xml "${NODE_A}" invite_timeout.xml 60s carol.csv carol alice-to-carol.csv

echo
echo "${passed} passed, ${failed} failed"

if [ "${failed}" -ne 0 ]; then
  echo "Failed:${failures}"
  exit 1
fi
