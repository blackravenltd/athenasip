# Interop fixture

A node for a real SIP client to be pointed at, so that "does AthenaSIP work with X" is
something somebody can run rather than something somebody believes.

```bash
test/interop/up.sh                      # build, start, provision
test/interop/up.sh --rtpengine          # the same, with rtpengine on the media path
test/interop/up.sh --rtpengine --admin  # and serve the admin client's softphone
test/interop/up.sh down                 # stop
```

It needs Docker and nothing else. The image is the one the end-to-end harness builds,
so the first run takes a while and the rest take seconds. Running `up.sh` again replaces
whatever was there.

## Prove the fixture before blaming the client

```bash
test/interop/smoke.py
```

A dependency-free client that registers on every transport the fixture exposes: one
REGISTER, the 401 that answers it, the Digest that answers that, and the 200 that ends
it. If this passes and a real client does not, the difference is in the client. If it
fails, the fixture is not serving what it claims and nothing else is worth debugging.

```
  udp  ok  200
  tcp  ok  200
  tls  ok  200
  ws   ok  200
```

## What it is

One node, published on loopback by default, with everything in process: `memory://`,
`local://` and the built-in relay. Nothing here is testing Redis or a broker, and a
fixture that needs them fails for reasons that have nothing to do with SIP.

| | Port | Notes |
|---|---|---|
| UDP | 5060 | |
| TCP | 5060 | |
| TLS | 5061 | Verify against `tls/ca/snakeca.crt` |
| WS | 8088 | Any path. `/ws` is what most clients ask for |
| Admin API | 8080 | Sign in as `ATHENA_INTEROP_API_USER` with `ATHENA_INTEROP_API_PASSWORD`, from `generated/fixture.env` |

Subscribers `1001`, `1002` and `1003` live in realm `127.0.0.1` with the password
`athenaphone`. The realm is named for the domain a client puts in its From and To,
which on loopback is the address it dialled: a realm named anything else is one the
registrar will not find (RFC 3261 10.3).

## Running it beside another fixture

Those ports are the ones AthenaPhone's harness already uses for its Asterisk fixture,
on purpose, so that one client can be pointed at either without being rewritten. That
also means only one of them can hold the ports at a time. Move this one to run both:

```bash
ATHENA_INTEROP_SIP_PORT=15060 ATHENA_INTEROP_TLS_PORT=15061 \
ATHENA_INTEROP_WS_PORT=18088  ATHENA_INTEROP_API_PORT=18080 \
ATHENA_INTEROP_RTP_MIN=23000  ATHENA_INTEROP_RTP_MAX=23020 \
ATHENA_INTEROP_NAME=athenasip-interop-alt test/interop/up.sh

test/interop/smoke.py --sip-port 15060 --tls-port 15061 --ws-port 18088
```

The container binds the same number the host publishes, never a translation: a node
writes its own local port into Via and Record-Route, so a mapping that moved the port
would tell a client to come back to one nothing is listening on. It is the same reason
`public_address` is set at all: the node is behind a published port and has to be told
the address a client reaches it on.

## Only one WebSocket listener

A node has one `websocket` section, so it serves `ws` or `wss` and not both. This
fixture serves `ws`, because it is the signalling fixture and a client that cannot be
handed a CA cannot verify `wss` anyway. Swap `tls: true` and the two filenames in
`config.yaml.template` to test the secure listener instead.

## Media

The default is the in-process relay: plain RTP, which is what an ordinary SIP softphone
on UDP, TCP or TLS expects. A client whose media is always WebRTC, DTLS-SRTP with ICE,
will register and signal cleanly against it and then fail to agree on media. That is
the honest result rather than one arranged by configuration.

```bash
test/interop/up.sh --rtpengine
```

is the answer to it, and it is what a browser or an AthenaPhone needs. Two things
change with it, both of them about addresses rather than about SIP:

- rtpengine is told `--interface=<its own address>!<this host's address>`, so it binds
  the address it has inside Docker and writes into the description the one the client
  can actually send to. Its media port range is published to the host, because an
  address a packet cannot arrive on is no better than the container's own.
- the fixture moves off loopback. `up.sh` works out this host's address on its own
  network and advertises that, and publishes every port on `0.0.0.0` rather than on
  `127.0.0.1`, because a phone reaching the fixture from a device is not on loopback.
  `ATHENA_INTEROP_PUBLIC_ADDRESS` overrides the address and `ATHENA_INTEROP_BIND` the
  interface it is published on, and the realm is named after the address, so with
  rtpengine the subscribers live in a realm named for the LAN address rather than
  `127.0.0.1`.

Each leg is profiled from what it has said rather than from the transport it signals
over, so a realm can hold a WebRTC client and a plain-RTP one and answer each of them
in its own terms. What is still undecided is the very first offer this node sends
*towards* an endpoint it has never heard describe itself, and only when the transport
misleads - a WebRTC endpoint on UDP, TCP or TLS. A realm can settle it:

```bash
source test/interop/generated/fixture.env
TOKEN=$(curl -s -X POST http://127.0.0.1:8080/api/v1/auth/login -H 'Content-Type: application/json' \
  -d "{\"username\":\"$ATHENA_INTEROP_API_USER\",\"password\":\"$ATHENA_INTEROP_API_PASSWORD\"}" |
  sed -n 's/.*"token":"\([0-9a-f]*\)".*/\1/p')
curl -X PUT http://127.0.0.1:8080/api/v1/realms/<realm> \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"behaviour":{"media_profile":"webrtc"}}'
```

`{"behaviour":{"media_anchor":false}}` is the other knob: it leaves every description untouched and
lets the two endpoints talk directly, which is the cheapest way to prove signalling and
ICE with no engine at all.

## Serving the admin client

`--admin` mounts the admin client's build read-only at `/admin` and turns `http.files`
on, so the node serves the console - and its softphone, which is the browser end of the
Milestone 3 harness. The build comes from `../athenasip-admin` and is mounted rather
than copied here, because a bundle checked into this tree is one that goes stale;
`ATHENA_INTEROP_ADMIN_DIR` points it somewhere else. Without a build to serve, `up.sh`
says so and stops rather than starting a node that serves nothing.

```bash
(cd ../athenasip-admin && npm run build)
test/interop/up.sh --rtpengine --admin
```

Open it over **loopback** - `http://127.0.0.1:8080/` - whatever address the node
advertises. `localhost` is a secure origin and `getUserMedia` needs one, so a page
opened at the LAN address cannot reach a microphone without turning Chrome's origin
checks off. The SIP WebSocket and the media still go wherever the node says; only the
page has to come from loopback.

## The browser call

```bash
test/interop/browser.sh          # up, run the spec, down
test/interop/browser.sh --keep   # leave the fixture up afterwards
```

Two headless Chromium contexts open the softphone, register, call each other through
rtpengine and read their own media counters back, while the engine's counters say the
same thing from the other side. The page and the Playwright spec live in
`../athenasip-admin`, because the page is the admin client's own softphone and a test
belongs beside what it tests; this script owns the node, the engine, the subscribers and
the environment that tells the spec where they are. The spec never brings the fixture
up - what is under test is the node, not a harness's arrangement of it.

It checks what the other repository owes it before starting a container, and says how
to fix each thing it is missing. The effective values, including the LAN address
`up.sh` detected, are passed through from `generated/fixture.env`, which `up.sh` writes
every time it runs.

## The live call

`UAT.md` is the runbook for the one thing nothing can automate: a browser calling an
AthenaPhone on a real device, with a person listening. It carries the record to fill
in, which is the first row of the interop matrix.

`media-stats.py` is what it reads the media with:

```bash
test/interop/media-stats.py           # every call the engine is holding
test/interop/media-stats.py --watch   # the same, once a second, during a call
```

The engine's own counters are the only thing that says media moved rather than that a
call was signalled: a description the engine declined travels on untouched and the two
endpoints reach each other directly, which looks identical from outside. It speaks
rtpengine's ng protocol and needs nothing installed; the control port is published on
loopback, and only ever on loopback, whatever the media is published on.

## When something fails

```bash
docker logs -f athenasip-interop
```

Every message in and out is logged with its first line, and the registrar and the proxy
say why they answered what they did.
