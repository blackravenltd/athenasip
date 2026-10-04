#!/usr/bin/env bash
#
# AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# A browser calls a browser through this node and rtpengine.
#
#   test/interop/browser.sh          both phases, then take the fixture down
#   test/interop/browser.sh --keep   leave the fixture up after the last phase
#   test/interop/browser.sh --direct only the direct phase
#   test/interop/browser.sh --relay  only the relay phase
#
# Two headless Chromium contexts open the admin client's softphone, register, call each
# other and read their media counters; rtpengine's counters confirm it from the other
# side.
#
# The page and the Playwright spec live in ../athenasip-admin. This script owns the node,
# the engine, the subscribers and the environment that tells the spec where they are.
#
# Every ATHENA_INTEROP_* variable up.sh takes is passed through. Effective values are
# read back from generated/fixture.env.
#
# Two phases, with the fixture rebuilt between them, because no one advertised address is
# reachable both from the host and from coturn:
#
#   direct  the engine advertises the public address and the browsers reach it
#           themselves, with no ICE servers.
#   relay   the engine advertises its address on the fixture network, which the browsers
#           cannot reach, so media must go through TURN: the credential this node mints,
#           coturn accepting it, and the relay reaching the engine.
#
# The spec tells the phase by whether ATHENA_INTEROP_RTPENGINE_ADVERTISE differs from
# ATHENA_INTEROP_PUBLIC_ADDRESS.
#
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

KEEP="no"
RUN_DIRECT="yes"
RUN_RELAY="yes"

for argument in "$@"; do
  case "$argument" in
    --keep)   KEEP="yes" ;;
    --direct) RUN_RELAY="no" ;;
    --relay)  RUN_DIRECT="no" ;;
    *) echo "Unknown argument: $argument" >&2; exit 1 ;;
  esac
done

ADMIN_REPO="${ATHENA_INTEROP_ADMIN_REPO:-$HERE/../../../athenasip-admin}"

if [[ ! -d "$ADMIN_REPO" ]]; then
  echo "No admin client checkout at ${ADMIN_REPO} - set ATHENA_INTEROP_ADMIN_REPO." >&2
  exit 1
fi

ADMIN_REPO="$(cd "$ADMIN_REPO" && pwd)"
export ATHENA_INTEROP_ADMIN_DIR="${ATHENA_INTEROP_ADMIN_DIR:-$ADMIN_REPO/build}"

# Check everything needed from the other repository before any container starts.
missing=()
[[ -f "$ATHENA_INTEROP_ADMIN_DIR/softphone.html" ]] || missing+=("the softphone page is not built - (cd ${ADMIN_REPO} && npm run build)")
[[ -d "$ADMIN_REPO/node_modules/@playwright/test" ]] || missing+=("Playwright is not installed - (cd ${ADMIN_REPO} && npm install)")

# Playwright keeps its browser in a cache outside both repositories.
playwright_cache="${PLAYWRIGHT_BROWSERS_PATH:-$HOME/Library/Caches/ms-playwright}"
[[ -d "$HOME/.cache/ms-playwright" && ! -d "$playwright_cache" ]] && playwright_cache="$HOME/.cache/ms-playwright"
compgen -G "${playwright_cache}/chromium-*" > /dev/null || missing+=("Chromium is not downloaded - (cd ${ADMIN_REPO} && npx playwright install chromium)")

if (( ${#missing[@]} > 0 )); then
  echo "Cannot run the browser call:" >&2
  printf '  %s\n' "${missing[@]}" >&2
  exit 1
fi

cleanup() {
  if [[ "$KEEP" == "yes" ]]; then
    echo
    echo "Fixture left up. ATHENA_INTEROP_NAME=${ATHENA_INTEROP_NAME:-athenasip-interop} test/interop/up.sh down"
    return
  fi

  echo
  echo "Taking the fixture down..."
  "$HERE/up.sh" down > /dev/null
}

trap cleanup EXIT

# Both phases bring up the same fixture; only the address the engine advertises differs.
run_phase() {
  local phase="$1" advertise="$2"

  echo
  echo "=== ${phase} ==="
  echo "Bringing the fixture up..."

  ATHENA_INTEROP_RTPENGINE_ADVERTISE="$advertise" "$HERE/up.sh" --rtpengine --admin

  # The fixture's effective values, re-read every phase. The spec asserts the browsers'
  # media went to the address up.sh detected.
  set -a
  # shellcheck disable=SC1091
  source "$HERE/generated/fixture.env"
  set +a

  echo
  echo "Calling ${ATHENA_INTEROP_SUBSCRIBERS:-1001,1002} on ${ATHENA_INTEROP_REALM}, media through ${ATHENA_INTEROP_MEDIA_ENGINE} to ${ATHENA_INTEROP_RTPENGINE_ADVERTISE}..."
  echo

  (cd "$ADMIN_REPO" && npm run test:e2e)
}

# The engine's address on the fixture network, which the relay phase advertises.
RELAY_ADVERTISE="${ATHENA_INTEROP_RTPENGINE_ADDRESS:-172.32.0.30}"

if [[ "$RUN_DIRECT" == "yes" ]]; then
  # Empty: up.sh falls back to the public address, which is the direct case.
  run_phase "Direct media" ""
fi

if [[ "$RUN_RELAY" == "yes" ]]; then
  run_phase "Relayed media, through coturn" "$RELAY_ADVERTISE"
fi
