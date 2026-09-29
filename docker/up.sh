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
#   docker/up.sh down        take it down, keeping the data
#   docker/up.sh --reset     take it down and forget the data too
#
# It renders docker/config.yaml from the template, because the node has to be told an
# address a client can come back to and a container's own address is not one; brings the
# stack up and waits for the node to report healthy; then provisions a realm, two accounts
# and the first administrator over the admin API, which is the same path an operator uses
# and a second check that the API works.
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

ACTION="up"
SERVE_CONSOLE="no"
RESET="no"

for argument in "$@"; do
  case "$argument" in
    --console) SERVE_CONSOLE="yes" ;;
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

# The address this host has on the network a client is on. Loopback is right for a
# softphone on this machine and no use at all to a phone on the LAN or to a browser being
# told where to send its media, so a stack meant for either needs the real one.
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

# The realm is the domain a client puts in its From and To. A realm named anything else is
# one the registrar will not find (RFC 3261 10.3 step 2).
REALM="${ATHENA_REALM:-$PUBLIC_ADDRESS}"

export ATHENA_SIP_PORT="${ATHENA_SIP_PORT:-5060}"
export ATHENA_TLS_PORT="${ATHENA_TLS_PORT:-5061}"
export ATHENA_WSS_PORT="${ATHENA_WSS_PORT:-9443}"
export ATHENA_API_PORT="${ATHENA_API_PORT:-8080}"

# rtpengine's address on the compose network, fixed so the node can be configured with it
# rather than with a name that would not resolve if the engine ever moved off this bridge.
export ATHENA_RTPENGINE_ADDRESS="${ATHENA_RTPENGINE_ADDRESS:-172.33.0.30}"
export ATHENA_RTP_MIN="${ATHENA_RTP_MIN:-25000}"
export ATHENA_RTP_MAX="${ATHENA_RTP_MAX:-25050}"
export ATHENA_TURN_MIN="${ATHENA_TURN_MIN:-25100}"
export ATHENA_TURN_MAX="${ATHENA_TURN_MAX:-25150}"

ADMIN_TOKEN="change-me-admin"
API="http://127.0.0.1:${ATHENA_API_PORT}/api/v1"

ADMIN_USER="${ATHENA_ADMIN_USER:-admin}"
ADMIN_PASSWORD="${ATHENA_ADMIN_PASSWORD:-}"

# A password nobody chose is not one to print and walk away from, so one is generated per
# stack rather than shipped in the repository for everybody to share.
if [[ -z "$ADMIN_PASSWORD" ]]; then
  # openssl rather than tr reading /dev/urandom into head: head closing the pipe kills
  # tr with SIGPIPE, and pipefail turns that into a script that exits 141 having printed
  # nothing at all.
  ADMIN_PASSWORD="$(openssl rand -hex 10)"
  GENERATED_PASSWORD="yes"
else
  GENERATED_PASSWORD="no"
fi

# The console lives in its own repository. Mounted when it has been built, and never
# checked in here, because a built bundle in this tree is a copy that goes stale.
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
  # Nothing to serve, and a mount that still has to resolve: this directory exists in
  # every case and is never served, because files are switched off.
  ATHENA_CONSOLE_DIR="$HERE/generated"
fi

SERVE_CONSOLE_YAML="false"
[[ "$SERVE_CONSOLE" == "yes" ]] && SERVE_CONSOLE_YAML="true"

# Anything left from a previous run goes first, so that bringing the stack up twice works
# and the check below does not find this stack's own ports. The volumes stay, so the
# realm, the accounts and the administrator survive; --reset is how you start over.
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

# The checked-in certificate names loopback and nothing else, so a stack advertising this
# machine's own address serves one that does not name what the client dialled - which
# fails verification however good the chain is. A second certificate for this address,
# signed by the same CA, means a client that already trusts tls/ca/snakeca.crt needs
# nothing new.
export ATHENA_TLS_DIR="./tls"

case "$PUBLIC_ADDRESS" in
  127.*|localhost) ;;
  *)
    echo "Certificate for ${PUBLIC_ADDRESS}..."
    "$ROOT/tls/generate.sh" --server-only --out "$HERE/generated/tls" "$PUBLIC_ADDRESS" > /dev/null
    ATHENA_TLS_DIR="./docker/generated/tls"
    ;;
esac

echo "Building and starting..."
compose up -d --build --wait

# Provisioning is idempotent, because the data outlives the containers: a second run
# finds the realm, the accounts and the administrator already there and that is the right
# answer rather than a failure. 409 is what the API says to "it already exists", and the
# only other acceptable answer is the one that created it.
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

echo "Provisioning realm ${REALM}..."
provision "realm ${REALM}" "/realms" "{\"name\":\"${REALM}\",\"registration_timeout\":3600}"

while IFS=, read -r username password; do
  [[ "$username" == "username" || -z "$username" ]] && continue

  provision "account ${username}@${REALM}" "/realms/${REALM}/accounts" \
    "{\"user\":\"${username}\",\"password\":\"${password}\"}"
done < "$HERE/accounts.csv"

# The first administrator, created with the configuration token, which is the bootstrap
# this API is built around: a fresh node has no users, so the token in the file is the
# only credential that exists until one of these is made.
#
# Already there on a second run, and that is not a failure: --reset is how you start over.
echo "Creating administrator ${ADMIN_USER}..."
created="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$API/users" \
  -H "Authorization: Bearer $ADMIN_TOKEN" \
  -H "Content-Type: application/json" \
  -d "{\"username\":\"${ADMIN_USER}\",\"display_name\":\"Quickstart Administrator\",\"password\":\"${ADMIN_PASSWORD}\",\"roles\":[\"manage-admin-users\",\"manage-realms\",\"manage-realm-subscribers\",\"view-cluster-status\"]}")"

case "$created" in
  201) echo "  ${ADMIN_USER}" ;;
  409)
    # The password generated a moment ago is not the one that works, and saying so beats
    # printing something that will not log in.
    echo "  ${ADMIN_USER} - already there, keeping the password it has"
    GENERATED_PASSWORD="kept"
    ;;
  *)
    echo "Could not create the administrator: HTTP ${created}" >&2
    exit 1
    ;;
esac

CONSOLE_NOTE="No console served. docker/up.sh --console serves one that has been built."
[[ "$SERVE_CONSOLE" == "yes" ]] && CONSOLE_NOTE="Console at http://127.0.0.1:${ATHENA_API_PORT}/ from ${ATHENA_CONSOLE_DIR}"

cat <<MSG

AthenaSIP is up, on ${PUBLIC_ADDRESS}.

  SIP        udp/tcp ${PUBLIC_ADDRESS}:${ATHENA_SIP_PORT}    tls ${PUBLIC_ADDRESS}:${ATHENA_TLS_PORT}
  WSS        wss://${PUBLIC_ADDRESS}:${ATHENA_WSS_PORT}
  Admin API  http://127.0.0.1:${ATHENA_API_PORT}/api/v1
  TURN       ${PUBLIC_ADDRESS}:3478, secret change-me-turn-secret

  Realm      ${REALM}
  Accounts   $(cut -d, -f1 "$HERE/accounts.csv" | tail -n +2 | tr '\n' ' ')

MSG

if [[ "$GENERATED_PASSWORD" == "yes" ]]; then
  cat <<MSG
  Administrator  ${ADMIN_USER} / ${ADMIN_PASSWORD}

  Written down now or not at all: it is not stored anywhere this script can read it
  back, and the API never hands a password out. If you lose it, either use the
  configuration token or run:

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

Change these before anything you do not control can reach it: the API tokens in
docker/config.yaml.template, and static-auth-secret in docker/turnserver.conf.

  docker compose logs -f athenasip     what the node is doing
  docker/up.sh down                    stop, keeping the data
  docker/up.sh --reset                 stop and forget it
MSG
