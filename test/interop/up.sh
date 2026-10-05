#!/usr/bin/env bash
#
# AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# Brings up the interop fixture and provisions it over the admin API.
#
#   test/interop/up.sh              bring it up and provision it
#   test/interop/up.sh --rtpengine  the same, with rtpengine on the media path
#   test/interop/up.sh --admin      also serve the admin client, whose softphone is the
#                                   browser end of the browser-call test
#   test/interop/up.sh down         take it down
#
# The builtin relay is plain RTP, which suits an ordinary softphone. A browser needs
# rtpengine, and --rtpengine also moves the fixture off loopback so that a phone on
# another device can reach the media address.
#
# The ports default to those AthenaPhone's harness uses for its Asterisk fixture. Move
# them to run both at once:
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

# This host's address on its network. Loopback only serves clients on this machine.
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

# The host address the fixture is published on: a client on another device cannot reach
# a port published to loopback only.
if [[ -z "${ATHENA_INTEROP_BIND:-}" ]]; then
  case "$PUBLIC_ADDRESS" in
    127.*|localhost) ATHENA_INTEROP_BIND="127.0.0.1" ;;
    *) ATHENA_INTEROP_BIND="0.0.0.0" ;;
  esac
fi

export ATHENA_INTEROP_BIND
export ATHENA_INTEROP_RTPENGINE_ADDRESS="${ATHENA_INTEROP_RTPENGINE_ADDRESS:-172.32.0.30}"

# The address rtpengine writes into session descriptions. The engine's own address on the
# fixture network makes a relay-only (TURN) call work but is unreachable to a direct
# client, which is why browser.sh runs the two cases separately.
export ATHENA_INTEROP_RTPENGINE_ADVERTISE="${ATHENA_INTEROP_RTPENGINE_ADVERTISE:-$PUBLIC_ADDRESS}"

export ATHENA_INTEROP_TURN_PORT="${ATHENA_INTEROP_TURN_PORT:-3478}"
export ATHENA_INTEROP_TURN_MIN="${ATHENA_INTEROP_TURN_MIN:-22300}"
export ATHENA_INTEROP_TURN_MAX="${ATHENA_INTEROP_TURN_MAX:-22350}"

# Per run, so no TURN credential outlives the fixture.
TURN_SECRET="${ATHENA_INTEROP_TURN_SECRET:-$(openssl rand -hex 16)}"
export ATHENA_INTEROP_NG_PORT="${ATHENA_INTEROP_NG_PORT:-22222}"
export ATHENA_INTEROP_LOG_MESSAGES="${ATHENA_INTEROP_LOG_MESSAGES:-true}"

# The engine is selected by URL; nothing else in the node's configuration changes.
if [[ "$ENGINE" == "rtpengine" ]]; then
  MEDIA_URL="rtpengine://${ATHENA_INTEROP_RTPENGINE_ADDRESS}:22222"
else
  MEDIA_URL="builtin://"
fi

API="http://127.0.0.1:${ATHENA_INTEROP_API_PORT}/api/v1"
# The fixture's user, created by the node as it starts, with a password per run because
# the fixture can be bound to the LAN.
export ATHENA_INTEROP_API_USER="${ATHENA_INTEROP_API_USER:-interop}"
export ATHENA_INTEROP_API_PASSWORD="${ATHENA_INTEROP_API_PASSWORD:-$(openssl rand -hex 10)}"

# The realm is the domain clients put in From and To (RFC 3261 10.3 step 2).
REALM="${ATHENA_INTEROP_REALM:-${PUBLIC_ADDRESS}}"

# down always names the overlay, or compose would leave the rtpengine network behind.
COMPOSE_FILES=(-f "$HERE/docker-compose.yml")
if [[ "$ENGINE" == "rtpengine" || "$ACTION" == "down" ]]; then
  COMPOSE_FILES+=(-f "$HERE/docker-compose.rtpengine.yml")
fi

compose() { docker compose -p "${ATHENA_INTEROP_NAME}" "${COMPOSE_FILES[@]}" "$@"; }

if [[ "$ACTION" == "down" ]]; then
  compose down --remove-orphans

  # Remove the rendered state too, so a stale fixture.env is not read as current.
  rm -f "$HERE/generated/fixture.env" "$HERE/generated/config.yaml" "$HERE/generated/coturn/turnserver.conf"
  exit 0
fi

# Remove a previous run so the port check does not find this fixture's own ports.
compose down --remove-orphans >/dev/null 2>&1 || true

# Docker does not say who holds a taken port. Asterisk is likely: the defaults match its.
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

# The checked-in certificate names only loopback. For any other address, sign a second
# one with the same CA, so a client trusting tls/ca/snakeca.crt needs nothing new.
export ATHENA_INTEROP_TLS_DIR="../../tls"

case "$PUBLIC_ADDRESS" in
  127.*|localhost) ;;
  *)
    echo "Certificate for ${PUBLIC_ADDRESS}..."
    "$HERE/../../tls/generate.sh" --server-only --out "$HERE/generated/tls" "$PUBLIC_ADDRESS" > /dev/null
    ATHENA_INTEROP_TLS_DIR="./generated/tls"
    ;;
esac

# The admin client is built in its own repository and mounted when present.
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
  # The mount must still resolve: this directory always exists, and file serving is off.
  ATHENA_INTEROP_ADMIN_DIR="$HERE/generated"
  ADMIN_NOTE="No admin client served. Add --admin for the softphone the browser harness drives."
fi

# ICE servers only with rtpengine: coturn is in that overlay.
if [[ "$ENGINE" == "rtpengine" ]]; then
  ICE_SERVERS=$(printf '    ice_servers:\n      - url: "stun:%s:%s"\n      - url: "turn:%s:%s"' \
    "$PUBLIC_ADDRESS" "$ATHENA_INTEROP_TURN_PORT" "$PUBLIC_ADDRESS" "$ATHENA_INTEROP_TURN_PORT")

  mkdir -p "$HERE/generated/coturn"
  sed -e "s|@PUBLIC_ADDRESS@|${PUBLIC_ADDRESS}|g" \
      -e "s|@TURN_SECRET@|${TURN_SECRET}|g" \
      -e "s|@TURN_MIN@|${ATHENA_INTEROP_TURN_MIN}|g" \
      -e "s|@TURN_MAX@|${ATHENA_INTEROP_TURN_MAX}|g" \
      "$HERE/turnserver.conf" > "$HERE/generated/coturn/turnserver.conf"
else
  ICE_SERVERS="    ice_servers: []"
  TURN_SECRET=""
fi

# Rendered, because the node binds the ports the host publishes.
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
    -e "s|@LOG_MESSAGES@|${ATHENA_INTEROP_LOG_MESSAGES}|g" \
    -e "s|@TURN_SECRET@|${TURN_SECRET}|g" \
    -e "s|@ICE_SERVERS@|${ICE_SERVERS//$'\n'/\\n}|g" \
    "$HERE/config.yaml.template" > "$HERE/generated/config.yaml"

echo "Building and starting..."
compose up -d --build --wait

ADMIN_TOKEN="$(curl -fsS -X POST "$API/auth/login" -H "Content-Type: application/json" \
  -d "{\"username\":\"${ATHENA_INTEROP_API_USER}\",\"password\":\"${ATHENA_INTEROP_API_PASSWORD}\"}" | sed -n 's/.*"token":"\([0-9a-f]*\)".*/\1/p')"

if [[ -z "$ADMIN_TOKEN" ]]; then
  echo "Could not sign in as ${ATHENA_INTEROP_API_USER}" >&2
  exit 1
fi

echo "Provisioning realm ${REALM}..."
curl -fsS -X POST "$API/realms" \
  -H "Authorization: Bearer $ADMIN_TOKEN" \
  -H "Content-Type: application/json" \
  -d "{\"name\":\"${REALM}\",\"registration_timeout\":3600}" > /dev/null

while IFS=, read -r username password; do
  [[ "$username" == "username" || -z "$username" ]] && continue

  echo "  subscriber ${username}@${REALM}"
  curl -fsS -X POST "$API/realms/${REALM}/subscribers" \
    -H "Authorization: Bearer $ADMIN_TOKEN" \
    -H "Content-Type: application/json" \
    -d "{\"user\":\"${username}\",\"password\":\"${password}\"}" > /dev/null
done < "$HERE/subscribers.csv"

# The fixture's effective values, for anything that talks to it. Read these rather than
# working them out again: the LAN address is detected.
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

# The administrator the fixture makes, for the admin API.
ATHENA_INTEROP_API_USER=${ATHENA_INTEROP_API_USER}
ATHENA_INTEROP_API_PASSWORD=${ATHENA_INTEROP_API_PASSWORD}
ATHENA_INTEROP_RTPENGINE_ADVERTISE=${ATHENA_INTEROP_RTPENGINE_ADVERTISE}
ATHENA_INTEROP_TURN_PORT=${ATHENA_INTEROP_TURN_PORT}
ATHENA_INTEROP_TURN_MIN=${ATHENA_INTEROP_TURN_MIN}
ATHENA_INTEROP_TURN_MAX=${ATHENA_INTEROP_TURN_MAX}
ATHENA_INTEROP_TURN_SECRET=${TURN_SECRET}
ENV

cat <<MSG

Ready. The node is listening on ${PUBLIC_ADDRESS}:

  udp   ${ATHENA_INTEROP_SIP_PORT}
  tcp   ${ATHENA_INTEROP_SIP_PORT}
  tls   ${ATHENA_INTEROP_TLS_PORT}    verify against tls/ca/snakeca.crt
  ws    ${ATHENA_INTEROP_WS_PORT}    any path; /ws is what most clients ask for
  api   ${ATHENA_INTEROP_API_PORT}    sign in as ${ATHENA_INTEROP_API_USER}, password in generated/fixture.env

Media is ${ENGINE}, on ${PUBLIC_ADDRESS}:${ATHENA_INTEROP_RTP_MIN}-${ATHENA_INTEROP_RTP_MAX}.
${ADMIN_NOTE}

Subscribers are 1001, 1002 and 1003 in realm ${REALM}, password athenaphone.

  test/interop/smoke.py --host ${PUBLIC_ADDRESS}   prove it before blaming a client
  docker logs -f ${ATHENA_INTEROP_NAME}     every message in and out, bodies included
  ATHENA_INTEROP_NAME=${ATHENA_INTEROP_NAME} test/interop/up.sh down
MSG
