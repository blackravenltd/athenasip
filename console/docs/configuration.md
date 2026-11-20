# AthenaSIP Admin - Configuration

There is almost nothing to configure: the node serves the bundle and the API from one
listener, so the API is same-origin and has no address to supply.

## Build-time variables

Read by Vite and compiled in. All optional. No credential is ever one of them.

| Variable | Default | Meaning |
| --- | --- | --- |
| `VITE_ATHENASIP_LIVE` | unset | `true` talks to a real node; anything else uses the in-memory fake. |
| `VITE_SIP_WS_URL` | from the node | The diagnostic softphone's WebSocket URL. |
| `VITE_SIP_URI` | empty | A SIP URI to prefill on the diagnostic softphone. |
| `VITE_SIP_TARGET` | empty | A call target to prefill on the diagnostic softphone. |

## Development variables

| Variable | Default | Meaning |
| --- | --- | --- |
| `ATHENASIP_API` | `http://127.0.0.1:8080` | Where the dev server proxies `/api`. |

## Signing in and roles

The console signs in as a **user** of the node with a username and password, and holds the
session in memory: a reload asks again. A user is not a **subscriber**; a subscriber belongs
to a realm, registers and makes calls.

A user can do what its roles allow. No role implies another, there is no superuser, and a new
user has none.

| Role | What it permits |
| --- | --- |
| View cluster status | Read nodes, registrations, calls and the media engine. |
| Manage realms | Create, change and remove realms and their behaviour. |
| Manage subscribers | Create, change and remove a realm's subscribers. |
| Manage users | Create, change and remove users and their roles. |
| Manage cluster | End live calls and reload the routing scripts. No screen uses it yet. |
| Manage trunks | Create, change and remove trunks and set their passwords. No screen uses it yet. |

A trunk's password is kept as given, not hashed, so give Manage trunks to as few users as
possible.

The console hides what a user's roles do not allow; the node's 403 is what enforces it. The
model is the server's, in its `docs/authentication.md`.

## What the console does not manage

The node can do more than the console shows. Until it does, use the admin API
(`docs/api/openapi.yaml` in the server) or the server's own tools:

| On the node | How | Role |
| --- | --- | --- |
| Routing and authorisation scripts (`lua://`) | The server's [scripting guide](https://github.com/blackravenltd/athenasip/blob/main/docs/scripting.md) | none, on the host |
| Reloading the scripts | `POST /api/v1/policy/reload`, or `SIGHUP` on each node | Manage cluster |
| Trunks | `/api/v1/trunks` | Manage trunks |
| Attributes on a realm, subscriber or trunk | `attributes` on create or update | as for the record |
| Ending a live call | `DELETE /api/v1/calls/{call}` | Manage cluster |
| The event stream | `GET /api/v1/events` | View cluster status |
| A subscriber's presence and dialogs | `GET /api/v1/realms/{realm}/subscribers/{user}/presence` | Manage subscribers |
| Who is watching whom (presence and BLF) | `GET /api/v1/presence/subscriptions` | View cluster status |

The console never sends `attributes`, and the node replaces them only when they are given,
so changing a realm or subscriber here leaves what a script reads alone.

## Rate limits

The node limits every route and answers 429 with `Retry-After`. The console says how long to
wait and never signs out on one. Sign-in is limited most tightly, per address and per
username, and successful sign-ins count.
