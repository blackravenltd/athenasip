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
#   test/interop/up.sh          bring it up and provision it
#   test/interop/up.sh down     take it down
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

export ATHENA_INTEROP_SIP_PORT="${ATHENA_INTEROP_SIP_PORT:-5060}"
export ATHENA_INTEROP_TLS_PORT="${ATHENA_INTEROP_TLS_PORT:-5061}"
export ATHENA_INTEROP_WS_PORT="${ATHENA_INTEROP_WS_PORT:-8088}"
export ATHENA_INTEROP_API_PORT="${ATHENA_INTEROP_API_PORT:-8080}"
export ATHENA_INTEROP_RTP_MIN="${ATHENA_INTEROP_RTP_MIN:-22000}"
export ATHENA_INTEROP_RTP_MAX="${ATHENA_INTEROP_RTP_MAX:-22100}"
export ATHENA_INTEROP_NAME="${ATHENA_INTEROP_NAME:-athenasip-interop}"

PUBLIC_ADDRESS="${ATHENA_INTEROP_PUBLIC_ADDRESS:-127.0.0.1}"
API="http://127.0.0.1:${ATHENA_INTEROP_API_PORT}/api/v1"
ADMIN_TOKEN="interop-admin"

# The realm is named for the domain a client puts in its From and To, which for a
# fixture on loopback is the address it dialled. A realm named anything else is one the
# registrar will not find (RFC 3261 10.3 step 2).
REALM="${ATHENA_INTEROP_REALM:-${PUBLIC_ADDRESS}}"

compose() { docker compose -p "${ATHENA_INTEROP_NAME}" -f "$HERE/docker-compose.yml" "$@"; }

if [[ "${1:-up}" == "down" ]]; then
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

# The node binds what the host publishes, so the config is rendered rather than fixed.
mkdir -p "$HERE/generated"
sed -e "s|@PUBLIC_ADDRESS@|${PUBLIC_ADDRESS}|g" \
    -e "s|@SIP_PORT@|${ATHENA_INTEROP_SIP_PORT}|g" \
    -e "s|@TLS_PORT@|${ATHENA_INTEROP_TLS_PORT}|g" \
    -e "s|@WS_PORT@|${ATHENA_INTEROP_WS_PORT}|g" \
    -e "s|@API_PORT@|${ATHENA_INTEROP_API_PORT}|g" \
    -e "s|@RTP_MIN@|${ATHENA_INTEROP_RTP_MIN}|g" \
    -e "s|@RTP_MAX@|${ATHENA_INTEROP_RTP_MAX}|g" \
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

cat <<MSG

Ready. The node is listening on 127.0.0.1:

  udp   ${ATHENA_INTEROP_SIP_PORT}
  tcp   ${ATHENA_INTEROP_SIP_PORT}
  tls   ${ATHENA_INTEROP_TLS_PORT}    verify against tls/ca/snakeca.crt
  ws    ${ATHENA_INTEROP_WS_PORT}    any path; /ws is what most clients ask for
  api   ${ATHENA_INTEROP_API_PORT}    bearer interop-admin / interop-client

Accounts are 1001, 1002 and 1003 in realm ${REALM}, password athenaphone.

  docker logs -f ${ATHENA_INTEROP_NAME}     what the node is doing
  test/interop/up.sh down                   stop it
MSG
