# AthenaSIP - Presence and BLF

The node tells a subscriber's colleagues what the subscriber is doing: whether its line is
in use, for a desk phone's busy lamp field (BLF) keys, and whether it is online, for a
softphone's buddy list. It does this itself, from what it already knows, so neither phone
needs anything configured beyond the key or the buddy.

It is on by default. [BLF on a desk phone](quick-start/blf.md) sets up a key in a few
minutes; this page is the whole of it.

## What a watcher sees

| A phone that... | Subscribes to | Is told |
|---|---|---|
| Has a BLF key for 1001 | `dialog` (RFC 4235) | Each call 1001 is on, as 1001's side of it: `trying` while 1001 dials, `early` while it rings, `confirmed` once answered. No call is a free line. |
| Shows 1001 in a buddy list | `presence` (RFC 3856) | What 1001's own client published (RFC 3903), or, when it published nothing, `open` while 1001 has a registration and `closed` once the last one goes |

The key or the buddy is a SUBSCRIBE to `sip:1001@example.com`, which the node answers as
the notifier: 200, then a NOTIFY with the state at once, and another whenever it changes.
Presence changes are sent at most once every five seconds per watcher, as RFC 3856 6.6.2
asks; a burst of changes inside that window is sent as its latest state.

A client that publishes its own presence (away, busy, a note) is shown as it published,
across all of its devices: each device's publication keeps its own tuple, and a tuple two
of them publish under the same `id` is taken from the one that lasts longer.

A conference room is a presentity too: a key for `sip:conf-sales@example.com` lights while
anyone's call to the room's focus is up, and goes dark when the last one ends. The room's
line is the focus's side of each call, so a key shows one dialog per person in the room
([Conferencing](design/conferencing.md)).

## Who may watch whom

A subscriber may watch anyone in its own realm. Anyone else, including a subscriber of
another realm on the same node, is refused 403. A watcher proves who it is as a caller
does: the node challenges its SUBSCRIBE as it would an INVITE.

A script can change the rule with a `watch` hook: let a partner realm watch, refuse a
package with an answer of its own, or forward the SUBSCRIBE to the presentity's devices so
that a presence server of the deployment's own keeps working. [Scripting](scripting.md)
has the hook; without one, the rule above applies, and a script written before presence
keeps it.

PUBLISH has no hook: only the subscriber itself may publish its presence.

## In a cluster

Subscriptions, publications and calls all live in the datastore, which the nodes share.
The node that writes a change signals it on the event bus (`presence/<aor>/changed`), and
the node holding each watcher's subscription reads the state back and sends the NOTIFY
down the watcher's own connection. So a key on a phone registered through node B lights
for a call that went through node A.

If the bus loses a signal, each node finds the difference itself within
`presence.reconcile` seconds (30 by default). If a node dies, the node a watcher's phone
re-registers through takes its subscriptions over and notifies at once, and the dead
node's calls are closed as for any other node failure, which puts their keys out.

## Configuration

```yaml
presence:
  enable: true        # false: SUBSCRIBE and PUBLISH are proxied to the devices
  min_expires: 60     # 423 Interval Too Brief below this
  max_expires: 3600   # longer requests are granted this
  reconcile: 30       # seconds between each node's re-reads; 0 never
```

The [reference](configuration-reference.md#presence) has each setting. Presence needs a
datastore that holds it: `memory://` and `redis://` both do. On a third-party datastore
that does not, the node turns presence off, warns at start, and proxies SUBSCRIBE and
PUBLISH as before; `athenasip --check` fails its `presence` line until
`presence.enable: false` says so.

## Seeing it

| | |
|---|---|
| `GET /api/v1/presence/subscriptions` | Who is watching whom across the cluster: watcher, presentity, package, the node holding it, and its expiry |
| `GET /api/v1/realms/{realm}/subscribers/{user}/presence` | A subscriber's state as its watchers are told it: open or closed and why, each publication, and its calls |
| `presence/<aor>/changed` on `GET /api/v1/events` | When to read a subscriber's state again |

The first needs the `view-cluster-status` role and the second `manage-realm-subscribers`
([Authentication](authentication.md)). The [API description](api/openapi.yaml) has the
fields, and [Events](events.md) the topic.

## What it does not do

- No buddy lists as one subscription (RFC 4662 resource lists): a phone configured with a
  BLF list rather than single keys gets nothing. One key, or one buddy, per subscription.
- No `pending` state, no authorisation documents (RFC 5025) and no XCAP: who may watch is
  the rule above or the script's.
- No message-waiting indication: a SUBSCRIBE for `message-summary` is forwarded to the
  subscriber's devices, as any other package is.
- No on-the-phone in presence: the `dialog` package says it, and a client that wants it in
  a buddy list can publish it.
- No partial `dialog-info` documents and no filters: each NOTIFY is the full state.

## When a key stays dark

| Symptom | Look at |
|---|---|
| The phone's SUBSCRIBE is answered 403 | The watcher and the presentity are in different realms, or a script's `watch` hook refused it. |
| The phone's SUBSCRIBE reaches the other phone instead | Presence is off, or the datastore cannot hold it (`athenasip --check`), or the key subscribes to another package such as `message-summary`, which is always forwarded. Set the key's type to BLF. |
| 200, but the key never changes | `GET /api/v1/presence/subscriptions` shows whether the subscription is held, and by which node; the subscriber's `/presence` shows whether the call is there. |
| It changes late in a cluster | The event bus: each node still catches up within `presence.reconcile` seconds. |

The design and its reasoning are in [Presence](design/presence.md).
