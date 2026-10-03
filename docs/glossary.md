# AthenaSIP - Glossary

The words this project uses, and the one thing each of them means. Every document, API
route, event topic, log line and console screen uses them in this sense. Where the
standards have their own word for the same thing it is given, so a reader coming from the
RFCs, or going to them, can find it.

A term is added here before it is used anywhere else. If two documents disagree about a
word, this page is right and the other is a bug.

## Who and what is on a node

**Realm.** The SIP domain subscribers belong to: the part after the `@` in
`sip:alice@example.com`. A realm has its own subscribers, its own Digest secret and its own
behaviour settings. Deleting a realm deletes everything in it.

**Subscriber.** A thing registered on a realm to make and receive calls: a phone, a
softphone, a browser. It has a SIP address in its realm and a password it authenticates
with by Digest. Subscribers are provisioned at `/api/v1/realms/{realm}/subscribers` and
reported on the bus under `subscribers/`. A subscriber belongs to exactly one realm and
cannot use the admin API.

**User.** A cluster-level entity that may sign in and observe and manage the cluster: a
person at the console, or a system that provisions. It has a username, a password and
roles, and signs in to the admin API; the console is only a client of that API, so a user
is also what signs in to the console. A user belongs to no realm. It cannot register or
make a call.

Neither a user nor a subscriber is ever created from the other, and neither credential
works as the other.

One other use of the word is the standards' and is kept: the `user` field of a subscriber
is the user part of its SIP address, what comes before the `@`.

**Account.** Not a term. Nothing in AthenaSIP is an account. The word was the API's and the
code's name for a subscriber until 2026-10-03 and is used in ordinary speech for a user as
well, which is why it now names neither. The "service account" in the installation guide is
the operating system's.

**Role.** What a user may do. A route names the roles that admit it and any one of them
does. There is no superuser role and no role implies another.

**Session.** What a user holds after signing in: a token, presented as a bearer credential,
with an absolute and an idle expiry. The node stores only its hash.

## Where a subscriber is

**Address of record (AOR).** A subscriber's public SIP address, `sip:alice@example.com`.
The standards' term (RFC 3261). It says who, not where.

**Contact.** Where one of a subscriber's devices can be reached right now, as that device
reported it. One subscriber can have several.

**Registration.** A device telling the node where it is, with a REGISTER request, for a
limited time. Also the result: the node knowing it.

**Binding.** One stored registration: an address of record tied to one contact until it
expires. `GET /api/v1/registrations` lists bindings. The standards' term (RFC 3261
section 10).

**Flow.** The connection a device registered over, kept so that calls to it go back down
the same connection. It is what reaches a device behind NAT or a browser, neither of which
can be connected to from outside. The standards' term (RFC 5626). A binding is shared by
every node through the datastore; a flow lives on the one node holding the connection.

**Channel.** The node's own object for one connection or UDP peer. A flow is a channel that
a registration arrived on. Not a registration: a channel opens for anything that connects.

**Transport.** How SIP messages travel: `udp`, `tcp`, `tls`, `ws` or `wss`.

## What happens on a call

**Call.** One conversation as the node sees it, from the first INVITE to the end. It has
participants and may have media passing through the node.

**Transaction.** One request and the responses to it. The standards' term (RFC 3261
section 17), and the layer that handles retransmission and timeouts.

**Dialog.** The relationship between two endpoints that an answered INVITE sets up and a
BYE ends. The standards' term (RFC 3261 section 12).

**Proxy.** The part of the node that passes a request on towards whoever it is for. The
node is a proxy for calls: it routes them and does not answer them itself.

**Registrar.** The part of the node that accepts registrations.

**Offer and answer.** How two endpoints agree on media: one describes what it can send and
receive (the offer, in SDP), the other replies with what it accepts (the answer). The
standards' terms (RFC 3264).

**Re-offer.** The node making a callee a second offer, in the other media profile, after
the callee refused the first with 488. Done once, and reported to the operator.

## Media

**Media engine.** What relays, and where needed converts, the audio and video of a call.
A plugin. `builtin` relays plain RTP; `rtpengine` relays and converts anything.

**Media profile.** What kind of media an endpoint takes: `rtp` (plain RTP), `webrtc` (ICE
and DTLS-SRTP, what a browser needs), `transport` (decided by how the endpoint connected)
or `mirror` (whatever the caller offered).

**Anchoring.** Routing a call's media through the node's media engine rather than letting
the two endpoints send to each other directly. It is what gets media through NAT.

**Qualify.** The node sending OPTIONS to registered devices on an interval to see whether
they are still there. Asterisk's word for it.

**Behaviour.** The settings for the choices SIP servers differ on: anchoring, media
profile, qualifying, Contact rewriting. Set for the server, for a realm, or for a
subscriber, each overriding only what it sets.

## Nodes and the cluster

**Node.** One running AthenaSIP process, named by `sip.node_id`.

**Cluster.** Several nodes sharing one datastore and one event bus. Any node serves any
subscriber. Nodes send calls to each other as SIP, over mutual TLS.

**Cluster CA.** The certificate authority a cluster makes for itself. A connection
presenting a certificate it signed is another node; anything else is an endpoint.

**Node status.** What a node says about itself on the bus, retained, every
`status_interval` seconds: whether it is up, and where it listens. Also called the
heartbeat. A node that dies without saying goodbye is reported `down` by the broker.

## The pieces a node is built from

**Plugin.** An implementation of one of the pluggable kinds, registered under a URL scheme
and chosen by a URL in the config. The kinds are datastore, event system and media engine.

**Driver.** A plugin, when speaking of which one: "the Redis driver".

**Datastore.** Where realms, subscribers, bindings, users and sessions are kept. `memory`
for a single node with nothing to install, `redis` for a cluster.

**Event system, event bus.** Where a node publishes what is happening, for monitors and
for other nodes to discover it. `local` or `mqtt`. It carries observability only; no call
ever waits on it.

**Topic.** The name an event is published under, such as `nodes/<id>/status`.

**Admin API.** The HTTP API a user provisions and observes a node through, at `/api/v1`.

**Console.** The web admin interface, served by the node. A client of the admin API and
nothing more.

**Core, strand.** The node's composition root and the single thread of execution that all
signalling runs on. Nothing on it ever blocks.

## Where the code does not say this yet

Settled on 2026-10-03 and not yet carried through. The C++ type `Account`, the datastore
operations `account_*` and the Redis keys `athena:account:*` mean a subscriber. This
section is deleted when the rename lands.
