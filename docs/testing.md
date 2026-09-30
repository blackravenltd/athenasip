# AthenaSIP - Testing

Principle 2 of the plan is that compliance is proven rather than asserted. That means
five layers, and each one answers a question the one before it cannot:

| | What it answers | Cost |
|---|---|---|
| [Unit tests](#unit-tests) | does the code do what the RFC says | seconds |
| [Sanitizers](#sanitizers) | is it memory- and thread-safe under the same tests | minutes |
| [The sipp harness](#the-sipp-harness) | does a real client on a real socket agree | ~2 minutes |
| [The interop fixture](#the-interop-fixture) | does a real *client* agree, over every transport | seconds, once built |
| [The browser call](#the-browser-call) | does a browser agree, with media through rtpengine | ~30 seconds |
| [The live call](#the-live-call) | did a person hear it | manual |

Everything below runs on a developer machine and needs no hosted CI. The first four run
unattended; the last two need Docker and, for the last, a device.

## Before anything is tagged

All of these pass, in this order, on `develop`:

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
```

Run the binary rather than `ctest`. ctest launches a process per test and takes eleven
minutes to do what this does in eleven seconds; use it only when a crash makes the
one-process run unreadable.

Two suites skip silently without a server, so set both or the two canonical drivers go
untested:

```bash
redis-server --port 6399 --save '' --daemonize yes

ATHENA_TEST_REDIS_URL=redis://127.0.0.1:6399 \
ATHENA_TEST_MQTT_URL=mqtt://127.0.0.1:1883 ./build-tests/athenasip_tests
```

A filter runs one suite: `--gtest_filter='ProxyMediaTest.*'`.

### Writing one

Tests come from the RFCs, the SIP concepts and the plan, never from the current
behaviour. Write the test for what the standard requires, watch it fail, then fix the
code. Assuming the existing shape is right is how the header, URI, message and SDP bugs
survived as long as they did.

Timers are injectable: use `ManualTimerSource` and advance it rather than waiting on a
real clock. RFC 3261 timer B is 64*T1, so a real-clock test would take 32 seconds.

## Sanitizers

```bash
cmake --preset asan && cmake --build build-asan -j8 && ./build-asan/athenasip_tests
cmake --preset tsan && cmake --build build-tsan -j8 && ./build-tsan/athenasip_tests
```

Required before anything touching the transaction, channel or media paths. ASan is
ASan plus UBSan. Both take the same environment variables as the plain run.

## The sipp harness

Eight scenarios against one node in Docker, driven by real sipp clients.

```bash
test/e2e/run.sh                 # the in-process relay on the media path
test/e2e/run.sh --rtpengine     # the same, with a real rtpengine
test/e2e/run.sh register        # only scenarios whose name contains "register"
```

The rtpengine run adds a ninth check the builtin run cannot make: it reads the engine's
counters afterwards and fails when nothing was relayed. A declined description travels
on untouched and the two endpoints reach each other directly, which looks exactly like
success from outside.

`invite-timeout` takes 32 seconds by design and runs last. Logs land in
`test/e2e/results/`, one per end, plus the node's own.

`test/e2e/README.md` has the scenario table, what each one proves, and how to add one.

## The interop fixture

A node for a real SIP client to be pointed at, so that "does AthenaSIP work with X" is
something somebody can run rather than something somebody believes.

```bash
test/interop/up.sh                      # loopback, in-process relay
test/interop/up.sh --rtpengine          # off loopback, rtpengine on the media path
test/interop/up.sh --rtpengine --admin  # and serving the admin client's softphone
test/interop/up.sh down

test/interop/smoke.py --host <address>  # REGISTER on udp, tcp, tls and ws
```

Run `smoke.py` before blaming a client. If it passes and the client does not, the
difference is in the client; if it fails, the fixture is not serving what it claims and
nothing you learn from the client is about the client.

`--rtpengine` moves the fixture off loopback onto this machine's own address, because
the address a client is told to send media to has to be one it can reach. It publishes
on `0.0.0.0` and signs a TLS certificate naming that address, so do not leave it up
unattended.

`test/interop/media-stats.py` reads what the engine actually did:

```bash
test/interop/media-stats.py           # every call it is holding
test/interop/media-stats.py --watch   # the same, once a second, during a call
```

The default ports are 5060, 5061, 8088 and 8080, which are the ones AthenaPhone's own
Asterisk fixture uses on purpose so that one client can be pointed at either. Only one
of the two can hold them, so stop the other or move this one - every port is an
environment variable, and `test/interop/README.md` has the incantation, along with the
accounts.

## The browser call

Two headless Chromium contexts register, call each other through rtpengine and read
their own media counters back, while the engine's counters say the same thing from the
other side. Two witnesses to the same media.

```bash
test/interop/browser.sh          # up, run the spec, down
test/interop/browser.sh --keep   # leave the fixture up afterwards
```

The page is the admin client's softphone and the Playwright spec lives beside it in
`../athenasip-admin` (decision of 2026-09-23). `browser.sh` owns the fixture's
lifecycle - the spec never brings it up, because what is under test is the node rather
than a harness's arrangement of it - and checks what the other repository owes it
before starting a container:

```bash
(cd ../athenasip-admin && npm install && npm run build && npx playwright install chromium)
```

It passes the whole fixture environment through from `test/interop/generated/fixture.env`,
which `up.sh` writes every run, so an address or a port is never guessed twice.

This layer proves direct media. **The relay is not in it**, and was checked by hand against
the quickstart stack instead, on 2026-09-30: two Chromium contexts with
`iceTransportPolicy: "relay"` and ICE servers from `GET /api/v1/client/config` carried 399
and 402 packets each way with no loss and `totalAudioEnergy` above 2 at both ends, relayed
through coturn to rtpengine. What that adds over the direct run is the whole TURN path -
the credential this node mints, coturn accepting it, and the relay reaching the engine - and
it found a real bug on the way, a TURN username carrying our own log prose.

It is not automated yet, and the reason is that `browser.sh` drives the interop fixture,
which has no TURN server in it. Doing it properly means coturn in that fixture and a relay
address the engine can be reached at, which is `docker/up.sh --console` plus
`ATHENA_RTPENGINE_ADVERTISE` today. Until then the relay is a manual check and this says so
rather than leaving a gap that reads as covered.

## The live call

The one thing nothing can automate: a browser calls an AthenaPhone on a real device and
a person hears it. `test/interop/UAT.md` is the runbook and carries the record to fill
in, which is the first row of the interop matrix.

What it adds over the browser call is exactly one thing - AthenaPhone's signalling
transport - because its media is the same WebRTC over any transport it signals on.

## No hosted CI

There is none and there will not be. These run where the code is written.
