#!/usr/bin/env bash
#
# AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# A browser calls a browser through this node and rtpengine.
#
#   test/interop/browser.sh          bring the fixture up, run the spec, take it down
#   test/interop/browser.sh --keep   leave the fixture up afterwards
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
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

KEEP="no"
[[ "${1:-}" == "--keep" ]] && KEEP="yes"

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

# One code path for bringing the node up: this is the same fixture a person points a
# real client at, with the client served and rtpengine on the media path.
echo "Bringing the fixture up..."
"$HERE/up.sh" --rtpengine --admin

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

# What the fixture turned out to be, rather than what was asked for. The LAN address in
# particular is detected by up.sh, and the spec asserts the browsers' media went to it.
set -a
# shellcheck disable=SC1091
source "$HERE/generated/fixture.env"
set +a

echo
echo "Calling ${ATHENA_INTEROP_ACCOUNTS:-1001,1002} on ${ATHENA_INTEROP_REALM}, media through ${ATHENA_INTEROP_MEDIA_ENGINE}..."
echo

(cd "$ADMIN_REPO" && npm run test:e2e)
