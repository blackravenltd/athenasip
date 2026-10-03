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
# What it proves that run.sh cannot: a node reads a binding whose flow another node holds,
# forwards the call over mutual TLS, and the ACK and the BYE cross back the same way. The
# first scenario is the control - both ends on one node of the cluster - so that when a
# call across the two fails, what failed is the crossing and not the cluster's plumbing.

set -u

cd "$(dirname "$0")/../.."

COMPOSE="docker compose -f docker-compose.cluster.yml"
IMAGE="athenasip-e2e-cluster-node"

NODE_A="172.32.0.10"
NODE_B="172.32.0.11"
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

# A port of its own for every sipp run, for the reason run.sh gives: sipp derives its
# branch from the call number, so two runs from one port are one transaction.
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

# The cluster's own authority and a certificate for each node, made the way an operator
# makes them (docs/certificates.md). Fresh every run: the authority refuses to be made over
# an existing one.
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

# The first administrator, made on the host as it has to be. With redis:// the command
# writes the user and exits, and both nodes then read it from the store they share.
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

# Each node has to have heard the other say where its peers reach it, or there is nobody
# to forward to. The session made on node A is good on node B: one store, one set of users.
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

# The callee registers with one node and waits; the caller registers with, and calls
# through, another - or the same one, for the control.
run_pair() {
  name="$1"
  uas_node="$2"
  uas_scenario="$3"
  uac_node="$4"
  uac_scenario="$5"
  timeout="$6"

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

  # Every binding of Bob's, from whichever node took it: the store is shared.
  ${COMPOSE} run --rm sipp-uas \
    -sf /e2e/scenarios/deregister.xml -inf /e2e/bob.csv -au bob -ap bob-secret \
    -p "${dereg_port}" -cid_str "${name}-uas-dereg-%u-%p@%s" \
    -m 1 -r 1 -timeout 20s -timeout_error \
    -nostdin "${uas_node}:5060" >"${RESULTS}/cluster-${name}-uas-deregister.log" 2>&1 || true

  ${COMPOSE} run --rm sipp-uas \
    -sf /e2e/scenarios/register.xml -inf /e2e/bob.csv -au bob -ap bob-secret \
    -p "${uas_port}" -cid_str "${name}-uas-reg-%u-%p@%s" \
    -m 1 -r 1 -timeout 20s -timeout_error \
    -nostdin "${uas_node}:5060" >"${RESULTS}/cluster-${name}-uas-register.log" 2>&1 || {
    failed=$((failed + 1))
    failures="${failures} ${name}"
    echo "    failed to register the callee - see ${RESULTS}/cluster-${name}-uas-register.log"
    return 0
  }

  ${COMPOSE} run -d --name "cluster-uas-${name}" sipp-uas \
    -sf "/e2e/scenarios/${uas_scenario}" -inf /e2e/bob.csv -au bob -ap bob-secret \
    -p "${uas_port}" -cid_str "${name}-uas-%u-%p@%s" \
    -m 1 -r 1 -timeout "${timeout}" -timeout_error \
    -trace_err -error_file "/results/cluster-${name}-uas.err" \
    -nostdin "${uas_node}:5060" >/dev/null 2>&1

  sleep 2

  if ${COMPOSE} run --rm sipp-uac \
    -sf "/e2e/scenarios/${uac_scenario}" -inf /e2e/alice.csv -au alice -ap alice-secret \
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

# The callee holds a connection to one node and is reachable only down it; the caller calls
# through another. This is what forwarding is for: a UDP callee's Contact is an address any
# node could send to, and a connection is not.
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

  # Registered, as the node the caller will ask sees it: the binding is in the shared store.
  attempt=0
  until api "http://${caller_node}:8080/api/v1/registrations" 2>/dev/null | grep -q "\"flow_id\":\"${transport}://172.32.0.22"; do
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

  # The callee exits when it has seen the BYE, or when thirty seconds pass with nothing
  # arriving on its connection, and says whether it saw the whole call.
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

# And the other way about, so neither node is only ever the first or only ever the second.
run_pair across-reversed-invite-bye "${NODE_A}" uas.xml "${NODE_B}" invite_bye.xml 30s

# Media across the two. The first node anchors it and the second leaves the call alone, so
# node A's relay carries the packets and node B's carries none.
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

# A callee on a connection. First on the caller's own node, as the control, then held by
# the other node, for each of the two transports that are a connection.
run_flow one-node-tcp-flow  tcp "${NODE_A}" "${NODE_A}"
run_flow across-tcp-flow    tcp "${NODE_B}" "${NODE_A}"
run_flow one-node-ws-flow   ws  "${NODE_A}" "${NODE_A}"
run_flow across-ws-flow     ws  "${NODE_B}" "${NODE_A}"

echo
echo "${passed} passed, ${failed} failed"

if [ "${failed}" -ne 0 ]; then
  echo "Failed:${failures}"
  exit 1
fi
