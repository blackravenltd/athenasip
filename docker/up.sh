#!/usr/bin/env bash
#
# AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# One command to a working, administrable SIP server.
#
#   docker/up.sh             bring the stack up, seed it, say what to do next
#   docker/up.sh --console   also serve the admin console, built in its own repository
#   docker/up.sh --build     build the node's image from this tree rather than pull it
#   docker/up.sh down        take it down, keeping the data
#   docker/up.sh --reset     take it down and delete the data too
#
# Renders docker/generated/config.yaml with an address clients can reach, starts the
# stack and waits for health, creates the first administrator with `athenasip --add-user`,
# then provisions a realm and two subscribers over the admin API.
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

ACTION="up"
SERVE_CONSOLE="no"
RESET="no"
BUILD="no"

for argument in "$@"; do
  case "$argument" in
    --console) SERVE_CONSOLE="yes" ;;
    --build)   BUILD="yes" ;;
    --reset)   ACTION="down"; RESET="yes" ;;
    up|down)   ACTION="$argument" ;;
    *) echo "Unknown argument: $argument" >&2; exit 1 ;;
  esac
done

compose() { (cd "$ROOT" && docker compose "$@"); }

if [[ "$ACTION" == "down" ]]; then
  if [[ "$RESET" == "yes" ]]; then
    compose down --remove-orphans --volumes
    echo "Down, and the Redis and Mosquitto volumes are gone with it."
  else
    compose down --remove-orphans
    echo "Down. The data is still in the volumes; --reset forgets it too."
  fi
  exit 0
fi

# This host's address on its network. Loopback only serves clients on this machine.
lan_address() {
  local interface
  if interface="$(route -n get default 2>/dev/null | awk '/interface:/{print $2}')" && [[ -n "$interface" ]]; then
    ipconfig getifaddr "$interface" 2>/dev/null && return 0
  fi

  ip route get 1.1.1.1 2>/dev/null | awk '{for (i = 1; i < NF; i++) if ($i == "src") {print $(i + 1); exit}}'
}

PUBLIC_ADDRESS="${ATHENA_PUBLIC_ADDRESS:-$(lan_address || true)}"

if [[ -z "$PUBLIC_ADDRESS" ]]; then
  echo "Cannot work out this host's address on its own network - set ATHENA_PUBLIC_ADDRESS." >&2
  exit 1
fi

export ATHENA_PUBLIC_ADDRESS="$PUBLIC_ADDRESS"

# The realm is the domain clients put in From and To (RFC 3261 10.3 step 2).
REALM="${ATHENA_REALM:-$PUBLIC_ADDRESS}"

export ATHENA_SIP_PORT="${ATHENA_SIP_PORT:-5060}"
export ATHENA_TLS_PORT="${ATHENA_TLS_PORT:-5061}"
export ATHENA_WSS_PORT="${ATHENA_WSS_PORT:-9443}"
export ATHENA_API_PORT="${ATHENA_API_PORT:-8080}"

# rtpengine's fixed address on the compose network.
export ATHENA_RTPENGINE_ADDRESS="${ATHENA_RTPENGINE_ADDRESS:-172.33.0.30}"

# The address rtpengine writes into session descriptions. Set it to the engine's bridge
# address to exercise a relay-only (TURN) call on a loopback stack.
export ATHENA_RTPENGINE_ADVERTISE="${ATHENA_RTPENGINE_ADVERTISE:-$PUBLIC_ADDRESS}"
export ATHENA_RTP_MIN="${ATHENA_RTP_MIN:-25000}"
export ATHENA_RTP_MAX="${ATHENA_RTP_MAX:-25050}"
export ATHENA_TURN_MIN="${ATHENA_TURN_MIN:-25100}"
export ATHENA_TURN_MAX="${ATHENA_TURN_MAX:-25150}"

API="http://127.0.0.1:${ATHENA_API_PORT}/api/v1"

ADMIN_USER="${ATHENA_ADMIN_USER:-admin}"
ADMIN_PASSWORD="${ATHENA_ADMIN_PASSWORD:-}"

# Generated per stack unless ATHENA_ADMIN_PASSWORD is set.
if [[ -z "$ADMIN_PASSWORD" ]]; then
  # Not tr < /dev/urandom | head: head closing the pipe fails the script under pipefail.
  ADMIN_PASSWORD="$(openssl rand -hex 10)"
  GENERATED_PASSWORD="yes"
else
  GENERATED_PASSWORD="no"
fi

# The console is built in its own repository and mounted when present.
export ATHENA_CONSOLE_DIR="${ATHENA_CONSOLE_DIR:-$ROOT/../athenasip-admin/build}"

if [[ "$SERVE_CONSOLE" == "yes" ]]; then
  if [[ ! -f "$ATHENA_CONSOLE_DIR/index.html" ]]; then
    cat >&2 <<MSG
No admin console to serve at ${ATHENA_CONSOLE_DIR}

Build it first:

  (cd ../athenasip-admin && npm run build)

or point ATHENA_CONSOLE_DIR at a directory that has one. Starting a node that serves
nothing would only fail later and further away.
MSG
    exit 1
  fi
else
  # The mount must still resolve: this directory always exists, and file serving is off.
  ATHENA_CONSOLE_DIR="$HERE/generated"
fi

SERVE_CONSOLE_YAML="false"
[[ "$SERVE_CONSOLE" == "yes" ]] && SERVE_CONSOLE_YAML="true"

# Remove a previous run so the port check does not find this stack's own ports. The
# volumes, and so the provisioned data, stay; --reset deletes them.
compose down --remove-orphans >/dev/null 2>&1 || true

taken=()
for port in "$ATHENA_SIP_PORT" "$ATHENA_TLS_PORT" "$ATHENA_WSS_PORT" "$ATHENA_API_PORT"; do
  if lsof -nP -iTCP:"$port" -sTCP:LISTEN >/dev/null 2>&1; then taken+=("$port"); fi
done

if (( ${#taken[@]} > 0 )); then
  cat >&2 <<MSG
Ports already in use: ${taken[*]}

Something else is listening. On 5060 and 5061 it is most likely another SIP server, and
this stack uses the standard numbers on purpose. Stop it, or move this one:

  ATHENA_SIP_PORT=15060 ATHENA_TLS_PORT=15061 \\
  ATHENA_WSS_PORT=19443 ATHENA_API_PORT=18080 docker/up.sh

Who has them:
MSG
  for port in "${taken[@]}"; do lsof -nP -iTCP:"$port" -sTCP:LISTEN 2>/dev/null | tail -n +2 | head -3 >&2; done
  exit 1
fi

# Rendered fresh each run, so no stale generated file is mistaken for the live one.
rm -f "$HERE/generated/config.yaml" "$HERE/generated/turnserver.conf" "$HERE/generated/coturn/turnserver.conf"
mkdir -p "$HERE/generated"

sed -e "s|@RTPENGINE_ADDRESS@|${ATHENA_RTPENGINE_ADDRESS}|g" \
    -e "s|@SIP_PORT@|${ATHENA_SIP_PORT}|g" \
    -e "s|@TLS_PORT@|${ATHENA_TLS_PORT}|g" \
    -e "s|@WSS_PORT@|${ATHENA_WSS_PORT}|g" \
    -e "s|@API_PORT@|${ATHENA_API_PORT}|g" \
    -e "s|@PUBLIC_ADDRESS@|${PUBLIC_ADDRESS}|g" \
    -e "s|@SERVE_CONSOLE@|${SERVE_CONSOLE_YAML}|g" \
    "$HERE/config.yaml.template" > "$HERE/generated/config.yaml"

mkdir -p "$HERE/generated/coturn"
sed -e "s|@PUBLIC_ADDRESS@|${PUBLIC_ADDRESS}|g" \
    "$HERE/turnserver.conf" > "$HERE/generated/coturn/turnserver.conf"

# The checked-in certificate names only loopback. For any other address, sign a second
# one with the same CA, so a client trusting tls/ca/snakeca.crt needs nothing new.
export ATHENA_TLS_DIR="./tls"

case "$PUBLIC_ADDRESS" in
  127.*|localhost) ;;
  *)
    echo "Certificate for ${PUBLIC_ADDRESS}..."
    "$ROOT/tls/generate.sh" --server-only --out "$HERE/generated/tls" "$PUBLIC_ADDRESS" > /dev/null
    ATHENA_TLS_DIR="./docker/generated/tls"
    ;;
esac

# A loopback stack cannot relay: coturn would send to 127.0.0.1, which is itself.
case "$PUBLIC_ADDRESS" in
  127.*|localhost)
    if grep -q '^use-auth-secret' "$HERE/turnserver.conf" 2>/dev/null && [[ "$ATHENA_RTPENGINE_ADVERTISE" == "$PUBLIC_ADDRESS" ]]; then
      cat >&2 <<MSG
Note: this stack advertises ${PUBLIC_ADDRESS}, so TURN cannot relay to the media engine.
A browser will gather a relay candidate and the call will not carry audio through it,
because coturn would be relaying to its own loopback. Direct media is unaffected.

For a relay path that works, either run with this host's own address:

  ATHENA_PUBLIC_ADDRESS=<this host's LAN address> docker/up.sh

or keep loopback and point the engine at its bridge address, which exposes nothing:

  ATHENA_RTPENGINE_ADVERTISE=${ATHENA_RTPENGINE_ADDRESS} docker/up.sh

MSG
    fi
    ;;
esac

# The image published for this checkout's release, unless asked to build this tree: a
# checkout between releases has changes the published image does not.
VERSION="$(sed -n 's/^project(athenasip VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
if [[ "$BUILD" == "yes" ]]; then
  export ATHENA_IMAGE="athenasip:local"
  echo "Building this tree and starting..."
  compose up -d --build --wait
else
  export ATHENA_IMAGE="${ATHENA_IMAGE:-tomcully/athenasip:${VERSION}}"
  echo "Pulling ${ATHENA_IMAGE} and starting..."
  if ! compose pull athenasip; then
    echo "${ATHENA_IMAGE} is not published; building this tree instead."
    export ATHENA_IMAGE="athenasip:local"
    compose up -d --build --wait
  else
    compose up -d --wait
  fi
fi

# The first administrator, created inside the container; the password goes in on stdin.
# Exit 3 means it already exists, which is fine on a second run.
echo "Creating administrator ${ADMIN_USER}..."
set +e
printf '%s\n' "$ADMIN_PASSWORD" | compose exec -T athenasip athenasip --add-user "$ADMIN_USER" --display-name "Quickstart Administrator" \
  --role manage-admin-users --role manage-realms --role manage-realm-subscribers --role view-cluster-status >/dev/null
added=$?
set -e

case "$added" in
  0) echo "  ${ADMIN_USER}" ;;
  3)
    # The generated password is not the one in force, so it is not printed.
    echo "  ${ADMIN_USER} - already there, keeping the password it has"
    [[ "$GENERATED_PASSWORD" == "yes" ]] && GENERATED_PASSWORD="kept"
    ;;
  *)
    echo "Could not create the administrator (athenasip --add-user exited ${added})" >&2
    exit 1
    ;;
esac

# Provisioning signs in as the administrator. With a kept, unknown password there is
# nothing to sign in with, and the realm and subscribers are already in Redis.
ADMIN_TOKEN=""
if [[ "$GENERATED_PASSWORD" != "kept" ]]; then
  ADMIN_TOKEN="$(curl -s -X POST "$API/auth/login" -H "Content-Type: application/json" \
    -d "{\"username\":\"${ADMIN_USER}\",\"password\":\"${ADMIN_PASSWORD}\"}" | sed -n 's/.*"token":"\([0-9a-f]*\)".*/\1/p')"
fi

# Idempotent, because the data outlives the containers: 409 means already there.
provision() {
  local what="$1" path="$2" body="$3"
  local status

  status="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$API$path" \
    -H "Authorization: Bearer $ADMIN_TOKEN" \
    -H "Content-Type: application/json" \
    -d "$body")"

  case "$status" in
    201) echo "  ${what}" ;;
    409) echo "  ${what} - already there" ;;
    *)
      echo "Could not provision ${what}: HTTP ${status}" >&2
      return 1
      ;;
  esac
}

if [[ -n "$ADMIN_TOKEN" ]]; then
  echo "Provisioning realm ${REALM}..."
  provision "realm ${REALM}" "/realms" "{\"name\":\"${REALM}\",\"registration_timeout\":3600}"

  while IFS=, read -r username password; do
    [[ "$username" == "username" || -z "$username" ]] && continue

    provision "subscriber ${username}@${REALM}" "/realms/${REALM}/subscribers" \
      "{\"user\":\"${username}\",\"password\":\"${password}\"}"
  done < "$HERE/subscribers.csv"
elif [[ "$GENERATED_PASSWORD" == "kept" ]]; then
  echo "Realm and subscribers left as an earlier run made them. To assert them again, run with"
  echo "ATHENA_ADMIN_PASSWORD set to ${ADMIN_USER}'s password."
else
  echo "Could not sign in as ${ADMIN_USER} to provision the realm" >&2
  exit 1
fi

CONSOLE_NOTE="No console served. docker/up.sh --console serves one that has been built."
[[ "$SERVE_CONSOLE" == "yes" ]] && CONSOLE_NOTE="Console at http://127.0.0.1:${ATHENA_API_PORT}/ from ${ATHENA_CONSOLE_DIR}"

cat <<MSG

AthenaSIP is up, on ${PUBLIC_ADDRESS}.

  SIP        udp/tcp ${PUBLIC_ADDRESS}:${ATHENA_SIP_PORT}    tls ${PUBLIC_ADDRESS}:${ATHENA_TLS_PORT}
  WSS        wss://${PUBLIC_ADDRESS}:${ATHENA_WSS_PORT}
  Admin API  http://127.0.0.1:${ATHENA_API_PORT}/api/v1
  TURN       ${PUBLIC_ADDRESS}:3478, secret change-me-turn-secret

  Realm       ${REALM}
  Subscribers $(cut -d, -f1 "$HERE/subscribers.csv" | tail -n +2 | tr '\n' ' ')

MSG

if [[ "$GENERATED_PASSWORD" == "yes" ]]; then
  cat <<MSG
  Administrator  ${ADMIN_USER} / ${ADMIN_PASSWORD}

  Written down now or not at all: it is not stored anywhere this script can read it
  back, and the API never hands a password out. If you lose it, run:

    docker compose exec athenasip athenasip --add-user someone-else

MSG
elif [[ "$GENERATED_PASSWORD" == "kept" ]]; then
  echo "  Administrator  ${ADMIN_USER}, with the password it already had."
  echo
else
  echo "  Administrator  ${ADMIN_USER}, with the password you gave."
  echo
fi

cat <<MSG
The TLS certificate is the snakeoil one in tls/. A browser will refuse wss://...:9443
until you trust tls/ca/snakeca.crt, which is what it is there for - and it is not for
production, because its private key is in this repository.

Change this before anything you do not control can reach it: static-auth-secret in
docker/turnserver.conf.

  docker compose logs -f athenasip     what the node is doing
  docker/up.sh down                    stop, keeping the data
  docker/up.sh --reset                 stop and forget it
MSG
