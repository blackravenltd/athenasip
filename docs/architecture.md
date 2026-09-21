# AthenaSIP - Architecture

AthenaSIP is a multi-master, clusterable SIP server. Any node serves any account;
registrations survive a node dying, and in-flight dialogs on a dead node do not.

The layering follows RFC 3261's own, because the RFC's own division of labour is the one
the timers and the retransmission rules are written against:

```
udp/tcp/tls/ws/wss servers -> Connection -> Channel     transport   s18, RFC 3581
                                              |
                                      Transaction layer   s17: matcher + 4 machines
                                              |
                    +-------------+-----------+-----------+
                Registrar       Proxy       Dialogs       UA        transaction users
               s10, 3327,      s16, 3263   s12, 4028     (local
               5626 flows      loose route              responses)
                    |             |
                Datastore    MediaEngine       Events: observability only, never the call path
                (async)

Core = composition root + the strand
```

`Core` owns the strand and composes everything else. It does not process SIP itself.

## The strand, and what is not on it

Core runs on a single strand. Servers post into it, transaction users run on it, and the
registries it owns - channels, transactions, dialogs, calls - are touched from nowhere
else.

That is why every plugin operation is async. A blocking datastore read on the strand
stops every call on the node rather than only the one that asked, so `Datastore`,
`EventSystem` and `MediaEngine` all take the caller's executor and answer through a
handler. The admin API is deliberately not on the strand at all: it goes to the
datastore directly with its own executor, so provisioning cannot hold up a call.

A connection belongs to the thread its server's io_context runs on, not to the strand.
Everything the strand wants done to a socket it hands to `Connection::executor()`.

## Transport

One `Channel` per flow, whatever carries it: UDP, TCP, TLS, WebSocket or secure
WebSocket. A channel is known by one name, `transport://host:port`, which is the key the
registry files it under and the flow id a binding records (RFC 5626).

WSS matters more than it looks: a browser will not open an insecure WebSocket from a
page served over https, so it is not a hardening option for a web client but the only
way in.

## Plugins

`Datastore`, `EventSystem` and `MediaEngine` are plugin kinds behind one registry keyed
by `(kind, URL scheme)`. A kind is a string rather than an enum, so a plugin can
introduce one the core was not built knowing about.

Two implementations ship per kind: one built-in that needs no external service, and one
canonical for production.

| Kind | In-tree | Canonical |
|---|---|---|
| `datastore` | `memory://` | `redis://` |
| `events` | `local://` | `mqtt://` |
| `media` | `builtin://` | `rtpengine://` |

The architecture privileges none of them. DynamoDB, NATS, Kafka, an SFU or anything else
is a plugin someone can write against the same contract, not a roadmap item the core
carries. What the project promises is the contract: versioned, async from its first
version because it could not be made async later, and handing each plugin its own YAML
root plus read access to the system config.

`docs/plugins.md` is how to write one.

## Clustering

Nodes proxy SIP to each other, over mutual TLS from a cluster CA on a dedicated
listener. A cluster-CA client certificate is what distinguishes a peer node from an
endpoint or a trunk, which authenticate with Digest.

The event bus carries observability, presence and discovery, and is never on the call
setup path. Nodes find each other through retained `nodes/<id>/status` messages carrying
each node's SIP and inter-node TLS addresses; Redis holds registration ownership with a
TTL.

A binding shares and a flow does not. The Redis row is readable by any node, but the
socket a client registered on lives on one node, which is what `Location.node_id` and
`Location.flow_id` record: a node that reads a binding it does not own forwards to the
node that does.

## Media

`MediaEngine` is not a two-party SDP rewriter. It advertises capabilities - `bridge`,
`conference`, `record`, `transcode` - and a `Call` is multi-party from the start, with
participants and an optional conference focus. Web video calling and conferencing are
first-class targets, so the signalling and media model has to allow for them before
they are built.

## Where this is going

`TODO/ACTIVE.md` is the plan and the decisions behind it; `TODO/COMPLETED.md` is what
has landed. This document describes the shape, not the schedule.

`design.md` is the transport layering in more detail, `plugins.md` is the contract,
`events.md` is the topic scheme, and `scripting.md` says why there is no scripting.
