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
# The first Milestone 3 "to the first call" item. Two headless Chromium contexts open
# the admin client's softphone, register, call each other and read their own media
# counters back; rtpengine's counters say the same thing from the other side. Two
# witnesses to the same media, and no device, no Metro and no AthenaPhone involved.
#
# The page and the Playwright spec live in ../athenasip-admin, because the page is the
# admin client's own softphone (decision of 2026-09-23 in TODO/ACTIVE.md) and a test
# belongs beside what it tests. This script owns the other half: the node, the engine,
# the accounts, and the environment that tells the spec where they are. The spec never
# brings the fixture up, so what is under test is this node rather than a harness's
# arrangement of it.
#
# Every ATHENA_INTEROP_* variable up.sh takes works here and is passed through. The
# effective values, including the LAN address up.sh detects, are read back from the
# fixture's own generated/fixture.env rather than worked out a second time.
#
# Two phases, and the fixture is rebuilt between them because no single advertised address
# serves both: from the host the engine is reachable on its published ports at loopback,
# from coturn only at its address on the fixture network, and nothing is both without
# putting the fixture on the LAN.
#
#   direct  the engine advertises the public address, and the browsers reach it themselves.
#           No ICE servers - this is the path an ordinary client takes.
#   relay   the engine advertises its own address on the fixture network, which the browsers
#           cannot reach, so the only way through is the TURN server. It proves the whole
#           TURN path: the credential this node mints, coturn accepting it, and the relay
#           reaching the engine.
#
# The spec chooses which it is running from the environment, by whether
# ATHENA_INTEROP_RTPENGINE_ADVERTISE differs from ATHENA_INTEROP_PUBLIC_ADDRESS. Phasing
# lives here rather than in the spec, because bringing the fixture up is this side's.
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

# Everything this needs from the other repository, checked before a container starts.
# Each of these fails much later and much less clearly if it is left to be discovered.
missing=()
[[ -f "$ATHENA_INTEROP_ADMIN_DIR/softphone.html" ]] || missing+=("the softphone page is not built - (cd ${ADMIN_REPO} && npm run build)")
[[ -d "$ADMIN_REPO/node_modules/@playwright/test" ]] || missing+=("Playwright is not installed - (cd ${ADMIN_REPO} && npm install)")

# Playwright downloads its browser into a cache outside either repository, and a run
# without it fails on the first page rather than on the first assertion.
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

# One code path for bringing the node up, whichever phase it is: the same fixture a person
# points a real client at, with the client served and rtpengine on the media path. Only what
# the engine advertises differs, and the spec reads that back to know which phase it is in.
run_phase() {
  local phase="$1" advertise="$2"

  echo
  echo "=== ${phase} ==="
  echo "Bringing the fixture up..."

  ATHENA_INTEROP_RTPENGINE_ADVERTISE="$advertise" "$HERE/up.sh" --rtpengine --admin

  # What the fixture turned out to be, rather than what was asked for. The LAN address in
  # particular is detected by up.sh, and the spec asserts the browsers' media went to it.
  # Re-read every phase, because the phase is what changed it.
  set -a
  # shellcheck disable=SC1091
  source "$HERE/generated/fixture.env"
  set +a

  echo
  echo "Calling ${ATHENA_INTEROP_ACCOUNTS:-1001,1002} on ${ATHENA_INTEROP_REALM}, media through ${ATHENA_INTEROP_MEDIA_ENGINE} to ${ATHENA_INTEROP_RTPENGINE_ADVERTISE}..."
  echo

  (cd "$ADMIN_REPO" && npm run test:e2e)
}

# The engine's address on the fixture network, which is what the relay phase advertises.
# Taken from the environment so that moving the subnet moves both together.
RELAY_ADVERTISE="${ATHENA_INTEROP_RTPENGINE_ADDRESS:-172.32.0.30}"

if [[ "$RUN_DIRECT" == "yes" ]]; then
  # Empty means up.sh falls back to the public address, which is the direct case and is
  # also what the fixture does when nobody has asked for anything.
  run_phase "Direct media" ""
fi

if [[ "$RUN_RELAY" == "yes" ]]; then
  run_phase "Relayed media, through coturn" "$RELAY_ADVERTISE"
fi
