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

| Route | Role | |
|---|---|---|
| `GET /health` | open | done |
| `POST /auth/login`, `POST /auth/logout` | open | done |
| `GET /session` | any authenticated caller | done |
| `GET /nodes`, `GET /registrations` | `view-cluster-status` | done |
| `GET /realms`, `GET /realms/{realm}` | `manage-realms` or `manage-realm-subscribers` | done |
| `POST/PUT/DELETE /realms[/{realm}]` | `manage-realms` | done |
| `GET/POST/PUT/DELETE /realms/{r}/accounts[/{u}]` | `manage-realm-subscribers` | done |
| `GET/POST/PUT/DELETE /users[/{u}]`, `DELETE /users/{u}/sessions` | `manage-admin-users` | done |
| `POST /users/{u}/password` | any authenticated caller; the handler decides | done |
| `/calls`, `/media`, `/events` (M5) | `view-cluster-status`; ending a call needs `manage-cluster` | |
| node membership and configuration (M4) | `manage-cluster` | |

Reading realms admits either role because anyone placing a subscriber has to discover
which realms exist; changing one is `manage-realms` alone. That was agreed with the
console, which needs exactly this to show a realm picker to somebody who only manages
accounts.

`Router::add` takes a set of roles, any of which admits, rather than one scope string, and
**an empty set means any authenticated caller rather than a public route**. That is the
safe thing for it to mean: a route declared without naming roles then demands a credential
and grants nothing, so forgetting to name them cannot open a route to the world. A route
genuinely open to anyone has to say so by name, with `Router::add_open`.

Both kinds of credential arrive as a set of roles, which is the only vocabulary a route
speaks. `BearerAuth::resolve` is what turns one into the other, and it is asynchronous
because resolving a session token means asking the datastore. Two consequences worth
knowing:

- **A configuration token is resolved before the store is consulted**, so it keeps working
  when the store is the broken thing - which is the point of it being the way back in. A
  session token cannot collide with one, being 32 random bytes, so the order costs nothing.
- **A store that cannot be asked is a 503, not a 401.** Telling an administrator their
  credential is bad when the real problem is Redis sends them looking in the wrong place.

The three refusals are therefore distinct: 401 for a credential that is absent or no longer
good (an unknown token, an expired session, a disabled user), 403 for a real credential that
does not hold the role, and 503 for not being able to tell. A known configuration token
holding no scopes is a 403, because it is a real credential authorised for nothing.

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

Both expiries are configuration, under `http.api`:

```
http:
  api:
    session_lifetime: 43200   # seconds a login is good for at all
    session_idle: 3600        # seconds it survives unused; 0 turns this off
```

They are not symmetric, and the asymmetry is the contract's. The absolute expiry is
written on the session record, so the datastore is what prunes on it and a lifetime of
zero is refused: `SETEX` has no non-positive expiry to give a key, and a record nothing
expires outlives the node. The idle timeout is never given to a driver at all - it is
`api::Sessions` that asks `Session::has_expired`, deletes a session that has gone idle
rather than leaving it to sit out the rest of its lifetime, and moves `last_seen_at`
along by writing the record again. Since a write per authenticated request is a real
cost, it moves it once per tenth of the idle window rather than on every lookup, which
is enough to keep a session in use alive and turns the other nine reads back into reads.

`api::Sessions` (`src/api/sessions.h`) is where all of that lives, and where a token is
made and hashed: the driver never sees one. It is also where the clock is a seam, for the
same reason `TimerSource` is one in the transaction layer - an idle timeout measured
against the real clock is an hour of waiting per test case. The drivers go on calling
`std::time` themselves, because the only thing they do with the time is refuse or prune a
record whose absolute expiry has passed, and a test controls that by choosing the expiry.

A token is hex rather than base64: twice the length, and nothing in it has to be escaped
in a header, a URL, a shell or a log line, which for a value an operator will paste around
is worth more than the characters it costs.

### Endpoints

| | |
|---|---|
| `POST /api/v1/auth/login` | `{username, password}` to `{token, expires_at, roles}` |
| `POST /api/v1/auth/logout` | ends the presented session |
| `GET /api/v1/session` | who this token is and what roles it holds |
| `GET/POST/PUT/DELETE /api/v1/users` | `manage-admin-users` |
| `POST /api/v1/users/{u}/password` | own password with the old one; anyone's with the role |
| `DELETE /api/v1/users/{u}/sessions` | revoke every session a user holds |

A delete answers 204 with no body, as every other delete in this API does, and 404 when
there is no such user. The datastore deliberately does not make that distinction -
`session_delete_for_user` succeeds whether or not anything was revoked, because "this
user holds no sessions" is the state the caller asked for - so revoking for a user who
has never logged in is a 204. The existence check belongs to the handler, exactly as
`_with_realm` does it for realms.

The three auth routes are built, in `api::AuthAPI` (`src/api/auth_api.h`), and answer:

| | |
|---|---|
| `POST /auth/login` | 200 `{token, expires_at, roles}`; 400 for a body that is not an object or is missing a field; 401 for anything else; 503 when the store cannot be asked |
| `POST /auth/logout` | 204 with no body; 401 with no token presented; 503 when the store cannot be asked |
| `GET /session` | 200 `{"kind":"user", username, display_name, roles, expires_at}` or `{"kind":"token", scopes, roles}`; 401; 503 |

Three things about those answers are load-bearing rather than incidental:

- **A refused login is one body, byte for byte**, for an unknown user, a wrong password
  and a disabled user alike. The three alternatives are between them a list of who holds
  an account on this node. An empty password is a 401 rather than a 400 for the same
  reason - probing with one should learn nothing a wrong password would not - while a
  *missing* field is a 400, because that is the caller's own mistake and says nothing
  about anybody.
- **A logout is 204 whether or not the token named a session.** A 404 there would be a way
  to ask whether a token is real, one guess at a time, on a route anybody can reach. This
  is why `Datastore::session_delete` succeeds for a hash it was not holding, which is the
  one place the session deletes and the realm deletes follow different rules: a realm name
  is not a secret. A 503 still means the store refused, so a client is never told its token
  is gone when it may not be.
- **503, not 500, when the store is down**, and with a fixed message rather than the
  store's own. A dependency being unreachable is a retry rather than a defect, and the
  detail belongs in the node's log rather than in a body an unauthenticated caller reads.

### The users routes

`api::UsersAPI` (`src/api/users_api.h`). All of it needs `manage-admin-users` except
changing a password, which is declared for any authenticated caller and decides for
itself: your own needs the old one, anyone's needs the role. "It is mine" is not something
a role can express, which is why that one route carries its own rule.

What is bootstrapped, and by what: a fresh node has no users, so the configuration token
is what creates the first one, and that user can then create the rest. This is the chain
that makes the console usable on a node nobody has logged into yet.

Four rules here are worth knowing before reading the code:

- **Nobody locks themselves out of the door they are standing in.** A user cannot disable
  itself, cannot take `manage-admin-users` away from itself, and cannot delete itself; all
  three are 409 `would_lock_out`. Deleting is in the list because a user that deleted
  itself is locked out exactly as thoroughly, and leaving that open would make the other
  two decorative. The rules are about *self*, not about the role: somebody else holding
  `manage-admin-users` may still be disabled, demoted and deleted, and a node with no
  administrators left is recovered with the configuration token. A configuration token is
  nobody, so the rules never apply to it.
- **Changing a password ends every session that user held**, including the one that asked.
  A password is changed because the old one is no longer trusted, and a session issued
  against it is exactly as untrusted; an administrator resetting a compromised account
  would otherwise leave whoever compromised it logged in. The cost is that changing your
  own password logs you out, which is the right way round.
- **An unknown role is refused on the way in**, 400 `unknown_role`. The datastore carries
  a role it does not recognise rather than dropping it, so that an older node rewriting a
  user cannot silently strip a role a newer one granted - which leaves the API as the only
  place a typo can be caught.
- **Somebody else's password without the role is 403 before the store is asked**, so the
  route cannot be walked to find out who exists. With the role it is a 404, because by then
  the caller is allowed to know.

The PBKDF2 iteration count is a constructor parameter rather than configuration. The count
is stored with each hash so it can be raised later without invalidating anybody, nothing
has asked to tune it, and a setting whose wrong value is invisible until somebody steals
the database is not one to offer before there is a reason. Tests turn it down; production
takes the default.

`/auth/login` is declared open because it is how a credential is obtained, and
`/auth/logout` because it answers the same whether or not the token it was handed
resolved - a token that resolves to nothing has to reach the handler rather than be turned
away with a 401 that tells the caller it was not real.

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
2. ~~Session lifetime: what absolute and idle expiries suit a console left open on a NOC
   screen?~~ Answered by making it configuration, with defaults that suit that console:
   twelve hours absolute, so a working day does not ask for the password twice, and an
   hour idle, so a tab somebody walked away from stops being trusted. An operator who
   wants a session that never idles out sets `session_idle: 0`; one that never expires at
   all is not offered, for the reason in Sessions above.
3. ~~Should a login be rejected outright for a user with no roles, or allowed so the
   console can say "you have no permissions, ask an administrator"? The latter is
   kinder and tells an attacker slightly more.~~ Allowed, which is what was agreed with
   the console and is what `api::Sessions` does: a login answers with the roles the user
   holds, and none is an answer. It tells an attacker who already has a valid password
   that the account is real, which they could learn from any other route anyway.
4. Does TLS on the admin listener block this work, or land beside it?
5. **`Account` versus `subscriber`.** The vocabulary above says subscriber; the type is
   `Account` and the resource is `/realms/{realm}/accounts`. `Subscriber` was renamed
   `Account` on 2026-09-21 because it collided with SUBSCRIBE (RFC 6665) once presence
   arrived - that collision is about the C++ type name, not about the word in an API or
   in a sentence. Renaming the resource back is a breaking API change that the console
   and the OpenAPI document follow, so it wants doing deliberately and in one go, or
   not at all.
