# Quick Start - BLF on a Desk Phone

A busy lamp field (BLF) key on one phone shows whether another subscriber's line is in
use: dark when it is free, flashing while it rings, lit while it is on a call. The node
serves it from the calls it already carries, so nothing changes on the phone being
watched.

It assumes a running node with a realm and three subscribers who can call each other,
from [One node by hand](one-node.md) or [Try it in Docker](docker.md), and phones set up
as in [Connecting phones](phones.md). Here Carol, at reception, watches Bob, and Alice
calls him. Presence is on unless the configuration says `presence.enable: false`.

## 1. Add Carol

```
curl -X POST "$ATHENA_API/realms/example.com/subscribers" \
  -H "Authorization: Bearer $ATHENA_ADMIN_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"user":"carol","password":"carol-secret"}'
```

Register Carol's desk phone as `carol@example.com`. Only subscribers of the same realm may
watch each other; [Presence and BLF](../presence.md) says how a script can change that.

## 2. Give her phone a key for Bob

In the phone's key settings (often called line keys, DSS keys or programmable keys):

| Setting | Value |
|---|---|
| Type | BLF |
| Value, or extension | `bob` |
| Account, or line | Carol's account on the node |
| Label | Whatever the key should say |

The phone subscribes to `sip:bob@example.com` with `Event: dialog`. The node answers it
and sends the line's state at once, so the key goes dark (free) within a second or two.

Use single keys. A BLF list, where the phone subscribes once for many lines (RFC 4662),
is not served.

## 3. Watch it

Call Bob from Alice's phone. Carol's key flashes while Bob's phone rings, lights when he
answers, and goes dark when either hangs up.

The node shows who watches whom, and what it is telling them:

```
curl "$ATHENA_API/presence/subscriptions" -H "Authorization: Bearer $ATHENA_ADMIN_TOKEN"
curl "$ATHENA_API/realms/example.com/subscribers/bob/presence" -H "Authorization: Bearer $ATHENA_ADMIN_TOKEN"
```

The first lists Carol watching Bob's `dialog`, and which node holds it. The second shows
Bob's calls as Carol's key sees them, with `state` going `early`, then `confirmed`, while
the call is up.

## Buddy lists

A softphone that shows colleagues as online or offline subscribes to `presence` instead.
Add `sip:bob@example.com` as a contact with presence turned on. Bob shows as online while
he has a registration and offline when the last one goes, or as whatever his own client
publishes, such as away or busy, if it publishes.

## When the key stays dark

- `GET /presence/subscriptions` does not list Carol: the phone's SUBSCRIBE was refused or
  never arrived. A 403 in the phone's log means another realm; nothing at all, the key's
  value or account.
- Listed, but the key never lights: `GET .../bob/presence` during a call shows whether the
  node sees Bob on it.

[Presence and BLF](../presence.md) has the rest, and [Troubleshooting](../troubleshooting.md)
the node's log.
