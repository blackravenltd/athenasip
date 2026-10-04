# AthenaSIP - Testing

| Layer | What it answers | Needs |
|---|---|---|
| [Unit tests](#unit-tests) | does the code do what the RFC says | a test build |
| [Sanitizers](#sanitizers) | is it memory- and thread-safe under the same tests | not routine, see below |
| [sipp harness](#the-sipp-harness) | does a real client on a real socket agree | Docker |
| [Interop fixture](#the-interop-fixture) | does a particular SIP client work, on every transport | Docker |
| [Browser call](#the-browser-call) | does a browser agree, with media through rtpengine | Docker, `../athenasip-admin` |
| [Live call](#the-live-call) | did a person hear it | a device |

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
```

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

### Writing one

Tests come from the RFCs and the SIP concepts, never from the current behaviour. Write
the test for what the standard requires, watch it fail, then fix the code.

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

## The sipp harness

sipp clients against a node in Docker, asserting what the RFCs require.

```bash
test/e2e/run.sh                      # one node, built-in relay
test/e2e/run.sh --rtpengine          # the same, with rtpengine on the media path
test/e2e/run.sh register             # only scenarios whose name contains "register"
test/e2e/cluster.sh                  # two nodes, one Redis, one broker
```

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
