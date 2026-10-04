# AthenaSIP - Architecture

AthenaSIP is a multi-master, clusterable SIP proxy and registrar. Any node serves any
subscriber. Registrations survive a node dying; dialogs in flight on that node do not.

The layering is RFC 3261's:

```
udp/tcp/tls/ws/wss Server -> Connection -> Channel     transport          s18, RFC 3581
                                              |
                                      Transaction layer   matcher + 4 machines   s17
                                              |
                    +-------------+-----------+-----------+
                Registrar       Proxy       Dialogs    Qualifier   transaction users
               s10, 3327,      s16, 3263   s12, 4028   (OPTIONS)
               5626            6026
                    |             |
                Datastore    MediaEngine       EventSystem: observability only
                (async)      (async)

Core = composition root + the strand
```

[Glossary](glossary.md) defines the terms.

## Source layout

| Path | Holds |
|---|---|
| `src/core.*` | `Core`: owns the strand and the channel, transaction, dialog and call registries |
| `src/servers/` | One `Server` and `Connection` type per transport |
| `src/channel.*` | `Channel`: SIP framing and parsing over one connection |
| `src/transactions/` | The four RFC 3261 17 state machines and the matcher |
| `src/registrar.*`, `src/proxy.*`, `src/dialogs.*`, `src/qualifier.*` | Transaction users |
| `src/dns/` | RFC 3263 server location: NAPTR, SRV, A/AAAA |
| `src/datastores/`, `src/events/`, `src/media/` | Plugin interfaces and the in-tree drivers |
| `src/plugins/` | The plugin base class and registry |
| `src/api/` | The admin HTTP API |
| `src/config.*`, `src/cli*.h`, `src/main.cpp` | Configuration, command line, startup |
| `tests/` | GoogleTest, mirroring `src/` ([Testing](testing.md)) |

## Threading

| Runs on | What |
|---|---|
| The Core strand | All signalling: transaction users, and the registries Core owns. Nothing here blocks. |
| Each server's `io_context` thread | Socket reads, writes and closes. The strand hands socket work to `Connection::executor()`. |
| The admin API's own executor | Provisioning. It calls the datastore directly, so it cannot hold up a call. |

This is why every plugin operation is asynchronous: it takes the caller's executor and
answers through a handler ([Plugins](plugins.md)).

Timers come from an injectable `TimerSource` (`src/timer_source.h`), so tests advance time
instead of waiting.

## Transport

| Layer | Role |
|---|---|
| `Server` (`src/servers/server.h`) | Listens on one transport and address; creates a `Connection` per peer. |
| `Connection` (`src/servers/connection.h`) | One byte-level path to a peer: a socket, or for UDP a remote address. Knows nothing of SIP. |
| `Channel` (`src/channel.h`) | Wraps exactly one connection; frames, parses and serialises SIP messages. Not a session or dialog. |

A channel is named `transport://host:port`. That name is the channel registry key and the
flow id a binding records (RFC 5626), so the node holding a binding can find the
connection again without resolving the Contact.

A channel writes one message at a time; later writes queue behind the one in flight.

Browsers served over HTTPS can only open `wss`, so secure WebSocket is the only way in for
a web client.

## Plugins

`Datastore`, `EventSystem` and `MediaEngine` are plugin kinds in one registry keyed by
`(kind, URL scheme)`.

| Kind | No external service | For a cluster or production |
|---|---|---|
| `datastore` | `memory://` | `redis://` |
| `events` | `local://` | `mqtt://` |
| `media` | `builtin://` | `rtpengine://` |

None is privileged; anything else is a plugin written against the same versioned, async
contract, with its own YAML section. [Plugins](plugins.md) is the contract.

## Clustering

Nodes share a datastore and an event bus, and proxy SIP to each other over mutual TLS on a
dedicated listener ([`cluster`](configuration.md#cluster), [Certificates](certificates.md)).
A certificate signed by the cluster CA marks a connection as a peer node; everything else
is an endpoint and authenticates with Digest.

The event bus is never on the call path. Nodes discover each other through the retained
`nodes/<id>/status` messages ([Events](events.md)).

**Bindings are shared; flows are not.** Any node can read a binding, but the connection a
client registered on lives on one node, recorded as `Location.node_id` and
`Location.flow_id`.

Forwarding rules:

- A node that reads a binding it does not own forwards the request, unchanged and still
  addressed to the subscriber, to the owning node's inter-node listener: once per node,
  however many flows that node holds.
- The receiving node delivers to the flows it holds and never forwards a peer's request
  to a third node.
- A request from a peer is not challenged; the first node already challenged the caller.
- A node whose status is not `ok`, that is stale for three status intervals, or that was
  never heard from is not forwarded to.
- The first node anchors the media; the delivering node does not anchor it again.
- Both nodes Record-Route with their inter-node listener, so ACK and BYE cross the same
  way.

Outbound (RFC 5626):

- A binding is identified by `+sip.instance` and `reg-id` (`Location.instance`,
  `Location.reg_id`), not by its Contact. Registering the same pair from a new flow
  replaces the old one; a second `reg-id` is a second flow from the same client.
- A call tries the client's flows one at a time, most recently registered first. A 408 or
  430 moves to the next flow. A lost flow is never replaced by the Contact (5.3).
- Keep-alives: a double CRLF on TCP and TLS is answered with CRLF, and a STUN Binding
  request on the SIP UDP port with the source address (4.4).

## Media

`MediaEngine` advertises capabilities (`bridge`, `conference`, `record`, `transcode`) and
the media profiles it can produce. A `Call` holds a list of participants rather than two
fixed legs. Whether and how a call's media is anchored is [Behaviour](behaviour.md).

## Versions

The version is CMake's `project(... VERSION x.y.z)`; `src/build_version.h.in` carries it
into the binary, so `athenasip --version` prints the same number. Release tags are plain
`x.y.z`, cut from `main`.
