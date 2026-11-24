#!/bin/sh
#
# AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# One step of the checks, the same in GitHub's runner and in a local debian:trixie container:
#
#   tools/ci.sh deps       the packages, Debian 13's Boost 1.83 among them
#   tools/ci.sh check      the header block on every source
#   tools/ci.sh release    g++, warnings as errors, the unit suite
#   tools/ci.sh clang      clang, the unit suite
#   tools/ci.sh asan       AddressSanitizer and UndefinedBehaviourSanitizer
#   tools/ci.sh tsan       ThreadSanitizer
#   tools/ci.sh coverage   an lcov report of src/ in build-ci-coverage/coverage.info
#   tools/ci.sh fuzz       every fuzz target for FUZZ_SECONDS (default 60) each, with ASan and UBSan
#   tools/ci.sh console    the console's typecheck, unit tests and build
#
# JOBS sets the parallelism (default: every core). docs/testing.md has the rest.
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JOBS="${JOBS:-$(nproc 2>/dev/null || sysctl -n hw.ncpu)}"
cd "$ROOT"

deps() {
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq --no-install-recommends build-essential ca-certificates clang cmake git lcov libgtest-dev libgmock-dev \
    libboost-dev libboost-thread-dev libboost-json-dev liblua5.4-dev libnghttp2-dev libssl-dev libyaml-cpp-dev mosquitto pkg-config \
    redis-server >/dev/null
}

check() {
  missing=0
  for file in $(git ls-files 'src/*.cpp' 'src/*.h' 'tests/*.cpp' 'tests/*.h'); do
    if ! head -n 6 "$file" | grep -q 'Copyright (C)'; then
      echo "no header block: $file"
      missing=1
    fi
  done
  [ "$missing" -eq 0 ]
}

# Builds the test binary in build-ci-<name> with the given CMake arguments and runs it against a Redis and a broker
# of its own, so the two canonical drivers are tested and nothing else on the machine is touched.
suite() {
  name="$1"
  shift
  build="$ROOT/build-ci-$name"
  cmake -S . -B "$build" -DATHENA_BUILD_TESTING=ON "$@" >/dev/null
  cmake --build "$build" -j"$JOBS"
  redis-server --port 6399 --save '' --daemonize yes --dir "$build" >/dev/null
  mosquitto -p 1899 -d >/dev/null 2>&1
  sleep 1
  status=0
  ATHENA_TEST_REDIS_URL=redis://127.0.0.1:6399 ATHENA_TEST_MQTT_URL=mqtt://127.0.0.1:1899 "$build/athenasip_tests" || status=$?
  redis-cli -p 6399 shutdown nosave >/dev/null 2>&1 || true
  pkill -f 'mosquitto -p 1899' || true
  return "$status"
}

case "${1:-}" in
  deps) deps ;;
  check) check ;;
  release) suite release -DCMAKE_BUILD_TYPE=Release -DATHENA_WERROR=ON ;;
  clang) suite clang -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ ;;
  asan) suite asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DATHENA_SANITIZE=address ;;
  tsan) suite tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DATHENA_SANITIZE=thread ;;
  coverage)
    suite coverage -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=--coverage -DCMAKE_EXE_LINKER_FLAGS=--coverage
    lcov --quiet --capture --directory "$ROOT/build-ci-coverage" --include "$ROOT/src/*" --output-file "$ROOT/build-ci-coverage/coverage.info" \
      --ignore-errors mismatch,inconsistent,negative
    lcov --summary "$ROOT/build-ci-coverage/coverage.info" --ignore-errors inconsistent
    ;;
  fuzz)
    build="$ROOT/build-ci-fuzz"
    cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=clang++ -DATHENA_SANITIZE=address -DATHENA_BUILD_FUZZING=ON >/dev/null
    cmake --build "$build" -j"$JOBS"
    for target in "$build"/athenasip_fuzz_*; do
      mkdir -p "$build/corpus/$(basename "$target")"
      "$target" -max_total_time="${FUZZ_SECONDS:-60}" -max_len=8192 -print_final_stats=1 "$build/corpus/$(basename "$target")" 2>&1 | tail -n 4
    done
    ;;
  console) (cd console && npm ci --no-audit --no-fund && npm run typecheck && npm test && npx vite build) ;;
  *)
    echo "usage: tools/ci.sh deps|check|release|clang|asan|tsan|coverage|fuzz|console" >&2
    exit 2
    ;;
esac
