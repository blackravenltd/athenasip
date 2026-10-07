# Quick Start - One Node by Hand

Two subscribers calling each other through one AthenaSIP process with nothing external:
no database, no broker, no media server. Each step is one command, so you see what the
node needs and what the admin API does. Nothing survives a restart.

## 1. Start a node with an administrator

Build it ([Compiling](../compiling.md)) and copy `config/config.example.yaml` to
`~/.athenasip/config.yaml`. The example uses `memory://`, `local://` and `builtin://`:
the datastore, event bus and media relay all inside the process.

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

## 2. Sign in

```
export ATHENA_API=http://127.0.0.1:8080/api/v1

curl -X POST "$ATHENA_API/auth/login" \
  -H "Content-Type: application/json" \
  -d '{"username":"you","password":"your password"}'

export ATHENA_ADMIN_TOKEN=<the token it answered>
```

The answer carries the token, its expiry and the user's roles. Present the token as
`Authorization: Bearer $ATHENA_ADMIN_TOKEN`. [Authentication](../authentication.md)
covers roles, sessions and password resets.

## 3. Create a realm and two subscribers

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
is reset, not recovered. [`api/openapi.yaml`](../api/openapi.yaml) is the whole API.

## 4. Point two SIP clients at it

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

## 5. Call

Dial `bob@example.com` from Alice's client. Media goes through the node's built-in
relay, so it works between clients that cannot reach each other directly.

## Next

- [Connecting phones](phones.md) covers TLS, NAT and what to check when a client will not
  register.
- [Calling from a browser](browser.md) adds rtpengine, secure WebSocket and TURN.
- [A server on Linux](linux-server.md) makes this a node that survives a restart.
