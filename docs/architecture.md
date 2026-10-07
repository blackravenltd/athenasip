# AthenaSIP - Architecture

AthenaSIP is a multi-master, clusterable SIP proxy and registrar. Any node serves any
subscriber. Registrations survive a node dying; dialogs in flight on that node do not.

The layering is RFC 3261's:

```
udp/tcp/tls/ws/wss Server -> Connection -> Channel     transport          s18, RFC 3581
                                              |
                                      Transaction layer   matcher + 4 machines   s17
                                              |
                    +-------------+-----------+-----------+-----------+
                Registrar       Proxy       Dialogs    Qualifier    LocalUA    transaction users
               s10, 3327,      s16, 3263   s12, 4028   (OPTIONS)    (BYE)
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
| `src/registrar.*`, `src/proxy.*`, `src/dialogs.*`, `src/qualifier.*`, `src/local_ua.*` | Transaction users. `LocalUA` ends a call with a BYE to each end, sent through the proxy as the far end would send it. |
| `src/dns/` | RFC 3263 server location: NAPTR, SRV, A/AAAA |
| `src/datastores/`, `src/events/`, `src/media/`, `src/push/` | Plugin interfaces and the in-tree drivers. `src/push/` also holds the HTTP/2 and HTTPS clients and the JWT signing the push services use. |
| `src/push_refresher.*` | Pushes a sleeping client to refresh its registration before it lapses (RFC 8599) |
| `src/plugins/` | The plugin base class, the registry, the module loader and the description of a driver's settings |
| `src/node_directory.*`, `src/address_discovery.*`, `src/stun.*` | The cluster's node list, and the node finding its own public address |
| `src/cluster_ca.*` | The cluster certificate authority behind `--ca-init` and `--ca-node` |
| `src/flow_tokens.*` | Flow tokens in Path and Record-Route (RFC 5626) |
| `src/api/` | The admin HTTP API, and the subscriber's own routes under `/api/v1/subscriber/{realm}/`, which take Digest |
| `src/config.*`, `src/config_schema.*` | The configuration file, and the description of every setting that the reference, the editor schema and the misspelt-key warning come from |
| `src/cli*`, `src/main.cpp` | Command line and startup |
| `src/types/`, `src/headers/`, `src/sdp.h` | Realms, subscribers, bindings and calls; SIP headers; session descriptions |
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

`Datastore`, `EventSystem`, `MediaEngine` and `PushService` are plugin kinds in one
registry keyed by `(kind, URL scheme)`.

| Kind | No external service | For a cluster or production |
|---|---|---|
| `datastore` | `memory://` | `redis://` |
| `events` | `local://` | `mqtt://` |
| `media` | `builtin://` | `rtpengine://`, one engine or a pool |
| `push` | (off) | `apns://`, `fcm://`, `webpush://` |

None is privileged; anything else is a plugin written against the same versioned, async
contract, with its own YAML section, compiled in or loaded as a shared library at start.
[Plugins](plugins.md) is the contract.

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

A REGISTER for a domain no node serves (`sip.forward_register`) goes to that domain's
registrar, for this node's subscribers only, with a Path naming this node and carrying the
flow token and `ob` (RFC 3327, RFC 5626 5.1). A request that comes back through that Path
goes down the client's flow. Flow tokens are sealed with a per-process key, so after a
restart the client has to register again before the far end can reach it.

Client failover, for a client that can use none of RFC 3263, RFC 5626 or a balancer:

- `AthenaSIP-Alternate-Server` in the 2xx to a REGISTER lists the other nodes that are
  up, in Contact grammar, each with `expires` set to the registration's lifetime:
  `<sips:203.0.113.9:5061;transport=tls>;expires=600`. The list is the one
  `/api/v1/nodes` gives.
- It is sent only to a client that sent `Supported: athenasip-failover`, and only over TLS
  or WSS, on the same transport. Over anything else it would be a redirection whoever can
  forge a response could send; a client should honour it only from a server whose
  certificate it verified.

## The node's own address

`sip.public_address`, when set, is always the answer. When it is not:

- The node asks the `stun:` servers in `http.api.ice_servers`, and no others, from its SIP
  UDP socket, every five minutes. What they say is in the node status as `discovered`, and
  `athenasip --check` reports it, failing when it disagrees with a configured
  `sip.public_address`.
- A peer node's response says where it saw this node's request come from (RFC 3581
  `received`). When no STUN server has answered, a public address there is a finding too;
  a private one is ignored, since nodes on one LAN see each other's LAN addresses.
- Each node sends an OPTIONS to every other node's discovered address, on that node's UDP
  port, and lists the ones that answer in its status as `reaches`.
- A node whose discovered address another node has reached advertises it as if it were
  `sip.public_address`. One that no node has reached is reported and never advertised, and
  the node logs a warning saying so. A single node has nobody to reach it, so it reports
  what STUN says and advertises its bind address as before.

Each node also tries every other node's inter-node listener every ten minutes and publishes
the results as `cluster_probes`. A node that a peer has tried and none has reached marks its
`cluster` entry `"reachable": false`, and peers stop forwarding to it: behind symmetric NAT
or a connection-pinning balancer only the flows its own clients opened reach it. It keeps
publishing its address, so it recovers when a peer gets through. `athenasip --check` reports
what peers found, and fails on a listener none reaches.

Every node answers an OPTIONS addressed to itself, its public or discovered address, or one
of its realms with no user (RFC 3261 11.2), which is what the probe relies on and what a
monitor or a trunk expects.

## Media

`MediaEngine` advertises capabilities (`bridge`, `conference`, `record`, `transcode`) and
the media profiles it can produce. A `Call` holds a list of participants rather than two
fixed legs. Whether and how a call's media is anchored is [Behaviour](behaviour.md).
`rtpengine://` can be a pool of engines, each call placed on one by its Call-ID and kept
there ([Media](media.md#more-than-one-engine)).

## Versions

The version is CMake's `project(... VERSION x.y.z)`; `src/build_version.h.in` carries it
into the binary, so `athenasip --version` prints the same number. Release tags are plain
`x.y.z`, cut from `main`.
