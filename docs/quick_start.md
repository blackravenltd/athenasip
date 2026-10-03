# AthenaSIP - Quick Start

Two subscribers calling each other on one node, from nothing, without a telecoms
background.

There are two ways in. The first is one command and gives you what a real deployment
looks like; the second is by hand and needs no external service at all. Read the second
if you want to understand what the first did.

## The whole thing in one command

```
docker/up.sh
```

That brings up AthenaSIP with the canonical backends - Redis for the datastore, Mosquitto
for the event bus, rtpengine on the media path, coturn for a browser that needs a relay -
then provisions a realm, two subscribers and an administrator, and prints what to do next.

| | |
|---|---|
| SIP | `5060` UDP and TCP, `5061` TLS |
| WSS | `9443` |
| Admin API | `8080` |
| TURN | `3478` |

It prints the administrator's password once and cannot print it again, because the API
never hands a password back. If you lose it, `docker compose exec athenasip athenasip
--add-user someone-else` is the way back in.

Ports move if something else already has them:

```
ATHENA_SIP_PORT=15060 ATHENA_TLS_PORT=15061 \
ATHENA_WSS_PORT=19443 ATHENA_API_PORT=18080 docker/up.sh
```

`docker/up.sh down` stops it and keeps the data; `docker/up.sh --reset` forgets it too.

### If you want to exercise TURN

A relay only works if the TURN server can reach the media engine, and on a stack running
entirely on loopback it cannot: rtpengine advertises `127.0.0.1`, and inside coturn's
container that address is coturn. A browser will gather a relay candidate, ICE will get as
far as checking, and no audio will flow. Direct media is unaffected, which is why this is
the default and only a note.

Two ways to a relay that works. Either run with this host's own address, which is what
`docker/up.sh` picks when it is not told otherwise:

```
ATHENA_PUBLIC_ADDRESS=<this host's address> docker/up.sh
```

Or keep everything on loopback and point the engine at its own address on the compose
network, which exposes nothing outside this machine:

```
ATHENA_RTPENGINE_ADVERTISE=172.33.0.30 docker/up.sh
```

The second works because TURN requires only the *TURN server* to reach the peer, never the
client: the browser talks to coturn on loopback, and coturn relays onto the compose network.
The cost is narrower than it looks. A client on the host cannot reach that address, so the
direct path fails - but a client that read `ice_servers` from `/api/v1/client/config` has a
TURN server, and ICE falls back to the relay on its own when the direct path fails. Measured
on this stack: a browser with relay forced carried 399 packets each way with no loss, and the
same browser with relay *not* forced also carried audio, because it fell back. Only a client
with no TURN server at all - the `softphone.html` harness page, which is given none
deliberately - fails outright.

So this mode costs a direct media path and not a working call, for anything that provisions
itself from the API.

One thing not to chase: Chrome's `RTCStatsReport` labels the local end of a relayed pair
`candidateType: "prflx"` with `relayProtocol: "udp"` and a blanked address, rather than
`"relay"`. Under relay policy nothing but a relay candidate is gathered, and the local ports
are inside coturn's configured range, so it is the TURN allocation - Chrome is relabelling
the relayed path peer-reflexive once connectivity checks run.

**Before this reaches a network you do not control**, change the API tokens in
`docker/config.yaml.template` and `static-auth-secret` in `docker/turnserver.conf`. They
are the same in every clone of this repository. The admin listener is also plain HTTP,
so a login puts a password on the wire - see the end of
[authentication.md](authentication.md) for what is and is not solved.

The admin console lives in its own repository. `docker/up.sh --console` serves a build of
it from port 8080 when there is one.

---

The rest of this page is the same thing by hand, on one process with nothing external.

## 1. Get a node running

Either build it ([compiling.md](compiling.md)) and run `athenasip`, or use the image:

```
docker build -t athenasip .
docker run --rm -p 5060:5060/udp -p 5060:5060/tcp -p 9500:9500 -p 8080:8080 \
  -p 22000-23000:22000-23000/udp athenasip
```

The configuration it starts with is `config/config.example.yaml`, copied to
`~/.athenasip/config.yaml` (or mounted over `/root/.athenasip` in the container). Out of
the box that is `memory://` for the datastore, `local://` for events and `builtin://`
for media - one process, no external services, and nothing that survives a restart.

Check it is serving:

```
curl http://127.0.0.1:8080/api/v1/health
```

## 2. Make an administrator

The admin API has no tokens to configure. The first administrator is made on the node's
host with `athenasip --add-user`, which asks for the password without echoing it:

```
athenasip --add-user you --role manage-admin-users --role manage-realms \
  --role manage-realm-subscribers --role view-cluster-status
```

With the example config's `memory://` datastore, which keeps nothing once a process
exits, that command creates the user and then carries on as the node - so it is how you
start the node the first time. With `redis://` it writes the user and exits, and you start
the node as usual. Then sign in:

```
export ATHENA_API=http://127.0.0.1:8080/api/v1

curl -X POST "$ATHENA_API/auth/login" \
  -H "Content-Type: application/json" \
  -d '{"username":"you","password":"your password"}'

export ATHENA_ADMIN_TOKEN=<the token it answered>
```

The login answers a token, what it expires at, and the roles that user holds. Present it
as `Authorization: Bearer $ATHENA_ADMIN_TOKEN`. A user who has lost their password is
reset by somebody holding `manage-admin-users`, or by `athenasip --add-user` on the host
if the API cannot be reached at all. [authentication.md](authentication.md) is the whole
of it.

## 3. Create a realm and two subscribers

A realm is the SIP domain subscribers live in. Use the domain your clients will register to.

```
curl -X POST "$ATHENA_API/realms" \
  -H "Authorization: Bearer $ATHENA_ADMIN_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"name":"example.com"}'

curl -X POST "$ATHENA_API/realms/example.com/subscribers" \
  -H "Authorization: Bearer $ATHENA_ADMIN_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"user":"alice","password":"alice-secret"}'

curl -X POST "$ATHENA_API/realms/example.com/subscribers" \
  -H "Authorization: Bearer $ATHENA_ADMIN_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"user":"bob","password":"bob-secret"}'
```

The password is turned into an HA1 hash on the way in and is not stored. Nothing gives
it back, so a subscriber whose password is lost is a subscriber whose password is reset.

[`docs/api/openapi.yaml`](api/openapi.yaml) is the whole API.

## 4. Point two SIP clients at it

Any SIP client will do - [Linphone](https://www.linphone.org/),
[Zoiper](https://www.zoiper.com/), or a browser client over WebSocket. Each one needs:

| Setting | Value |
|---|---|
| SIP address | `alice@example.com` (and `bob@example.com` on the other) |
| Password | the one you provisioned |
| Domain / proxy | the address the node is reachable at |
| Transport | UDP, TCP or WS to start with |

If your clients cannot resolve `example.com`, either use a realm name that resolves or
point the clients at the node's address directly and leave the SIP domain as the realm.

Check that both registered:

```
curl "$ATHENA_API/registrations" -H "Authorization: Bearer change-me-client"
```

Each entry is one binding: the subscriber, the contact it registered, and when it expires.

## 5. Call

Dial `bob@example.com` from Alice's client. Media goes through the node's built-in
relay, which is why it works between two clients that cannot reach each other directly.

## TLS and WSS

The node ships with a self-signed certificate and CA in `tls/` so that TLS can be tried
without a certificate authority. Turn on the `tls` listener, or `websocket.tls` for a
browser - a page served over https will not open an insecure WebSocket, so a web client
needs WSS rather than WS.

A self-signed certificate means clients will refuse it until they are told to accept it.
In Linphone that is `verify_server_certs=0` in `~/.linphonerc`. **Undo that when you
have finished evaluating**: it turns off the check that makes TLS worth having.

For anything real, use a certificate from a CA your clients already trust.
