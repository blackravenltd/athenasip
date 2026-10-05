# AthenaSIP - Interop fixture

A node to point a real SIP client at.

```bash
test/interop/up.sh                      # build, start, provision
test/interop/up.sh --rtpengine          # the same, with rtpengine and coturn
test/interop/up.sh --rtpengine --admin  # and serve the admin client's softphone
test/interop/up.sh down                 # stop
```

It needs only Docker. Running `up.sh` again replaces whatever was there. It prints what
it settled on and writes the same values to `generated/fixture.env`.

## Prove the fixture before blaming the client

```bash
test/interop/smoke.py --host <address>
```

A dependency-free client that registers on every transport: REGISTER, 401, Digest, 200.
If it passes and a real client does not, the difference is in the client. If it fails,
fix the fixture first.

```
  udp  ok  200
  tcp  ok  200
  tls  ok  200
  ws   ok  200
```

`--host` defaults to `127.0.0.1`; `--sip-port`, `--tls-port`, `--ws-port`, `--user`,
`--password`, `--realm` and `--transports` follow a moved or changed fixture.

## What it is

One node with everything in process: `memory://`, `local://` and the built-in relay. It
is published on loopback unless `--rtpengine` is given.

| | Port | Variable | Notes |
|---|---|---|---|
| UDP, TCP | 5060 | `ATHENA_INTEROP_SIP_PORT` | |
| TLS | 5061 | `ATHENA_INTEROP_TLS_PORT` | Verify against `tls/ca/snakeca.crt` |
| WS | 8088 | `ATHENA_INTEROP_WS_PORT` | Any path |
| WSS | 8089 | `ATHENA_INTEROP_WSS_PORT` | Any path; verify against `tls/ca/snakeca.crt` |
| Admin API | 8080 | `ATHENA_INTEROP_API_PORT` | Sign in as `ATHENA_INTEROP_API_USER` with `ATHENA_INTEROP_API_PASSWORD`, both in `generated/fixture.env` |
| Media | 22000-22100 UDP | `ATHENA_INTEROP_RTP_MIN`, `_MAX` | |

Subscribers `1001` to `1004` (`subscribers.csv`, and `ATHENA_INTEROP_SUBSCRIBERS` in `fixture.env`) have the password
`athenaphone`. The realm is the address the fixture advertises, `127.0.0.1` by default,
because the registrar finds a realm by the host in the address of record (RFC 3261
10.3). `ATHENA_INTEROP_REALM` overrides it.

The API password is generated on each run. The fixture serves `ws` only; for `wss`, add
`websocket.secure_port` and the certificate filenames to `config.yaml.template`.

## Running it beside another fixture

The default ports are the ones AthenaPhone's Asterisk fixture uses, so one client can be
pointed at either. Only one can hold them at a time. To run both, move this one:

```bash
ATHENA_INTEROP_SIP_PORT=15060 ATHENA_INTEROP_TLS_PORT=15061 \
ATHENA_INTEROP_WS_PORT=18088  ATHENA_INTEROP_API_PORT=18080 \
ATHENA_INTEROP_RTP_MIN=23000  ATHENA_INTEROP_RTP_MAX=23020 \
ATHENA_INTEROP_NAME=athenasip-interop-alt test/interop/up.sh

test/interop/smoke.py --sip-port 15060 --tls-port 15061 --ws-port 18088
```

`ATHENA_INTEROP_NAME` names the container and the compose project; pass the same value
to `up.sh down`. The container binds the same port the host publishes: a node writes its
local port into Via and Record-Route, so a translated port would send clients to the
wrong one.

## Media

The default is the built-in relay: plain RTP, which is what an ordinary softphone
expects. A WebRTC client (DTLS-SRTP with ICE) registers and signals against it but
cannot agree on media. A browser or other WebRTC client needs:

```bash
test/interop/up.sh --rtpengine
```

With `--rtpengine`:

- The fixture moves off loopback. `up.sh` detects this host's LAN address, advertises
  it, names the realm after it, publishes every port on `0.0.0.0`, and signs a TLS
  certificate naming it with the snakeoil CA. Do not leave it up unattended.
  `ATHENA_INTEROP_PUBLIC_ADDRESS` overrides the address and `ATHENA_INTEROP_BIND` the
  interface the ports are published on.
- rtpengine binds its address on the fixture network (`172.32.0.30`) and advertises the
  public address. `ATHENA_INTEROP_RTPENGINE_ADVERTISE` overrides what it advertises.
- coturn runs on `ATHENA_INTEROP_TURN_PORT` (3478), relaying on
  `ATHENA_INTEROP_TURN_MIN` to `_MAX` (22300-22350), with a secret generated per run.
  `GET /api/v1/subscriber/{realm}/config` then serves the STUN and TURN servers with a credential.
  Without `--rtpengine` it serves an empty `ice_servers`.

Each leg is profiled from what it has said in a session description. For the first
offer towards an endpoint that has not yet described itself, the realm's
`media_profile` decides:

```bash
source test/interop/generated/fixture.env
TOKEN=$(curl -s -X POST http://127.0.0.1:$ATHENA_INTEROP_API_PORT/api/v1/auth/login \
  -H 'Content-Type: application/json' \
  -d "{\"username\":\"$ATHENA_INTEROP_API_USER\",\"password\":\"$ATHENA_INTEROP_API_PASSWORD\"}" |
  sed -n 's/.*"token":"\([0-9a-f]*\)".*/\1/p')
curl -X PUT http://127.0.0.1:$ATHENA_INTEROP_API_PORT/api/v1/realms/$ATHENA_INTEROP_REALM \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"behaviour":{"media_profile":"webrtc"}}'
```

`{"behaviour":{"media_anchor":false}}` leaves every description untouched, so the two
endpoints exchange media directly with no engine. `docs/behaviour.md` describes both
settings.

## Serving the admin client

`--admin` mounts the admin client's build read-only and turns `http.files` on, so the
node serves the console and its softphone. The build comes from
`../athenasip-admin/build`, or `ATHENA_INTEROP_ADMIN_DIR`. `up.sh` stops if there is no
build to serve.

```bash
(cd ../athenasip-admin && npm run build)
test/interop/up.sh --rtpengine --admin
```

Open it over loopback, `http://127.0.0.1:8080/`, whatever address the node advertises.
`getUserMedia` needs a secure origin, and loopback is one; the LAN address over plain
HTTP is not. The SIP WebSocket and the media still go wherever the node says.

## The browser call

```bash
test/interop/browser.sh          # both phases, then take the fixture down
test/interop/browser.sh --keep   # leave the fixture up afterwards
test/interop/browser.sh --direct # only the direct phase
test/interop/browser.sh --relay  # only the relay phase
```

Two headless Chromium contexts open the softphone, register, call each other through
rtpengine and read their own media counters. The page and the Playwright spec live in
`../athenasip-admin` (`ATHENA_INTEROP_ADMIN_REPO` moves it); this script brings the
fixture up with `--rtpengine --admin` and passes `generated/fixture.env` to the spec.

It checks its prerequisites first:

```bash
(cd ../athenasip-admin && npm install && npm run build && npx playwright install chromium)
```

There are two phases, with the fixture restarted between them, because no single
advertised address is reachable both from the host and from coturn:

| Phase | rtpengine advertises | What it proves |
|---|---|---|
| direct | the public address | the browsers reach the engine themselves |
| relay | its address on the fixture network (`ATHENA_INTEROP_RTPENGINE_ADDRESS`) | the TURN path: the credential the node mints, coturn accepting it, the relay reaching the engine |

The spec asserts a relayed pair by its local port being inside coturn's range, not by
`candidateType`: Chrome reports `prflx` for a relayed local candidate.

## Reading the media

```bash
test/interop/media-stats.py            # every call the engine is holding
test/interop/media-stats.py --watch    # the same, once a second
test/interop/media-stats.py <call-id>  # one call
```

It speaks rtpengine's ng protocol on the control port (`--port`, default 22222, published
on loopback only) and needs nothing installed. The engine's counters are the only
evidence that media was relayed: when the engine declines a description the endpoints
reach each other directly and the call still completes.

## The live call

`UAT.md` is the script for a browser calling a SIP phone on a real device.

## When something fails

```bash
docker logs -f athenasip-interop
```

Every SIP message in and out is logged in full, bodies included, with credentials
redacted. `ATHENA_INTEROP_LOG_MESSAGES=false` turns that off.
