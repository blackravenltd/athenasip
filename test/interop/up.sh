#!/usr/bin/env bash
#
# AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# Brings up the interop fixture and provisions it over the admin API, which is the same
# path an operator uses and a second check that the API works.
#
#   test/interop/up.sh              bring it up and provision it
#   test/interop/up.sh --rtpengine  the same, with rtpengine on the media path
#   test/interop/up.sh --admin      also serve the admin client, whose softphone is the
#                                   browser end of the Milestone 3 harness
#   test/interop/up.sh down         take it down
#
# The in-process relay is plain RTP, which is what an ordinary softphone expects. A
# browser needs rtpengine, and --rtpengine also moves the fixture off loopback: the
# address a client is told to send media to has to be one it can reach, and a phone on
# a device is not on loopback.
#
# The ports default to the ones AthenaPhone's harness already uses for its Asterisk
# fixture, so pointing that harness here changes a host and a CA. Move them to run both
# at once:
#
#   ATHENA_INTEROP_SIP_PORT=15060 ATHENA_INTEROP_TLS_PORT=15061 \
#   ATHENA_INTEROP_WS_PORT=18088  ATHENA_INTEROP_API_PORT=18080 \
#   ATHENA_INTEROP_RTP_MIN=23000  ATHENA_INTEROP_RTP_MAX=23020 \
#   ATHENA_INTEROP_NAME=athenasip-interop-alt test/interop/up.sh
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

ENGINE="builtin"
ACTION="up"
SERVE_ADMIN="no"

for argument in "$@"; do
  case "$argument" in
    --rtpengine) ENGINE="rtpengine" ;;
    --admin) SERVE_ADMIN="yes" ;;
    up|down) ACTION="$argument" ;;
    *) echo "Unknown argument: $argument" >&2; exit 1 ;;
  esac
done

export ATHENA_INTEROP_SIP_PORT="${ATHENA_INTEROP_SIP_PORT:-5060}"
export ATHENA_INTEROP_TLS_PORT="${ATHENA_INTEROP_TLS_PORT:-5061}"
export ATHENA_INTEROP_WS_PORT="${ATHENA_INTEROP_WS_PORT:-8088}"
export ATHENA_INTEROP_API_PORT="${ATHENA_INTEROP_API_PORT:-8080}"
export ATHENA_INTEROP_RTP_MIN="${ATHENA_INTEROP_RTP_MIN:-22000}"
export ATHENA_INTEROP_RTP_MAX="${ATHENA_INTEROP_RTP_MAX:-22100}"
export ATHENA_INTEROP_NAME="${ATHENA_INTEROP_NAME:-athenasip-interop}"

# The address this host has on the network a client is on. Loopback is enough while the
# client is a process on this machine; it is no use at all to a phone on the LAN or to
# a browser told where to send its media, which is why rtpengine moves off it.
lan_address() {
  local interface
  if interface="$(route -n get default 2>/dev/null | awk '/interface:/{print $2}')" && [[ -n "$interface" ]]; then
    ipconfig getifaddr "$interface" 2>/dev/null && return 0
  fi

  ip route get 1.1.1.1 2>/dev/null | awk '{for (i = 1; i < NF; i++) if ($i == "src") {print $(i + 1); exit}}'
}

if [[ -z "${ATHENA_INTEROP_PUBLIC_ADDRESS:-}" && "$ENGINE" == "rtpengine" ]]; then
  ATHENA_INTEROP_PUBLIC_ADDRESS="$(lan_address || true)"

  if [[ -z "$ATHENA_INTEROP_PUBLIC_ADDRESS" ]]; then
    echo "Cannot work out this host's address on its own network - set ATHENA_INTEROP_PUBLIC_ADDRESS." >&2
    exit 1
  fi
fi

export ATHENA_INTEROP_PUBLIC_ADDRESS="${ATHENA_INTEROP_PUBLIC_ADDRESS:-127.0.0.1}"
PUBLIC_ADDRESS="$ATHENA_INTEROP_PUBLIC_ADDRESS"

# What the host publishes the fixture on, which follows from the address it advertises:
# a client on another device cannot reach a port published to loopback only.
if [[ -z "${ATHENA_INTEROP_BIND:-}" ]]; then
  case "$PUBLIC_ADDRESS" in
    127.*|localhost) ATHENA_INTEROP_BIND="127.0.0.1" ;;
    *) ATHENA_INTEROP_BIND="0.0.0.0" ;;
  esac
fi

export ATHENA_INTEROP_BIND
export ATHENA_INTEROP_RTPENGINE_ADDRESS="${ATHENA_INTEROP_RTPENGINE_ADDRESS:-172.32.0.30}"
export ATHENA_INTEROP_NG_PORT="${ATHENA_INTEROP_NG_PORT:-22222}"

# The engine is named by URL and nothing else changes, which is the plugin contract
# doing its job: the node is the same node either way.
if [[ "$ENGINE" == "rtpengine" ]]; then
  MEDIA_URL="rtpengine://${ATHENA_INTEROP_RTPENGINE_ADDRESS}:22222"
else
  MEDIA_URL="builtin://"
fi

API="http://127.0.0.1:${ATHENA_INTEROP_API_PORT}/api/v1"
ADMIN_TOKEN="interop-admin"

# The realm is named for the domain a client puts in its From and To, which for a
# fixture on loopback is the address it dialled. A realm named anything else is one the
# registrar will not find (RFC 3261 10.3 step 2).
REALM="${ATHENA_INTEROP_REALM:-${PUBLIC_ADDRESS}}"

# Taking it down names the overlay whether or not it was asked for, because compose
# removes what the files it was given describe: a down that did not name it would leave
# the rtpengine container's network behind.
COMPOSE_FILES=(-f "$HERE/docker-compose.yml")
if [[ "$ENGINE" == "rtpengine" || "$ACTION" == "down" ]]; then
  COMPOSE_FILES+=(-f "$HERE/docker-compose.rtpengine.yml")
fi

compose() { docker compose -p "${ATHENA_INTEROP_NAME}" "${COMPOSE_FILES[@]}" "$@"; }

if [[ "$ACTION" == "down" ]]; then
  compose down --remove-orphans
  exit 0
fi

# Anything left from a previous run goes first, so that bringing the fixture up twice
# works and the check below does not find this fixture's own ports.
compose down --remove-orphans >/dev/null 2>&1 || true

# A port already taken is the common way this fails, and Docker's own message does not
# say who has it. Asterisk on the same ports is the likely answer, because this fixture
# deliberately uses them.
taken=()
for port in "$ATHENA_INTEROP_SIP_PORT" "$ATHENA_INTEROP_TLS_PORT" "$ATHENA_INTEROP_WS_PORT" "$ATHENA_INTEROP_API_PORT"; do
  if lsof -nP -iTCP:"$port" -sTCP:LISTEN >/dev/null 2>&1; then taken+=("$port"); fi
done

if (( ${#taken[@]} > 0 )); then
  cat >&2 <<MSG
Ports already in use: ${taken[*]}

Something else is listening, and on these ports it is most likely another SIP fixture -
this one uses the same numbers on purpose, so that one client can be pointed at either.

Stop the other one, or move this one:

  ATHENA_INTEROP_SIP_PORT=15060 ATHENA_INTEROP_TLS_PORT=15061 \\
  ATHENA_INTEROP_WS_PORT=18088  ATHENA_INTEROP_API_PORT=18080 \\
  ATHENA_INTEROP_RTP_MIN=23000  ATHENA_INTEROP_RTP_MAX=23020 \\
  ATHENA_INTEROP_NAME=athenasip-interop-alt test/interop/up.sh

Who has them:
MSG
  for port in "${taken[@]}"; do lsof -nP -iTCP:"$port" -sTCP:LISTEN 2>/dev/null | tail -n +2 | head -3 >&2; done
  exit 1
fi

# The checked-in certificate names loopback and nothing else, so a fixture advertising
# this machine's own address serves one that does not name what the client dialled -
# which fails verification however good the chain is. Sign a second one for this
# address with the same CA, so a client that already trusts tls/ca/snakeca.crt needs
# nothing new, and serve that instead.
export ATHENA_INTEROP_TLS_DIR="../../tls"

case "$PUBLIC_ADDRESS" in
  127.*|localhost) ;;
  *)
    echo "Certificate for ${PUBLIC_ADDRESS}..."
    "$HERE/../../tls/generate.sh" --server-only --out "$HERE/generated/tls" "$PUBLIC_ADDRESS" > /dev/null
    ATHENA_INTEROP_TLS_DIR="./generated/tls"
    ;;
esac

# The admin client is built in its own repository and its output is mounted here rather
# than checked in: a built bundle in this tree would be a copy that goes stale, and the
# client is versioned where it is written.
export ATHENA_INTEROP_ADMIN_DIR="${ATHENA_INTEROP_ADMIN_DIR:-$HERE/../../../athenasip-admin/build}"
FILES_ENABLE="false"

if [[ "$SERVE_ADMIN" == "yes" ]]; then
  if [[ ! -f "$ATHENA_INTEROP_ADMIN_DIR/index.html" ]]; then
    cat >&2 <<MSG
No admin client to serve at ${ATHENA_INTEROP_ADMIN_DIR}

Build it first:

  (cd ../athenasip-admin && npm run build)

or point ATHENA_INTEROP_ADMIN_DIR at a directory that has one. Starting a node that
serves nothing would only fail later and further away.
MSG
    exit 1
  fi

  FILES_ENABLE="true"
  ADMIN_NOTE="Admin client served from ${ATHENA_INTEROP_ADMIN_DIR}, at http://127.0.0.1:${ATHENA_INTEROP_API_PORT}/ -
open the softphone over loopback like that whatever the node advertises: localhost is a
secure origin, which is what getUserMedia needs."
else
  # Nothing to serve, and a mount that has to resolve to something: the fixture's own
  # generated directory is there in every case and is never served, because files are
  # disabled.
  ATHENA_INTEROP_ADMIN_DIR="$HERE/generated"
  ADMIN_NOTE="No admin client served. Add --admin for the softphone the browser harness drives."
fi

# The node binds what the host publishes, so the config is rendered rather than fixed.
mkdir -p "$HERE/generated"
sed -e "s|@PUBLIC_ADDRESS@|${PUBLIC_ADDRESS}|g" \
    -e "s|@SIP_PORT@|${ATHENA_INTEROP_SIP_PORT}|g" \
    -e "s|@TLS_PORT@|${ATHENA_INTEROP_TLS_PORT}|g" \
    -e "s|@WS_PORT@|${ATHENA_INTEROP_WS_PORT}|g" \
    -e "s|@API_PORT@|${ATHENA_INTEROP_API_PORT}|g" \
    -e "s|@RTP_MIN@|${ATHENA_INTEROP_RTP_MIN}|g" \
    -e "s|@RTP_MAX@|${ATHENA_INTEROP_RTP_MAX}|g" \
    -e "s|@MEDIA_URL@|${MEDIA_URL}|g" \
    -e "s|@FILES_ENABLE@|${FILES_ENABLE}|g" \
    "$HERE/config.yaml.template" > "$HERE/generated/config.yaml"

echo "Building and starting..."
compose up -d --build --wait

echo "Provisioning realm ${REALM}..."
curl -fsS -X POST "$API/realms" \
  -H "Authorization: Bearer $ADMIN_TOKEN" \
  -H "Content-Type: application/json" \
  -d "{\"name\":\"${REALM}\",\"registration_timeout\":3600}" > /dev/null

while IFS=, read -r username password; do
  [[ "$username" == "username" || -z "$username" ]] && continue

  echo "  account ${username}@${REALM}"
  curl -fsS -X POST "$API/realms/${REALM}/accounts" \
    -H "Authorization: Bearer $ADMIN_TOKEN" \
    -H "Content-Type: application/json" \
    -d "{\"user\":\"${username}\",\"password\":\"${password}\"}" > /dev/null
done < "$HERE/accounts.csv"

# What the fixture actually turned out to be, for anything that has to talk to it and
# did not choose these values itself. The addresses are worked out here - the LAN one is
# detected - so a wrapper that guessed them again could guess differently.
cat > "$HERE/generated/fixture.env" <<ENV
ATHENA_INTEROP_NAME=${ATHENA_INTEROP_NAME}
ATHENA_INTEROP_PUBLIC_ADDRESS=${PUBLIC_ADDRESS}
ATHENA_INTEROP_BIND=${ATHENA_INTEROP_BIND}
ATHENA_INTEROP_REALM=${REALM}
ATHENA_INTEROP_SIP_PORT=${ATHENA_INTEROP_SIP_PORT}
ATHENA_INTEROP_TLS_PORT=${ATHENA_INTEROP_TLS_PORT}
ATHENA_INTEROP_WS_PORT=${ATHENA_INTEROP_WS_PORT}
ATHENA_INTEROP_API_PORT=${ATHENA_INTEROP_API_PORT}
ATHENA_INTEROP_RTP_MIN=${ATHENA_INTEROP_RTP_MIN}
ATHENA_INTEROP_RTP_MAX=${ATHENA_INTEROP_RTP_MAX}
ATHENA_INTEROP_MEDIA_ENGINE=${ENGINE}
ATHENA_INTEROP_NG_PORT=${ATHENA_INTEROP_NG_PORT}
ATHENA_INTEROP_ADMIN_DIR=${ATHENA_INTEROP_ADMIN_DIR}
ATHENA_INTEROP_PASSWORD=${ATHENA_INTEROP_PASSWORD:-athenaphone}
ENV

cat <<MSG

Ready. The node is listening on ${PUBLIC_ADDRESS}:

  udp   ${ATHENA_INTEROP_SIP_PORT}
  tcp   ${ATHENA_INTEROP_SIP_PORT}
  tls   ${ATHENA_INTEROP_TLS_PORT}    verify against tls/ca/snakeca.crt
  ws    ${ATHENA_INTEROP_WS_PORT}    any path; /ws is what most clients ask for
  api   ${ATHENA_INTEROP_API_PORT}    bearer interop-admin / interop-client

Media is ${ENGINE}, on ${PUBLIC_ADDRESS}:${ATHENA_INTEROP_RTP_MIN}-${ATHENA_INTEROP_RTP_MAX}.
${ADMIN_NOTE}

Accounts are 1001, 1002 and 1003 in realm ${REALM}, password athenaphone.

  test/interop/smoke.py --host ${PUBLIC_ADDRESS}   prove it before blaming a client
  docker logs -f ${ATHENA_INTEROP_NAME}     what the node is doing
  ATHENA_INTEROP_NAME=${ATHENA_INTEROP_NAME} test/interop/up.sh down
MSG
