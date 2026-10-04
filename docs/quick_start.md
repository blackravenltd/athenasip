# AthenaSIP - Quick Start

Two subscribers calling each other on one node. There are two ways: one command that
brings up a full stack in Docker, or by hand on one process with nothing external.

## The full stack in one command

```
docker/up.sh
```

This starts AthenaSIP with Redis (datastore), Mosquitto (event bus), rtpengine (media)
and coturn (TURN), creates an administrator, a realm and subscribers `alice` and `bob`,
and prints the addresses, the credentials and what to do next.

| | Default | Variable |
|---|---|---|
| SIP, UDP and TCP | `5060` | `ATHENA_SIP_PORT` |
| SIP, TLS | `5061` | `ATHENA_TLS_PORT` |
| Secure WebSocket | `9443` | `ATHENA_WSS_PORT` |
| Admin API | `8080` | `ATHENA_API_PORT` |
| Media | `25000-25050` UDP | `ATHENA_RTP_MIN`, `ATHENA_RTP_MAX` |
| TURN | `3478`, relaying on `25100-25150` UDP | `ATHENA_TURN_MIN`, `ATHENA_TURN_MAX` |

```
docker/up.sh --console   # also serve a build of the admin console on the API port
docker/up.sh down        # stop, keeping the data
docker/up.sh --reset     # stop and delete the data
```

- The node advertises this host's LAN address, which is also the realm name. Override
  them with `ATHENA_PUBLIC_ADDRESS` and `ATHENA_REALM`.
- The administrator is `admin` with a generated password, printed once. Set
  `ATHENA_ADMIN_USER` and `ATHENA_ADMIN_PASSWORD` to choose them. If the password is
  lost, `docker compose exec athenasip athenasip --add-user someone-else` makes another
  administrator.
- Subscriber passwords are in `docker/subscribers.csv`.
- TLS and WSS use a certificate for the advertised address signed by the snakeoil CA in
  `tls/`. Clients must trust `tls/ca/snakeca.crt`. The CA's private key is in the
  repository, so it is for evaluation only.
- `--console` serves `../athenasip-admin/build`, or the directory in
  `ATHENA_CONSOLE_DIR`. Build it first with `npm run build` in that repository.

Before the stack is reachable from a network you do not control, change the TURN secret,
which is the same in every clone: `turn_shared_secret` in `docker/config.yaml.template`
and `static-auth-secret` in `docker/turnserver.conf` must match. The admin listener is
plain HTTP, so a login sends a password in the clear; see
[authentication.md](authentication.md).

### TURN on a loopback stack

With `ATHENA_PUBLIC_ADDRESS=127.0.0.1`, direct media works but TURN cannot relay: coturn
would be relaying to its own loopback. To exercise the relay on loopback, have rtpengine
advertise its address on the compose network instead:

```
ATHENA_PUBLIC_ADDRESS=127.0.0.1 ATHENA_RTPENGINE_ADVERTISE=172.33.0.30 docker/up.sh
```

Clients on the host then have no direct media path and reach the engine only through
TURN, so a client needs the `ice_servers` from `GET /api/v1/client/config`.

Chrome reports the local candidate of a relayed pair as `prflx`, not `relay`. Check the
local port is inside coturn's range instead.

## By hand, on one process

### 1. Start a node with an administrator

Build it ([compiling.md](compiling.md)) and copy `config/config.example.yaml` to
`~/.athenasip/config.yaml`. The example uses `memory://`, `local://` and `builtin://`:
no external services, and nothing survives a restart.

The admin API has no configured tokens. The first administrator is made with
`--add-user`, which prompts for a password without echoing it. With `memory://` it
creates the user and then carries on as the node, so this one command starts it:

```
athenasip --add-user you --role manage-admin-users --role manage-realms \
  --role manage-realm-subscribers --role view-cluster-status
```

Or with the image, whose configuration is the same example:

```
docker build -t athenasip .
docker run --rm -it -p 5060:5060/udp -p 5060:5060/tcp -p 9500:9500 -p 8080:8080 \
  -p 22000-23000:22000-23000/udp athenasip --add-user you --role manage-admin-users \
  --role manage-realms --role manage-realm-subscribers --role view-cluster-status
```

With a persistent datastore such as `redis://`, `--add-user` writes the user and exits;
start the node afterwards with plain `athenasip`.

Check it is serving:

```
curl http://127.0.0.1:8080/api/v1/health
```

### 2. Sign in

```
export ATHENA_API=http://127.0.0.1:8080/api/v1

curl -X POST "$ATHENA_API/auth/login" \
  -H "Content-Type: application/json" \
  -d '{"username":"you","password":"your password"}'

export ATHENA_ADMIN_TOKEN=<the token it answered>
```

The answer carries the token, its expiry and the user's roles. Present the token as
`Authorization: Bearer $ATHENA_ADMIN_TOKEN`. [authentication.md](authentication.md)
covers roles, sessions and password resets.

### 3. Create a realm and two subscribers

A realm is the SIP domain subscribers live in. Name it for the domain your clients
register to; the registrar finds a realm by the host of the address of record.

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

Only the HA1 hash of a password is stored, and no endpoint returns it: a lost password
is reset, not recovered. [`api/openapi.yaml`](api/openapi.yaml) is the whole API.

### 4. Point two SIP clients at it

Any SIP client will do, such as [Linphone](https://www.linphone.org/) or
[Zoiper](https://www.zoiper.com/).

| Setting | Value |
|---|---|
| SIP address | `alice@example.com` on one, `bob@example.com` on the other |
| Password | the one you provisioned |
| Proxy | the address the node is reachable at |
| Transport | UDP or TCP on 5060, or WebSocket on 9500 |

If the clients cannot resolve `example.com`, name the realm for the node's address
instead, or set the node's address as the client's proxy and keep the realm as the SIP
domain.

For clients on other machines, set `sip.public_address` and
`media.builtin.public_address` in the configuration to the node's address; the example
advertises media on `127.0.0.1`.

Check that both registered:

```
curl "$ATHENA_API/registrations" -H "Authorization: Bearer $ATHENA_ADMIN_TOKEN"
```

Each entry is one binding: the subscriber, its contact and when it expires.

### 5. Call

Dial `bob@example.com` from Alice's client. Media goes through the node's built-in
relay, so it works between clients that cannot reach each other directly.

## TLS and WSS

`tls/` holds a self-signed certificate and CA for trying TLS. Enable the `tls` listener,
and set `websocket.tls` for a browser: a page served over https cannot open an insecure
WebSocket.

Clients refuse the certificate until they trust `tls/ca/snakeca.crt`. For anything real,
use a certificate from a CA your clients already trust;
[configuration.md](configuration.md) has the settings.
