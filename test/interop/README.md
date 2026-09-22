# Interop fixture

A node for a real SIP client to be pointed at, so that "does AthenaSIP work with X" is
something somebody can run rather than something somebody believes.

```bash
test/interop/up.sh          # build, start, provision
test/interop/up.sh down     # stop
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

One node, published on loopback, with everything in process: `memory://`, `local://`
and the built-in relay. Nothing here is testing Redis or a broker, and a fixture that
needs them fails for reasons that have nothing to do with SIP.

| | Port | Notes |
|---|---|---|
| UDP | 5060 | |
| TCP | 5060 | |
| TLS | 5061 | Verify against `tls/ca/snakeca.crt` |
| WS | 8088 | Any path. `/ws` is what most clients ask for |
| Admin API | 8080 | Bearer `interop-admin` or `interop-client` |

Accounts `1001`, `1002` and `1003` live in realm `127.0.0.1` with the password
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
would tell a client to come back to one nothing is listening on. That is also why
`public_address` is `127.0.0.1` here.

## Only one WebSocket listener

A node has one `websocket` section, so it serves `ws` or `wss` and not both. This
fixture serves `ws`, because it is the signalling fixture and a client that cannot be
handed a CA cannot verify `wss` anyway. Swap `tls: true` and the two filenames in
`config.yaml.template` to test the secure listener instead.

## Media

Plain RTP through the in-process relay, which is what an ordinary SIP softphone on UDP,
TCP or TLS expects. A client whose media is always WebRTC, DTLS-SRTP with ICE, will
register and signal cleanly here and then fail to agree on media. That is the honest
result rather than one arranged by configuration.

It can be arranged, knowingly:

```bash
curl -X PUT http://127.0.0.1:8080/api/v1/realms/127.0.0.1 \
  -H 'Authorization: Bearer interop-admin' -H 'Content-Type: application/json' \
  -d '{"media_profiles":"webrtc"}'
```

That tells this node to produce WebRTC descriptions for every leg in the realm, which
makes a WebRTC-only client work and makes a plain-RTP one stop. A realm cannot hold
both. `{"media_anchor":false}` is the other knob: it leaves every description untouched
and lets the two endpoints talk directly, which is the cheapest way to prove signalling
and ICE with no engine at all.

Bridging WebRTC to plain RTP needs rtpengine, which this fixture does not run.

## When something fails

```bash
docker logs -f athenasip-interop
```

Every message in and out is logged with its first line, and the registrar and the proxy
say why they answered what they did.
