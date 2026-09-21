# AthenaSIP - Quick Start

Two accounts calling each other on one node, from nothing, without a telecoms
background. Nothing here needs a database, a message broker or a media server: the
defaults are in-process.

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

## 2. Change the API tokens

The example config ships with two tokens named `change-me-admin` and `change-me-client`.
Change them before anything is listening on a network you do not control, in the
`http.api.tokens` section. A request without a matching token is refused.

```
export ATHENA_ADMIN_TOKEN=change-me-admin
export ATHENA_API=http://127.0.0.1:8080/api/v1
```

## 3. Create a realm and two accounts

A realm is the SIP domain accounts live in. Use the domain your clients will register to.

```
curl -X POST "$ATHENA_API/realms" \
  -H "Authorization: Bearer $ATHENA_ADMIN_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"name":"example.com"}'

curl -X POST "$ATHENA_API/realms/example.com/accounts" \
  -H "Authorization: Bearer $ATHENA_ADMIN_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"user":"alice","password":"alice-secret"}'

curl -X POST "$ATHENA_API/realms/example.com/accounts" \
  -H "Authorization: Bearer $ATHENA_ADMIN_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"user":"bob","password":"bob-secret"}'
```

The password is turned into an HA1 hash on the way in and is not stored. Nothing gives
it back, so an account whose password is lost is an account whose password is reset.

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

Each entry is one binding: the account, the contact it registered, and when it expires.

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
