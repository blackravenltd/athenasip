# AthenaSIP - Testing

Principle 2 of the plan is that compliance is proven rather than asserted. That means
five layers, and each one answers a question the one before it cannot:

| | What it answers | Cost |
|---|---|---|
| [Unit tests](#unit-tests) | does the code do what the RFC says | seconds |
| [Sanitizers](#sanitizers) | is it memory- and thread-safe under the same tests | minutes, and not routine - see below |
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

**Not a routine step.** Tom's instruction of 2026-09-30: they are for tracing a fault that
cannot be pinned down otherwise. Reaching for one in place of understanding the code is the
habit being avoided, not the tool. `CLAUDE.md` still carries the older rule - run them
before touching the transaction, channel or media paths - and the two do not agree; this is
the live one until that is changed to match.

What does the work instead is reasoning about lifetimes and threading directly, and
exercising a change against a running node. Nearly every bug in the work since 0.7.0 was
found by running something rather than by a checker.

ASan is ASan plus UBSan. Both take the same environment variables as the plain run, and a
build interrupted partway leaves a stale object behind - check the test counts agree across
build directories before believing a green run.

With GoogleTest from Homebrew, ASan reports a `container-overflow` inside GoogleTest's own
filter parsing before any test runs. It is a false positive - the library is not built with
ASan, and container annotations only agree when everything is - so turn that one check off:
`ASAN_OPTIONS=detect_container_overflow=0 ./build-asan/athenasip_tests`.

## The sipp harness

Eleven scenarios against one node in Docker, driven by real sipp clients.

```bash
test/e2e/run.sh                 # the in-process relay on the media path
test/e2e/run.sh --rtpengine     # the same, with a real rtpengine
test/e2e/run.sh register        # only scenarios whose name contains "register"
```

The rtpengine run adds a twelfth check the builtin run cannot make: it reads the engine's
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
subscribers.

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

### The relay

`--rtpengine` now brings up coturn as well, so the fixture can exercise the TURN path and not
only direct media. What that adds is the whole of it: the credential this node mints, coturn
accepting it, and the relay reaching the engine.

It needs two runs, and that is forced rather than chosen. No single advertised address serves
both cases: from the host the engine is reachable on its published ports at loopback, from
coturn only at its address on the fixture network, and nothing is both without putting the
fixture on the LAN. So the direct case runs with the engine advertising the public address,
and the relay case with `ATHENA_INTEROP_RTPENGINE_ADVERTISE` set to
`ATHENA_INTEROP_RTPENGINE_ADDRESS`, restarting between them.

What the fixture provides for the relay case, all of it in `generated/fixture.env`:

| | |
|---|---|
| `ATHENA_INTEROP_TURN_PORT` | where coturn listens on the host |
| `ATHENA_INTEROP_TURN_MIN`, `_MAX` | the relay range, which is what a relayed pair's local port is inside |
| `ATHENA_INTEROP_TURN_SECRET` | generated per run, so no credential outlives the fixture |
| `ATHENA_INTEROP_RTPENGINE_ADVERTISE` | what the engine wrote into the session description |

`GET /api/v1/client/config` serves the STUN and TURN servers with a credential, and serves an
empty `ice_servers` without `--rtpengine`, because telling a client about a TURN server that
is not running leaves it worse off than telling it about none.

The spec asserts the relayed pair by the local port being inside that range rather than by
`candidateType`: Chrome reports `prflx` for a relayed local candidate once connectivity
checks run, so the label is the browser's opinion and the port is a fact about our
configuration. The query-string contract by which the spec hands the page its ICE servers is
documented with the page, in `../athenasip-admin`, per the decision of 2026-09-23.

`browser.sh` runs both phases and takes the fixture down after; `--direct` and `--relay` run
one of them when iterating.

Both passing, 2026-09-30. What the relay phase's record holds, which is what makes it a
relayed call rather than a call that happened to work: local ports 22326 and 22312, both
inside coturn's configured range, against a remote address of `172.32.0.30` - the engine on
the fixture network, which the browsers have no route to. Pair succeeded, DTLS connected,
109 and 112 packets each way, `totalAudioEnergy` 0.35 and 0.43. `candidateType` reads
`prflx`, which is why the assertion is on the port.

It was proven by hand first, against the quickstart stack, and that run found a real bug - a
TURN username carrying our own log prose - which is the argument for having it in the
automated layer rather than in somebody's memory of an evening.

## The live call

The one thing nothing can automate: a browser calls an AthenaPhone on a real device and
a person hears it. `test/interop/UAT.md` is the runbook and carries the record to fill
in, which is the first row of the interop matrix.

What it adds over the browser call is exactly one thing - AthenaPhone's signalling
transport - because its media is the same WebRTC over any transport it signals on.

## No hosted CI

There is none and there will not be. These run where the code is written.
