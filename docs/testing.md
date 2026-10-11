# AthenaSIP - Testing

| Layer | What it answers | Needs |
|---|---|---|
| [Unit tests](#unit-tests) | does the code do what the RFC says | a test build |
| [Sanitizers](#sanitizers) | is it memory- and thread-safe under the same tests | not routine, see below |
| [sipp harness](#the-sipp-harness) | does a real client on a real socket agree | Docker |
| [Interop fixture](#the-interop-fixture) | does a particular SIP client work, on every transport | Docker |
| [Browser call](#the-browser-call) | does a browser agree, with media through rtpengine | Docker, `../athenasip-admin` |
| [Live call](#the-live-call) | did a person hear it | a device |
| [The whole suite](#the-whole-suite) | do the node, the console and AthenaPhone agree | Docker, the sibling repositories |

Everything runs on a developer machine. There is no hosted CI.

## Before a release

All of these pass on `develop` before it is merged to `main` and tagged:

```bash
cmake --preset tests && cmake --build build-tests -j8
ATHENA_TEST_REDIS_URL=redis://127.0.0.1:6399 \
ATHENA_TEST_MQTT_URL=mqtt://127.0.0.1:1883 ./build-tests/athenasip_tests

cmake --preset asan && cmake --build build-asan -j8 && ./build-asan/athenasip_tests
cmake --preset tsan && cmake --build build-tsan -j8 && ./build-tsan/athenasip_tests

test/e2e/run.sh
test/e2e/run.sh --rtpengine
test/e2e/run.sh --lua
test/e2e/cluster.sh --lua
test/e2e/trunk.sh
test/suite/run.sh
```

## The whole suite

```bash
test/suite/run.sh              # everything that runs unattended
test/suite/run.sh --no-sipp    # without the sipp harnesses
test/suite/run.sh --device     # also what needs a phone and a person
```

It runs the unit tests and the sipp harnesses, then brings the interop fixture up twice,
with direct media and relayed through coturn, and runs each sibling project's suite against
it: `npm run test:athenasip` in `../athenasip-admin` and `../athenaphone`
(`ATHENA_SUITE_ADMIN_REPO` and `ATHENA_SUITE_PHONE_REPO` move them). Results, with each
project's `summary.json` and the machine's load every ten seconds (`load.log`), are under `test/suite/results/`.

A project's script reads the fixture from `ATHENA_INTEROP_*` (`generated/fixture.env`),
the phase from `ATHENA_SUITE_PHASE` and where to write from `ATHENA_SUITE_RESULTS`; it
never starts or stops a container, except the node through `ATHENA_SUITE_RESTART_CMD`; and
it runs device tests only when `ATHENA_SUITE_DEVICE=1`. A project without the script is
reported, not failed.

## Unit tests

GoogleTest under `tests/`, mirroring the `src/` layout.

```bash
cmake --preset tests && cmake --build build-tests -j8
./build-tests/athenasip_tests
./build-tests/athenasip_tests --gtest_filter='ProxyMediaTest.*'   # one suite
```

Run the binary, not `ctest`: ctest starts a process per test and takes minutes to do
what the binary does in seconds.

The Redis and MQTT suites skip silently without a server. Set both variables, or the two
drivers go untested:

```bash
redis-server --port 6399 --save '' --daemonize yes
mosquitto -p 1883 -d

ATHENA_TEST_REDIS_URL=redis://127.0.0.1:6399 \
ATHENA_TEST_MQTT_URL=mqtt://127.0.0.1:1883 ./build-tests/athenasip_tests
```

Every test runs against the `builtin://` policy. `ATHENA_TEST_POLICY=lua` runs them
against the standard Lua scripts instead, which must pass the same tests:
`PolicyEquivalenceTest` does that for the proxy, registrar, dialog and core suites in a
child process, and feeds both drivers the same requests to compare their decisions. A
change to `src/policy/builtin_policy.cpp` or `scripts/athenasip/standard.lua` needs the
same change in the other.

### Writing one

Tests come from the RFCs and the SIP concepts, never from the current behaviour. Write
the test for what the standard requires, watch it fail, then fix the code.

Assert facts about this node's own configuration, never a client's labels for them. A
relayed browser candidate is recognised by its port lying in coturn's range, because
Chrome reports it as `prflx` once connectivity checks run.

Timers are injectable: use `ManualTimerSource` and advance it rather than waiting on a
real clock. RFC 3261 timer B is 64*T1, 32 seconds of real time.

## Sanitizers

```bash
cmake --preset asan && cmake --build build-asan -j8 && ./build-asan/athenasip_tests
cmake --preset tsan && cmake --build build-tsan -j8 && ./build-tsan/athenasip_tests
```

`asan` is AddressSanitizer plus UndefinedBehaviourSanitizer; `tsan` is ThreadSanitizer.
They are for a release check or for hunting a fault that cannot be pinned down any other
way, not for every change. Day to day, reason about lifetimes and threading and run the
change against a node.

- They take the same `ATHENA_TEST_*` variables as the plain run.
- An interrupted build can leave a stale object behind. Check the test count matches the
  `build-tests` run before believing a green result.
- With Homebrew's GoogleTest, ASan reports a false `container-overflow` inside
  GoogleTest before any test runs, because the library is not built with ASan. Run with
  `ASAN_OPTIONS=detect_container_overflow=0`.
- UBSan's `vptr` check fails on Boost.JSON's memory resource, which comes from the
  uninstrumented Boost library. Suppress it:

  ```bash
  printf 'vptr:boost/json/*\nvptr:boost/container/*\nvptr:*memory_resource*\n' > /tmp/ubsan.supp
  UBSAN_OPTIONS=suppressions=/tmp/ubsan.supp ASAN_OPTIONS=detect_container_overflow=0 ./build-asan/athenasip_tests
  ```

## The sipp harness

sipp clients against a node in Docker, asserting what the RFCs require.

```bash
test/e2e/run.sh                      # one node, built-in relay
test/e2e/run.sh --rtpengine          # the same, with rtpengine on the media path
test/e2e/run.sh register             # only scenarios whose name contains "register"
test/e2e/cluster.sh                  # two nodes, one Redis, one broker
test/e2e/run.sh --lua                # any of them on the standard Lua scripts
test/e2e/trunk.sh                    # trunks to sipp as a carrier
```

`--lua` runs any of them with the node on the standard Lua scripts instead of
`builtin://`, and `test/e2e/trunk.sh` runs trunks against sipp as a carrier: registering,
calling out through a challenge, failing over to a second trunk, and a call in.

Logs land in `test/e2e/results/`. [`test/e2e/README.md`](../test/e2e/README.md) has the
scenarios, what each proves and how to add one.

## The interop fixture

A node to point a real SIP client at.

```bash
test/interop/up.sh                      # loopback, built-in relay
test/interop/up.sh --rtpengine          # this host's LAN address, rtpengine and coturn
test/interop/up.sh --rtpengine --admin  # and serve the admin client's softphone
test/interop/up.sh down

test/interop/smoke.py --host <address>  # REGISTER on udp, tcp, tls and ws
test/interop/media-stats.py --watch     # rtpengine's counters during a call
```

Run `smoke.py` before blaming a client: if it passes and the client does not, the
difference is in the client. [`test/interop/README.md`](../test/interop/README.md) has
the ports, the subscribers and the environment variables.

## The browser call

```bash
test/interop/browser.sh          # both phases, then take the fixture down
test/interop/browser.sh --keep   # leave the fixture up afterwards
test/interop/browser.sh --direct # only direct media
test/interop/browser.sh --relay  # only media relayed through coturn
```

Two headless Chromium contexts register, call each other through rtpengine and check
their own media counters. The page and the Playwright spec live in `../athenasip-admin`;
`browser.sh` owns the fixture and passes its environment to the spec. See
[`test/interop/README.md`](../test/interop/README.md#the-browser-call).

`e2e/phone-call.spec.ts` in `../athenasip-admin` is the same browser calling a real
phone, with video, through whatever node it is pointed at. Somebody has to answer the
phone.

## The live call

A browser calls a SIP phone on a real device and a person listens.
[`test/interop/UAT.md`](../test/interop/UAT.md) is the script.
