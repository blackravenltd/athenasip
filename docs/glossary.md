# AthenaSIP - Glossary

Each term has one meaning across the documentation, API, event topics, logs and console.
Where a term is the standards' own, the RFC is given.

## Who and what is on a node

| Term | Meaning |
|---|---|
| **Realm** | The SIP domain subscribers belong to: the part after `@` in `sip:alice@example.com`. Has its own subscribers, Digest secret and behaviour. Deleting a realm deletes everything in it. |
| **Subscriber** | A phone, softphone or browser registered on one realm to make and receive calls. Authenticates by Digest. Cannot sign in to the admin API, but reads its own configuration and registrations under `/api/v1/subscriber/{realm}/` with the same Digest credentials. |
| **User** | A person or system that signs in to the admin API or console, with a username, password and roles. Belongs to no realm and cannot register or call. Also, as in the standards, the part of a SIP address before the `@`. |
| **Account** | Not a term. Nothing in AthenaSIP is an account, except the operating system's service account. |
| **Role** | What a user may do. A route names the roles that admit it. No role implies another. |
| **Session** | What a user holds after signing in: a bearer token with an absolute and an idle expiry. |

A subscriber and a user are never created from each other; see
[Authentication](authentication.md).

## Where a subscriber is

| Term | Meaning |
|---|---|
| **Address of record (AOR)** | A subscriber's public SIP address, `sip:alice@example.com`. Says who, not where (RFC 3261). |
| **Contact** | Where one of a subscriber's devices can be reached now, as the device reported it. |
| **Registration** | A device telling the node where it is, by REGISTER, for a limited time. |
| **Binding** | One stored registration: an AOR tied to one contact until it expires (RFC 3261 10). `GET /api/v1/registrations` lists bindings. Shared by every node through the datastore. |
| **Flow** | The connection a device registered over, kept so calls reach it through NAT or in a browser (RFC 5626). Lives on the one node holding the connection. |
| **Channel** | The node's object for one connection or UDP peer, named `transport://host:port`. A flow is a channel a registration arrived on. |
| **Transport** | How SIP messages travel: `udp`, `tcp`, `tls`, `ws` or `wss`. |

## What happens on a call

| Term | Meaning |
|---|---|
| **Call** | One conversation as the node sees it, from the first INVITE to the end, with its participants. |
| **Transaction** | One request and its responses; the layer that handles retransmission and timeouts (RFC 3261 17). |
| **Dialog** | The relationship between two endpoints that an answered INVITE sets up and a BYE ends (RFC 3261 12). |
| **Proxy** | The part of the node that forwards a request towards its destination. The node routes calls; it does not answer them. |
| **Local UA** | The part of the node that ends a call itself, sending each end a BYE: when its media stops, when it reaches `sip.max_call_duration`, or when an administrator hangs it up. It never starts or answers one. |
| **Registrar** | The part of the node that accepts registrations. |
| **Offer and answer** | How two endpoints agree on media, in SDP (RFC 3264). |
| **Re-offer** | A second offer the node makes to a callee, in the other media profile, after a 488. Made once. |

## Media

| Term | Meaning |
|---|---|
| **Media engine** | The plugin that relays, and where needed converts, a call's audio and video. `builtin` relays plain RTP; `rtpengine` relays and converts. |
| **Push service** | The plugin that wakes a sleeping phone for a call (RFC 8599): `apns` for iOS, `fcm` for Android, `webpush` for browsers. |
| **Media profile** | The kind of media a leg is offered: `rtp`, `webrtc`, `srtp`, `transport` (decided by how it connected) or `mirror` (what the caller offered). |
| **Anchoring** | Routing a call's media through the media engine instead of end to end. |
| **Qualify** | Sending OPTIONS to registered devices on an interval to see whether they are still there. |
| **Behaviour** | The settings for choices SIP servers differ on, set per server, realm or subscriber. See [Behaviour](behaviour.md). |

## Nodes and the cluster

| Term | Meaning |
|---|---|
| **Node** | One running AthenaSIP process, named by `sip.node_id`. |
| **Cluster** | Nodes sharing one datastore and one event bus. Any node serves any subscriber. Nodes pass calls to each other as SIP over mutual TLS. |
| **Cluster CA** | The certificate authority a cluster makes for itself. A connection presenting a certificate it signed is another node. |
| **Node status** | What a node publishes about itself, retained, every `events.status_interval` seconds. Also called the heartbeat. |

## What a node is built from

| Term | Meaning |
|---|---|
| **Plugin** | An implementation of a pluggable kind (datastore, event system, media engine, push service), registered under a URL scheme and selected by a URL in the config. |
| **Driver** | A particular plugin: "the Redis driver". |
| **Module** | A shared library holding one or more drivers, loaded at start from `plugins.path`. |
| **Datastore** | Where realms, subscribers, bindings, users, sessions and call records are kept: `memory` or `redis`. |
| **Event system, event bus** | Where a node publishes what is happening: `local` or `mqtt`. Observability and discovery only; no call waits on it. |
| **Topic** | The name an event is published under, such as `nodes/<id>/status`. |
| **Consumer** | Something that reads the event bus. Not called a subscriber. |
| **Listener** | A socket the node accepts connections on: SIP, inter-node, or admin. |
| **Server** | The object that owns one listener and creates a connection per peer. |
| **Connection** | One transport-level path to a peer, at the byte level. A channel wraps exactly one. |
| **Admin API** | The HTTP API at `/api/v1` through which a user provisions and observes a node. |
| **Console** | The web admin interface the node serves. A client of the admin API. |
| **Core** | The node's composition root. |
| **Strand** | The single serialised executor all signalling runs on. Nothing on it blocks. |
