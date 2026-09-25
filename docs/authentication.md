# AthenaSIP - Authentication

**Status: design. None of the admin half described here is built.** What exists today is
at the top; everything from "The gap" onward is a proposal to be agreed before code.

AthenaSIP has two entirely separate authentication problems, and conflating them would
be the first mistake:

| | Who is authenticated | How | Where the secret lives |
|---|---|---|---|
| **SIP** | A phone or a browser registering or calling | Digest, RFC 3261 s22 | `Account.ha1` / `ha1_sha256` in the datastore |
| **Admin API** | A person or a script provisioning the node | Bearer token | `http.api.tokens` in the config file |

They are different trust domains. A SIP account is a handset credential handed to an
endpoint; an admin credential provisions the server that handset registers to. One must
never be usable as the other, and no design below changes that.

## SIP authentication, which is built

An account holds HA1 - the hash of user, realm and password (RFC 2617) - once per
algorithm it can authenticate with, because the MD5 and SHA-256 hashes are both derived
from the password and neither can be derived from the other. The password itself is
never stored and never recoverable. The registrar challenges, the endpoint answers, and
`Registrar` checks the response against the stored HA1.

This is standard, it works, and it is not what this document is about.

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
declared. `admin` provisions; `client` reads what a client may see. `bearer_auth.h`
says in its own comment that this "is what a datastore-backed token table would
replace".

### What is wrong with it

- **No identity.** A token is not a person. Six operators share one string, and the
  audit log - when there is one - can only say "someone with the admin token".
- **No revocation.** Removing a token means editing a file on every node and restarting
  each one.
- **No expiry.** A token leaked into a shell history, a CI log or a screen share is
  valid until somebody notices and edits that file.
- **Not shared.** A cluster's nodes each carry their own copy, so the tokens are only
  the same because somebody copied them carefully.
- **No login.** The admin console cannot ask a person who they are, which is why it has
  no login screen: there is nothing to log in to.

## The gap

The admin console needs a person to sign in, and a cluster needs credentials that are
provisioned once rather than copied to every node. That is an authentication database:
users, hashed passwords, and sessions.

## Proposed model

### Admin users

A record in the datastore, beside realms and accounts, so a cluster shares it:

| Field | |
|---|---|
| `username` | unique, the login |
| `display_name` | for an audit line and the console |
| `password_hash` | PBKDF2-HMAC-SHA256, see below |
| `scopes` | `admin`, `client`; the same scopes routes already declare |
| `disabled` | a user kept for the audit trail but unable to log in |
| `created_at`, `last_login_at` | |

Not reusing `Account`. A SIP account belongs to a realm and authenticates a handset; an
admin user belongs to the node and authenticates a person. Sharing the type would make
"can this handset provision the server" a question anybody has to ask.

### Password hashing

PBKDF2-HMAC-SHA256, from OpenSSL, which is already a dependency. Per-user random salt,
iteration count stored with the hash so it can be raised later without invalidating
existing passwords, and a constant-time comparison.

Argon2id would be the better choice on its merits and is what a greenfield design
should use. It is not in OpenSSL, and the rule in this tree is to prefer writing
something by hand over adding a library - and hand-rolling a memory-hard KDF is
precisely the kind of thing not to hand-roll. PBKDF2 with a high iteration count is the
honest compromise: weaker against a GPU attacker, standard, and already present. If a
dependency is ever taken for this, it should be libsodium or libargon2, and the stored
format below is designed so both can coexist.

Stored as one self-describing string, so the algorithm can change per user:

```
pbkdf2-sha256$600000$<base64 salt>$<base64 hash>
```

### Sessions

A login exchanges a username and password for an opaque random session token:

- 32 bytes from a CSPRNG, given to the client once.
- Stored **hashed** (SHA-256, no salt needed for a high-entropy random value), so a
  datastore dump does not hand over live sessions.
- An absolute expiry and an idle expiry, both configurable.
- Revocable individually, and all of a user's at once.

The session token is presented as `Authorization: Bearer <token>`, exactly as a config
token is, so `BearerAuth` gains a second place to look and every route is unchanged.

Not JWT. A signed token that cannot be revoked before it expires is the wrong trade for
an admin plane, and this node already has a datastore to ask.

### Endpoints

| | |
|---|---|
| `POST /api/v1/auth/login` | `{username, password}` to `{token, expires_at, scopes}` |
| `POST /api/v1/auth/logout` | ends the presented session |
| `GET /api/v1/auth/me` | who this token is, and what it may do |
| `GET/POST/PUT/DELETE /api/v1/users` | admin scope; manage users |
| `POST /api/v1/users/{u}/password` | change one, with the old password unless admin |

### Bootstrapping, and the way back in

The config tokens do not go away. They become the break-glass credential: the way the
first user is created, and the way back in when the datastore is empty or every admin
password has been lost. A node with no users and no config tokens is a node nobody can
administer, which is a worse failure than a token in a file.

`athenasip --add-user` for a node that is not running is the other half of that, so
recovery does not require the API to be reachable.

## What this does not solve

- **The API is plain HTTP today.** A bearer token on an unencrypted LAN listener is
  readable by anything on that LAN, and a login would put a password there too. TLS for
  the admin listener is a prerequisite for this design being worth building, and it is
  not in the config schema yet. On the deployed node at `corvus-fi-1` the listener is
  bound to the LAN address, which limits but does not remove this.
- **Rate limiting.** A login endpoint without one is a password oracle. Per-username and
  per-source backoff has to land with it, not after it.
- **Audit.** Identity is only worth having if what each identity did is written down.
  The event bus already carries provisioning events; they need to carry who.
- **Inter-node authentication** is mutual TLS from the cluster CA (decision of
  2026-09-17) and is unrelated to any of this.

## Open questions

1. Does an admin user need to belong to a realm, or is the admin plane node-wide? The
   proposal above is node-wide, which is simpler and matches what the console shows.
2. Session lifetime: what absolute and idle expiries are right for a console somebody
   leaves open on a NOC screen?
3. Is `client` scope ever issued to a person, or only to a machine? If only to a
   machine, users have one scope and the model simplifies.
4. Should TLS on the admin listener be a prerequisite that blocks this, or land beside
   it?
