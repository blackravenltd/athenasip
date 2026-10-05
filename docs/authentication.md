# AthenaSIP - Authentication

Two separate populations authenticate, and neither credential works as the other
([Glossary](glossary.md)):

| | Who | How | Stored as |
|---|---|---|---|
| **Subscriber** | A phone or browser registering or calling | SIP Digest (RFC 3261 22) | HA1 per algorithm (MD5, SHA-256), per realm |
| **User** | A person or system using the admin API or console | Username and password, then a session token | PBKDF2 hash; session token hash |

Creating one never creates the other. Nodes authenticate to each other with mutual TLS
from the cluster CA ([Certificates](certificates.md)); a request from a peer node is not
challenged.

## Subscribers

The password is never stored; the datastore holds HA1 for each algorithm. The registrar
challenges REGISTER with 401. The proxy challenges calls with 407 (RFC 3261 22.3):

| The caller's From is | Calling | Result |
|---|---|---|
| in a realm this node serves | anybody | 407; must answer as that subscriber. Another subscriber's credentials are 403. |
| in a realm this node serves, on a TCP, TLS or WebSocket connection that carried an authenticated REGISTER for it | anybody | Let through |
| anywhere else | a realm this node serves | Let through: an incoming call |
| anywhere else | anywhere else | 403 |

In-dialog requests, ACK and CANCEL are never challenged. A UDP client answers a 407 on
every call. The Digest arithmetic is in `src/digest.h`.

Subscribers are provisioned at `/api/v1/realms/{realm}/subscribers`.

### Over HTTP

A subscriber's softphone reaches its own routes, all under `/api/v1/subscriber/{realm}/`,
with HTTP Digest (RFC 7616) and the same username and password it registers with: the HTTP
realm is the SIP realm, so the stored HA1 is the key and nothing else is kept. The node
answers 401 with two challenges, SHA-256 then MD5, each with `qop="auth"`; a nonce that has
expired is challenged again with `stale=true`. A wrong password and an unknown subscriber
get the same 401, and failures are rate limited by source address like an open route.

These credentials open nothing outside that prefix, and a user's session opens nothing
inside it. The routes: `config` (where to signal, ICE servers with a minted TURN
credential, what the realm expects), `registrations` (the subscriber's own bindings) and
`password` (change its own).

## Users

Stored in the datastore, so a cluster shares one set.

| Field | |
|---|---|
| `username` | Unique; the login |
| `display_name` | For the console |
| `password_hash` | See [Passwords](#passwords) |
| `roles` | Any combination of the roles below, including none |
| `disabled` | Cannot sign in |
| `created_at`, `last_login_at` | |

### The first administrator, and recovery

There are no configured tokens. Both commands run on the node's host, start no listener,
and read the password from the terminal without echo, or from standard input when it is
not a terminal. No option takes a password.

| Command | Does | Exit |
|---|---|---|
| `athenasip --add-user NAME [--display-name N] [--role R ...]` | Creates a user. Without `--role` it holds `manage-admin-users`. | 0; 3 if the name is taken; 1 otherwise |
| `athenasip --reset-password NAME` | Sets a new password and ends every session the user held | 0; 3 if there is no such user; 1 otherwise |

With `redis://` the command writes the user and exits, and is safe beside a running node.
With `memory://` a separate process cannot reach a running node's memory, so `--add-user`
creates the user and then carries on as the node.

### Roles

There is no superuser and no role implies another. A user with no roles can sign in and
do nothing.

| Role | Permits |
|---|---|
| `view-cluster-status` | Read nodes, registrations, calls, call records, media, qualify state, metrics and the client config |
| `manage-admin-users` | Create, change and remove users and their roles. Its holder can grant itself any role. |
| `manage-realms` | Create, change and remove realms |
| `manage-realm-subscribers` | Create, change and remove subscribers in a realm |
| `manage-cluster` | Defined; no route requires it yet |

### Routes and roles

All under `/api/v1` except `/metrics`. [`api/openapi.yaml`](api/openapi.yaml) is the full
reference.

| Route | Admitted |
|---|---|
| `GET /health`, `POST /auth/login`, `POST /auth/logout` | Open |
| `GET /session`, `POST /users/{user}/password` | Any signed-in user |
| `GET /nodes`, `/registrations`, `/calls`, `/calls/{call}`, `/call-records`, `/media`, `/media/reoffers`, `/qualify`, `/events`, `GET /metrics` | `view-cluster-status` |
| `GET /realms`, `GET /realms/{realm}` | `manage-realms` or `manage-realm-subscribers` |
| `POST /realms`, `PUT`/`DELETE /realms/{realm}` | `manage-realms` |
| `/realms/{realm}/subscribers[/{user}]` | `manage-realm-subscribers` |
| `/users[/{user}]`, `DELETE /users/{user}/sessions` | `manage-admin-users` |
| `GET /subscriber/{realm}/config`, `GET /subscriber/{realm}/registrations`, `PUT /subscriber/{realm}/password` | The subscriber itself, with Digest (below) |

Refusals:

| Status | Meaning |
|---|---|
| 401 | No credential, or one that is no longer good: unknown token, expired session, disabled user |
| 403 | A valid session without a role the route admits |
| 429 | Rate limited; see [`http.api.rate_limits`](configuration.md#httpapi) |
| 503 | The datastore could not be asked. Retry; the credential may be fine. |

### Signing in

| Route | Answers |
|---|---|
| `POST /auth/login` with `{username, password}` | 200 `{token, expires_at, roles}`; 400 for a malformed body or missing field; 401 otherwise |
| `POST /auth/logout` | 204, whether or not the token named a session |
| `GET /session` | 200 `{"kind":"user", username, display_name, roles, expires_at}` |

Present the token as `Authorization: Bearer <token>`.

A refused login has the same body for an unknown user, a wrong password, an empty
password and a disabled user, so it reveals nothing about who exists.

### Sessions

- The token is 32 random bytes, hex encoded, returned once. The datastore holds only its
  SHA-256 hash.
- `http.api.session_lifetime` (default 43200 s) is the absolute expiry, enforced by the
  datastore. `http.api.session_idle` (default 3600 s, `0` off) is the idle expiry, checked
  on each request.
- Roles are read from the user on each request, so a role change or a disable takes
  effect at once.
- A session can be revoked singly (logout) or for a whole user
  (`DELETE /users/{user}/sessions`).

### Managing users

- A user cannot disable itself, remove `manage-admin-users` from itself or delete itself:
  409 `would_lock_out`. Another holder of the role can do all three. A node with no
  administrator left is recovered with `--add-user`.
- Changing a password ends every session that user held, including the caller's.
- Changing your own password needs the old one. Changing another user's needs
  `manage-admin-users`; without it the answer is 403 whether or not the user exists.
- An unknown role is refused: 400 `unknown_role`.

### Passwords

```
pbkdf2-sha256$600000$<base64 salt>$<base64 hash>
```

PBKDF2-HMAC-SHA256 from OpenSSL with a random per-user salt and a constant-time
comparison. The iteration count is stored with the hash, so it can be raised without
invalidating existing passwords. A password is never logged or returned.

## Transport

The plain `http` listener sends credentials in the clear. Enable
[`http.tls`](configuration.md#http) and sign in over HTTPS.

## For contributors

| Where | What |
|---|---|
| `src/api/router.h` | `Router::add(verb, path, roles, handler)` admits any of `roles`. An empty set means any signed-in user, not a public route; a public route is declared with `Router::add_open`. |
| `src/api/bearer_auth.h` | Resolves a bearer token to a role set, asynchronously |
| `src/api/sessions.h` | Makes and hashes tokens, applies the idle expiry, and takes an injectable clock for tests. `last_seen_at` is rewritten at most once per tenth of the idle window. |
| `src/api/auth_api.h`, `src/api/users_api.h` | The routes above |
| `src/api/rate_limiter.h` | Rate limits |
| `src/types/password.h` | Hashing; tests pass a lower iteration count |

Datastore drivers must keep two behaviours: `session_delete` succeeds for a hash it does
not hold (otherwise logout reveals whether a token is real), and a store failure is
reported as a failure, never as "not found".
