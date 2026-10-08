# Quick Start - Try It in Docker

A complete stack from a checkout, in one command: AthenaSIP with Redis for its data,
Mosquitto for its event bus, rtpengine for media and coturn for TURN, plus an
administrator, a realm and two subscribers ready to call each other. It needs Docker and
nothing else.

```
git clone https://github.com/blackravenltd/athenasip.git
cd athenasip
docker/up.sh
```

The first run pulls the image for the release checked out,
[`tomcully/athenasip`](https://hub.docker.com/r/tomcully/athenasip) on Docker Hub, and the
images it runs beside. When it finishes it prints the addresses, the credentials and what
to do next. `docker/up.sh --build` builds the image from the checkout instead, which takes
several minutes and is what to use on a branch with changes of its own.

## What you get

| | Default | Variable |
|---|---|---|
| SIP, UDP and TCP | `5060` | `ATHENA_SIP_PORT` |
| SIP, TLS | `5061` | `ATHENA_TLS_PORT` |
| Secure WebSocket | `9443` | `ATHENA_WSS_PORT` |
| Admin API | `8080` | `ATHENA_API_PORT` |
| Media | `25000-25050` UDP | `ATHENA_RTP_MIN`, `ATHENA_RTP_MAX` |
| TURN | `3478`, relaying on `25100-25150` UDP | `ATHENA_TURN_MIN`, `ATHENA_TURN_MAX` |

- The node advertises this host's LAN address, which is also the realm name. Override
  them with `ATHENA_PUBLIC_ADDRESS` and `ATHENA_REALM`.
- The subscribers are `alice` and `bob`; their passwords are in `docker/subscribers.csv`.
- The administrator is `admin` with a generated password, printed once. Set
  `ATHENA_ADMIN_USER` and `ATHENA_ADMIN_PASSWORD` to choose them. If the password is
  lost, `docker compose exec athenasip athenasip --add-user someone-else` makes another
  administrator.
- TLS and WSS use a certificate for the advertised address signed by the snakeoil CA in
  `tls/`. Clients must trust `tls/ca/snakeca.crt`. The CA's private key is in the
  repository, so it is for evaluation only.

## Make a call

Point two SIP clients at the address it printed, one as `alice` and one as `bob`
([Connecting phones](phones.md) has the settings), and dial one from the other.

To check both registered, sign in and list the registrations:

```
export ATHENA_API=http://<address>:8080/api/v1

curl -X POST "$ATHENA_API/auth/login" -H "Content-Type: application/json" \
  -d '{"username":"admin","password":"<the printed password>"}'

curl "$ATHENA_API/registrations" -H "Authorization: Bearer <the token it answered>"
```

## Stopping and starting

```
docker/up.sh down        # stop, keeping the data
docker/up.sh             # start again
docker/up.sh --reset     # stop and delete the data
docker/up.sh --console   # also serve a build of the admin console on the API port
docker/up.sh --build     # build the node's image from the checkout rather than pull it
```

`--console` serves `../athenasip-admin/build`, or the directory in `ATHENA_CONSOLE_DIR`.
Build it first with `npm run build` in the
[AthenaSIP Admin](https://github.com/blackravenltd/athenasip-admin) repository.

## Before anyone else can reach it

The stack is set up for evaluation. Before it is reachable from a network you do not
control:

- Change the TURN secret, which is the same in every clone: `turn_shared_secret` in
  `docker/config.yaml.template` and `static-auth-secret` in `docker/turnserver.conf` must
  match.
- The admin listener is plain HTTP, so a login sends a password in the clear. See
  [Authentication](../authentication.md).
- Replace the snakeoil certificate ([Certificates](../certificates.md)).

For a node that stays up, [A server on Linux](linux-server.md) is the next step.

## TURN on a loopback stack

With `ATHENA_PUBLIC_ADDRESS=127.0.0.1`, direct media works but TURN cannot relay: coturn
would be relaying to its own loopback. To exercise the relay on loopback, have rtpengine
advertise its address on the compose network instead:

```
ATHENA_PUBLIC_ADDRESS=127.0.0.1 ATHENA_RTPENGINE_ADVERTISE=172.33.0.30 docker/up.sh
```

Clients on the host then have no direct media path and reach the engine only through
TURN, so a client needs the `ice_servers` from `GET /api/v1/subscriber/{realm}/config`.

Chrome reports the local candidate of a relayed pair as `prflx`, not `relay`. Check the
local port is inside coturn's range instead.
