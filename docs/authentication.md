# AthenaSIP - Authentication

**Status: design. None of the admin half described here is built.** What exists today is
described first; everything from "The model" onward is a proposal to be agreed before
code.

AthenaSIP has two entirely separate authentication problems, and conflating them would
be the first mistake:

## Two populations, and the words for them

- A **user** is a thing that can use the API. Because the admin interface is only a
  client of the API, that is also what can use the admin interface.
- A **subscriber** is a thing registered on a realm to make and receive calls.

These words are used in that sense throughout, and nowhere else in this document does
"user" mean a handset or "subscriber" mean a person at a console.

| | Who is authenticated | How | Where the secret lives |
|---|---|---|---|
| **Subscriber** | A phone or a browser registering or calling | Digest, RFC 3261 s22 | `ha1` / `ha1_sha256` in the datastore |
| **User** | A person or a system using the API | Username and password, then a session | Nothing yet - see below |

They are different trust domains and different populations. A subscriber credential
belongs to a realm and is handed to a handset; a user administers the server those
handsets register to. **Neither is ever created from the other, and neither can be used
as the other.** Creating a subscriber does not create a user; creating a user does not
create a subscriber. There is no automatic creation in either direction.

## Subscriber authentication, which is built

A subscriber holds HA1 - the hash of user, realm and password (RFC 2617) - once per
algorithm it can authenticate with, because the MD5 and SHA-256 hashes are both derived
from the password and neither can be derived from the other. The password itself is
never stored and never recoverable. The registrar challenges, the endpoint answers, and
`Registrar` checks the response against the stored HA1.

This is standard, it works, and it is not what the rest of this document is about.

The type is still called `Account` in the code and the resource is still
`/realms/{realm}/accounts`. That disagrees with the vocabulary above and is noted at
the end as a thing to settle.

## Admin authentication, which is a config file

Today, and all of it:

```yaml
http:
  api:
    tokens:
      - token: "a-long-random-string"
        scopes: [admin]
      - token: "another-one"
        scopes: [client]
```

`BearerAuth` compares the presented bearer against that list in constant time and
answers 401 for an unknown token, 403 for a known one without the scope the route
declared. `bearer_auth.h` says in its own comment that this "is what a datastore-backed
token table would replace".

### What is wrong with it

- **No identity.** A token is not a person. Six operators share one string, and an audit
  line can only say "someone with the admin token".
- **No revocation.** Removing a token means editing a file on every node and restarting
  each one.
- **No expiry.** A token leaked into a shell history, a CI log or a screen share is
  valid until somebody notices and edits that file.
- **Not shared.** A cluster's nodes each carry their own copy, identical only because
  somebody copied them carefully.
- **No login.** The admin console cannot ask a person who they are, because there is
  nothing to log in to.
- **Two coarse scopes.** `admin` can do everything provisioning can do. There is no way
  to let somebody read cluster status without also letting them delete a realm.

## The model

### Users

A record in the datastore, beside realms and accounts, so a cluster shares one set:

| Field | |
|---|---|
| `username` | unique across the node, the login |
| `display_name` | for the console and for an audit line |
| `password_hash` | see **Passwords** below |
| `roles` | any combination of the roles below, **including none** |
| `disabled` | kept for the audit trail, cannot log in |
| `created_at`, `last_login_at` | |

Not a realm, not a scope list, not a subscriber. A user belongs to the node.

### Roles

**There is no superuser role.** Nothing implies anything else. A user holds the roles
they were given and no others, and a user with no roles can log in and do nothing -
which is a useful state for an account that is being set up or wound down, and is the
default for a newly created one.

| Role | What it permits |
|---|---|
| `view-cluster-status` | Read node, registration, call and media status. Read-only, everywhere. |
| `manage-admin-users` | Create, change and remove admin users and their roles. |
| `manage-realms` | Create, change and remove realms. |
| `manage-realm-subscribers` | Create, change and remove the subscribers within a realm. |
| `manage-cluster` | Change what the cluster is: node membership, node configuration. |

A role is a permission, not a rank. `manage-realms` does not let you read cluster
status; `view-cluster-status` does not let you change anything. Somebody who needs both
is given both.

`manage-admin-users` is written for the population it manages, which is users. It is
the one role to be careful with: whoever holds it can grant themselves every other
role. That is inherent in being able to manage users, and it is why it is a role of its
own rather than bundled into another.

### Roles against the routes that exist

| Route | Role |
|---|---|
| `GET /health` | public, as now |
| `GET /session` | any authenticated user |
| `GET /nodes`, `GET /registrations` | `view-cluster-status` |
| `GET/POST/PUT/DELETE /realms[/{realm}]` | `manage-realms` |
| `GET /realms/{realm}` | `manage-realms` or `manage-realm-subscribers` (to place an account in one) |
| `GET/POST/PUT/DELETE /realms/{r}/accounts[/{u}]` | `manage-realm-subscribers` |
| `GET/POST/PUT/DELETE /users[/{u}]` | `manage-admin-users` |
| `/calls`, `/media`, `/events` (M5) | `view-cluster-status`; ending a call needs `manage-cluster` |
| node membership and configuration (M4) | `manage-cluster` |

`Router::add` therefore takes a set of roles, any of which admits, rather than one
scope string. A route with an empty set is public.

### Passwords

Stored as one self-describing string, so the algorithm can be changed per user without
invalidating everybody:

```
pbkdf2-sha256$600000$<base64 salt>$<base64 hash>
```

PBKDF2-HMAC-SHA256 from OpenSSL, which is already a dependency: a per-user random salt
from a CSPRNG, the iteration count stored with the hash so it can be raised later, and
a constant-time comparison.

Argon2id would be the better choice on its merits and is what a greenfield design should
use. It is not in OpenSSL, the rule in this tree is to prefer writing something by hand
over adding a library, and a memory-hard KDF is precisely the thing not to hand-roll.
PBKDF2 at a high iteration count is the honest compromise: weaker against a GPU
attacker, standard, and already present. The stored format above is designed so that
`argon2id$...` can coexist later if libsodium or libargon2 is ever taken as a
dependency.

The password is never stored, never logged and never returned by any endpoint.
Minimum length and any complexity rule belong in configuration, not in code.

### Sessions

A login exchanges a username and password for an opaque session token:

- 32 bytes from a CSPRNG, given to the client once and never retrievable again.
- Stored **hashed** (SHA-256; no salt is needed for a high-entropy random value), so a
  datastore dump does not hand over live sessions.
- An absolute expiry and an idle expiry, both configurable.
- Revocable individually, and all of a user's at once - which is what makes disabling a
  user immediate rather than eventual.
- Carries the roles resolved at login, re-checked against the user on each request, so
  a role removed takes effect on the next request rather than at the next login.

Presented as `Authorization: Bearer <token>`, exactly as a config token is, so the
existing `BearerAuth` gains a second place to look and no route changes shape.

Not JWT. A signed token that cannot be revoked before it expires is the wrong trade for
an admin plane, and this node already has a datastore to ask.

### Endpoints

| | |
|---|---|
| `POST /api/v1/auth/login` | `{username, password}` to `{token, expires_at, roles}` |
| `POST /api/v1/auth/logout` | ends the presented session |
| `GET /api/v1/session` | who this token is and what roles it holds |
| `GET/POST/PUT/DELETE /api/v1/users` | `manage-admin-users` |
| `POST /api/v1/users/{u}/password` | own password with the old one; anyone's with the role |
| `DELETE /api/v1/users/{u}/sessions` | revoke every session a user holds |

### What happens to the config tokens

They stay, and they change meaning. A config token becomes a **machine credential and
the way back in**:

- It is how the first admin user is created on a fresh node, and how somebody gets back
  in when every admin password has been lost. A node with no users and no config token
  is a node nobody can administer, which is worse than a token in a root-owned file.
- The `client` scope stays as it is, for SIP client applications fetching what a client
  may see. That is a machine reading its own configuration, not a person administering
  anything, and it has no business in the role model.
- The `admin` scope in a config token becomes equivalent to holding every role. This is
  the one place a "can do everything" credential exists, it lives in a root-owned file
  on disk, and the intent is that a deployment stops using it once real users exist.

`athenasip --add-user` for a node that is not running is the other half of recovery, so
getting back in does not require the API to be reachable.

## What this does not solve

- **The admin listener is plain HTTP today.** A bearer token on an unencrypted LAN
  listener is readable by anything on that LAN, and a login would put a password there
  too. TLS on the admin listener is arguably a prerequisite for building this rather
  than a companion to it, and it is not in the config schema yet. On the deployed node
  at `corvus-fi-1` the listener is bound to the LAN address, which limits but does not
  remove this.
- **Rate limiting.** A login endpoint without one is a password oracle. Per-username and
  per-source backoff has to land with it, not after it.
- **Audit.** Identity is only worth having if what each identity did is written down.
  The event bus already carries provisioning events; they need to carry who did it.
- **Inter-node authentication** is mutual TLS from the cluster CA (decision of
  2026-09-17) and is unrelated to all of this.

## Open questions

1. `manage-cluster` has no routes yet - node membership and configuration are M4 and M5.
   Is it worth defining now, or added when there is something for it to permit?
2. Session lifetime: what absolute and idle expiries suit a console left open on a NOC
   screen?
3. Should a login be rejected outright for a user with no roles, or allowed so the
   console can say "you have no permissions, ask an administrator"? The latter is
   kinder and tells an attacker slightly more.
4. Does TLS on the admin listener block this work, or land beside it?
5. **`Account` versus `subscriber`.** The vocabulary above says subscriber; the type is
   `Account` and the resource is `/realms/{realm}/accounts`. `Subscriber` was renamed
   `Account` on 2026-09-21 because it collided with SUBSCRIBE (RFC 6665) once presence
   arrived - that collision is about the C++ type name, not about the word in an API or
   in a sentence. Renaming the resource back is a breaking API change that the console
   and the OpenAPI document follow, so it wants doing deliberately and in one go, or
   not at all.
