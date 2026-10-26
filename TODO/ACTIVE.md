# AthenaSIP - Active Work

Work happens on `develop`; `main` carries the last release, `0.12.0` (2026-10-13). Every
release from `0.5.0` to `0.12.0` is tagged (`0.10.0` was tagged late, on 2026-10-12, dated
at its merge). Nothing has landed on `develop` since `0.12.0` but this plan. Line numbers
refer to the current tree; update them as files move.

Milestones 1 to 6 are complete, and so is presence, the first part of Milestone 7
(`COMPLETED.md` has every item with what shipped, and its Releases section the tags). This
file was rationalised against the tree on 2026-10-26: every open item below was checked
against the code that day, and the done milestones' scaffolding is gone. What is open is in
two lists: **Next**, which nothing blocks, in the order to work it, and **Waiting on
something**, each item with what it waits on.

**Two live nodes**, neither production, both on `0.12.0` (deployed 2026-10-13, the binary
built on fi-1 and copied to gbni-1 with the systemd unit; the previous binary kept as
`/usr/local/bin/athenasip.previous` on each; `verify.py` 15 of 15 after):
- `corvus-fi-1` (10.35.1.20): `lua://` with `athenasip.trunks` from
  `/etc/athenasip/scripts/main.lua` (previous configuration `config.yaml.before-lua`), and a
  trunk `macnessa` to gbni-1, registering as gbni's subscriber 1003 through 10.44.1.50.
  Deploying there is standing-authorised; touch the files changed since the last deploy
  before building (a stale object crash-looped it on 2026-10-09).
- `corvus-gbni-1` (10.44.1.50) as `macnessa.athenasip.org` behind the other site's NAT:
  native rtpengine, the binary built on fi-1. Each deploy there needs Tom's yes. `lua://`
  since 2026-10-12 (previous configuration `config.yaml.before-trunks`): `main.lua` is
  `athenasip.trunks` plus service codes (`*55`, `*NN`) out by the trunk `2talk`, a real
  carrier: Tom's NZ number +64 28 2559 1432 rings `tom` (the A85), and 2talk allows no other
  calls out. API user `trunks` (manage-trunks, manage-cluster, view-cluster-status), its
  password in `/root/.athenasip-trunks-password`.
- A probe on fi-1 (`athenasip-probe.timer`) runs `test/interop/verify.py
  test/interop/live.json` every 15 minutes and publishes `athenasip/probes/corvus-fi-1/calls`.
  A manual run at the same time as the probe fails some checks: both use subscribers 1901
  and 1902.
- TURN on both (2026-10-11): native coturn on 3478 UDP/TCP, relay 24000-24100, credentials
  minted by each node under `http.api.turn_shared_secret` and handed out by
  `/api/v1/subscriber/{realm}/config`. fi-1's is reachable only where 10.35.1.20 is; gbni-1's
  from anywhere, its router forwarding 3478 and 24000-24100 (proved from fi-1 on
  2026-10-12: an allocation over UDP and over TCP, 20 of 20 relayed).

**What passes at `0.12.0`** (2026-10-13, the whole of `docs/testing.md`'s Before a release):
the unit suite 1529 of 1530 with Redis and MQTT set (one skipped without a resolver); ASan
with UBSan clean, and TSan clean after the one race it found was fixed (`0398fb2`, the Redis
connection cancelled off its own thread on close: the first TSan run with the Redis tests
in); the sipp harnesses, one node 16 of 16, with rtpengine 18 of 18, under `--lua` 16 of 16,
two nodes 19 of 19 and again under `--lua` 19 of 19, trunks 4 of 4; the suite's smoke over
every transport, the console (263 direct, 5 relayed) and AthenaPhone (93 each) specs.

**Once things are stable, video calling is a primary feature** (Tom, 2026-10-03): nothing
built may assume a call is audio only.

Items are in priority order: work top to bottom, and say why when something is taken out
of turn. Move items to `COMPLETED.md` as they land, with a note on what shipped.

## Waiting on Tom

Nothing below can move until these are settled, and each one has somebody or something
stopped against it. They are here rather than scattered through the lists because a
session that has lost its context needs to see them first.

1. **The rest of the `0.11.0` and `0.12.0` releases.** Both tagged and pushed (step 1 of
   `docs/releasing.md`). Left, from step 2: the images built from the tags, the
   `blackravenltd/homebrew-athenasip` tap repository, and the GitHub releases, which Tom
   makes. No Debian packages for now (Tom, 2026-10-12): `docs/releasing.md` says so, and the
   `deb` target in the Dockerfile and `packaging/debian/` stay for when they return.
2. **The phone (A85), when Tom lends it**: the browser call to AthenaPhone with
   `ATHENA_INTEROP_PAGE_URL=https://10.44.1.50:8443` (plain http gives the page no
   microphone), and the hold, DTMF and transfer checks under Waiting on something.
   `phone.py` taps only while AthenaPhone is in front; its wireless adb port is found by
   mDNS.
3. **`../athenasip-admin` is not checked out on this machine** (2026-10-26). The console
   items under Waiting on something are that repository's, and `test/suite/run.sh` reports
   its spec as missing rather than failing until it is.

## How to prove it

`docs/testing.md` is the whole of it: the layers, what each answers that the one
before it cannot, and the exact commands. The short version, cheapest first:

```
./build-tests/athenasip_tests    the unit suite, with Redis and MQTT set or they skip
test/e2e/run.sh [--rtpengine]    the sipp scenarios, the second with a real engine
test/e2e/cluster.sh              the same call across two nodes of a cluster
test/e2e/run.sh --lua            any harness on the standard Lua scripts
test/e2e/trunk.sh                trunks, with sipp as the carrier
test/suite/run.sh                everything that runs unattended, the siblings' specs too
test/interop/verify.py live.json both live nodes, every transport and the trunk
test/interop/phone.py            calls to and from AthenaPhone on the A85, over adb
test/interop/up.sh --rtpengine   a node to point a real client at
test/interop/browser.sh          two browsers calling, direct then relayed through coturn
e2e/phone-call.spec.ts           (../athenasip-admin) a headless browser calls a real phone
                                 through a named node, with video; somebody answers it
test/interop/UAT.md              the call a person has to make
```

All of them pass before anything is tagged.

**The sanitizers are not on that list.** Tom, 2026-09-30, and directly again on
2026-10-03: they have a place before a release, or to hunt an especially intractable bug,
and running them on every change is a waste of time. `docs/testing.md`'s Sanitizers section
says the same. (This plan and `docs/design/presence.md` used to cite a `CLAUDE.md` for it;
no such file has ever been in the tree.)

What replaces them is the thing they were standing in for: reason about lifetimes and
threading directly, and exercise the change against a running node. Nearly every bug found
in the work since 0.7.0 came from running something rather than from a checker - a
connection the node addressed by a name that would never resolve, a UDP flow whose close
was silently skipped, a TURN username no browser could use.

## Principles

1. **Multi-master, clusterable, HA SIP server.** Any node serves any subscriber.
   Registrations survive a node dying. A dialog's state is in the datastore (2026-10-11),
   but its signalling does not move to another node yet (Parked).
   State lives in the datastore (Tom, 2026-10-11): the nodes share the datastore and the
   event system, and that is what makes them a cluster. A node holds in memory only caches
   of records, timers and its transport flows; the bus signals changes and is not storage.
2. **Standards first.** RFC 3261 in full (sections 16 and 17, timers A-K), plus 3263,
   3327, 3581, 4028, 5626, 7118, 8866, and for WebRTC/conferencing 4575, 4579, 8843.
   Compliance is proven by a sipp harness, not asserted.
3. **Easy to install, configure and use** without a telecoms background. Sane defaults,
   a ten-line config, one docker-compose, a CLI that generates the cluster CA and node
   certificates, and an admin UI that works out of the box. Every feature ships with
   the tooling and documentation that makes it easy, or it is not done.
4. **IoC / plugin architecture.** `Datastore`, `EventSystem`, `MediaEngine`, `PushService`
   and `Policy` are plugin kinds behind one registry keyed by (kind, URL scheme). Two implementations
   per kind: one built-in for zero-config single node, one canonical for production.
5. **Canonical backends:** Redis (datastore), MQTT (events), rtpengine (media), Lua
   (policy, beside the built-in default).
6. **Web video calling and conferencing are first class.** AthenaPhone is the client
   for now; a web client follows. Everything in the signalling and media model must
   already allow for it.

## Decisions

Dated, and not reopened without asking.

- (2026-10-12) **Every flavour of DTMF is handled** (Tom): RFC 4733 telephone-events,
  SIP INFO (`application/dtmf-relay` and `application/dtmf`) and in-band tones, passed
  through when both ends agree and converted when they differ. AthenaPhone does the same
  on its side; its session has been told.

- (2026-09-17) Nodes proxy SIP to each other (Path / Record-Route / Via). MQTT is for
  events, presence, discovery and observability, never on the call setup path.
- (2026-09-17) Inter-node authentication is mutual TLS from a cluster CA on a dedicated
  listener. Cluster-CA client cert = peer node (Route trust); anything else = endpoint
  or trunk (Digest).
- (2026-09-17) Cluster discovery: retained MQTT `nodes/<id>/status` messages carrying
  the node's SIP and inter-node TLS addresses; Redis holds registration ownership with
  TTL.
- (2026-09-17) MySQL, PostgreSQL and SQLite drivers are deleted. `memory` + `redis` for
  datastores, `local` + `mqtt` for events and `builtin` + `rtpengine` for media are what
  ship in-tree and are tested in v1. The architecture privileges none of them: every
  driver, in-tree or not, registers through the same plugin contract, and DynamoDB,
  NATS, Kafka or anything else is a plugin someone can write, not a roadmap item the
  core carries.
- (2026-09-18) The plugin contract is a stability promise. It is versioned, it is async
  from its first version because it cannot be made async later without breaking every
  plugin built against it, and a plugin receives its own YAML root plus read access to
  the system config. Plugins are compiled in for now and become shared libraries with
  autodiscovery later, through the same contract.
- (2026-09-17) rtpengine (ng protocol) is the flagship media engine. `RTPProxyClient`
  (rtpproxy text protocol) may remain as a driver but nothing is built on it.
- (2026-09-17) `MediaEngine` carries capabilities: `bridge`, `conference`, `record`,
  `transcode`. It is not a two-party SDP rewriter.
- (2026-09-17) `Call` is multi-party from the start (participants, optional conference
  focus).
- (2026-09-17) The transaction layer is the four RFC 3261 section 17 state machines.
  `is_reliable()` only disables timers A/E/G and shortens D/K.
- (2026-09-17) WSS is required (browsers need a secure origin); WS stays for local
  development.
- (2026-09-17) Conferencing is RFC 4579 focus routing. FreeSWITCH `conference` is the
  first focus; an SFU (Janus / mediasoup / LiveKit, or rtpengine publish/subscribe)
  comes later behind the same URI scheme.
- (2026-09-17) Lua scripting stays parked and out of the build path until there is a
  real use. When it returns it is a routing-policy plugin, not a core feature.
  **Superseded 2026-10-07 (Tom):** routing and authorisation are decisions a policy makes
  and the node carries out, and a script is how a deployment expresses them, because a
  configuration option for every case makes a configuration nobody can read and still
  cannot express the next case. `policy` is a plugin kind with five hooks (`authorize`,
  `route`, `on_failure`, `register`, `init`; presence added a sixth, `watch`, on
  2026-10-11). Two drivers ship and both stay: `builtin://`
  is today's behaviour in C++ and the default, so a plain node runs no script;
  `lua://` runs Lua 5.4 scripts, and the standard scripts reproduce `builtin://` exactly,
  which a fixture in the tree keeps checked. Trunks are records in the datastore plus
  scripts, never configuration. The SIP mechanics (transactions, Via, Record-Route, flow
  tokens, Digest verification, nonces, media anchoring, the RFC 3261 16.7 rules) stay
  native and cannot be reached from a script. Design: `docs/design/scripting-1-the-node.md`
  and `scripting-2-the-engine.md`.
- (2026-09-17) Core runs on a single strand. Servers post into it.
- (2026-09-20) Nodes can sit behind a load balancer, and the shape it forces is this. A
  binding shares; a flow does not: the Redis row is readable by any node, but the socket
  a TCP, TLS, WS or WSS client registered on lives on one node, so a node that reads a
  binding it does not own forwards to the node that does, which is what
  `Location.node_id` and `Location.flow_id` are for. Connection-oriented transports
  balance cleanly because the balancer pins a connection to one node; UDP does not,
  because an L4 balancer cannot see Call-ID, so front UDP with a SIP-aware dispatcher or
  do not balance it. TLS is passed through and never terminated at the balancer, because
  the cluster-CA peer certificate is what distinguishes a peer from an endpoint.
  Record-Route names the node, not the balancer, so in-dialog requests come straight
  back to the node that anchored the media, which is coherent with principle 1 and needs
  no dialog replication; every node therefore needs a client-reachable address of its
  own as well as the shared one. RFC 3263 SRV is the failover mechanism for SIP
  endpoints and needs no balancer; browsers cannot use it, so they get a balancer or a
  provisioned list from `GET /api/v1/subscriber/{realm}/config`.
- (2026-09-21) Client failover without infrastructure. A realm should survive a node
  dying without the operator running DNS they control, which is what principle 3 asks
  for and what RFC 3263 alone does not give. The answer is the standard mechanisms
  first and an optional extension for what they do not cover, in this order: RFC 3608
  Service-Route and `GET /api/v1/nodes` (both done), the node list fed from discovery,
  RFC 5626 outbound with two flows to two nodes, RFC 3263, and last the
  `AthenaSIP-Alternate-Server` header. The M4 items carry the detail.
- (2026-09-21, reversed 2026-10-03) The type for a subscriber was renamed `Account` in the
  code, because `Subscriber` would have collided with SUBSCRIBE (RFC 6665) the moment
  presence arrived. It is `Subscriber` again: see the 2026-10-03 decisions.
- (2026-09-25) The vocabulary, which every document, endpoint and role name follows: a
  **user** is a thing that can use the API, and the admin interface is only a client of
  the API; a **subscriber** is a thing registered on a realm to make and receive calls.
  Neither is created from the other in either direction and neither credential works as
  the other. The resource became `/realms/{realm}/subscribers` on 2026-10-03, and the
  type `Subscriber`.
- (2026-10-05, replacing the 2026-10-03 and 2026-10-04 decisions on browsers) A subscriber
  reaches its own API routes, all under `/api/v1/subscriber/{realm}/`, with HTTP Digest
  (RFC 7616) and its SIP credentials: the HTTP realm is the SIP realm, so the stored HA1 is
  the key. A softphone gets its ICE servers and a minted TURN credential there, so coturn
  stays in shared-secret mode (it cannot also take long-term users: tested 2026-10-05).
  `/client/config` moved there with no alias, and the console follows.
- (2026-10-04) nghttp2 is a dependency, for APNs, which speaks only HTTP/2.
- (2026-10-04) A node asks the `stun:` servers already in `http.api.ice_servers` for its own
  address, and no others: no public STUN server by default.
- (2026-10-04) A REGISTER for a domain no node serves is forwarded for this node's
  subscribers by default (`sip.forward_register`), over any transport.
- (2026-10-03) Tom's answers to the questions that were waiting on him:
  - The API says **subscriber** for a subscriber, everywhere and in one go:
    `/realms/{realm}/subscribers`, and `subscriber` / `subscriber_id` where a response
    said `account` / `account_id`. A subscriber belongs to a realm; a person who uses the
    admin interface is a user. No alias for the old path. The console followed in the
    same deploy.
  - **Deleting a realm deletes everything in it**: its subscribers and their
    registrations. It is in the `Datastore::realm_delete` contract, so every driver does
    it, not only the two in-tree.
  - **Every route is rate limited.** Public routes aggressively, authenticated routes
    appropriately.
  - **The admin interface does not need HTTPS yet**, unless it is really easy, in which
    case it is done to get it out of the way.
  - **No role is defined before it has routes to permit.** `manage-cluster` waits for M4
    and M5.
  - **`status_interval`** is in the node status payload, for monitors such as T.O.M.S to
    derive staleness from.
  - **Sanitizers are not routine** (see How to prove it).
  - **Terms are consistent everywhere, and `docs/glossary.md` is where they are defined.**
    The event topic is `subscribers/<uri>/status`, and logs, API messages, docs and
    scripts say subscriber. Nothing consumed the bus topic when it changed.
  - **Internal names match external names.** A **user** may sign in and observe and manage
    the cluster; a **subscriber** belongs to a realm and can REGISTER and make and receive
    calls. **Account is not anything**: the C++ type is `Subscriber`, the datastore
    operations are `subscriber_*` (contract version 14) and the Redis keys are
    `athena:subscriber:*`, which reverses the 2026-09-21 rule that the type is `Account`.
    The Redis schema is renamed and a store is recreated to match, not migrated. What
    reads the event bus is a consumer, so that subscriber means one thing.
- (2026-10-03) A cluster is a full mesh: every node reaches every other node's inter-node
  listener directly (Tom). A node forwards to the node that holds a flow in one hop, with
  no Route between them, and discovery carries no notion of which node can reach which.
  Nodes that cannot reach each other are not a cluster with a hole in it; they are two
  servers. A client only ever talks to the node it is connected to: the inter-node
  addresses are in the route set, and only a node dials them.
- (2026-09-25) There is no superuser role, and no role implies another.
- (2026-09-29) `Datastore::session_delete` succeeds whether or not that hash was held, and
  the realm and subscriber deletes still report when there was nothing there. A session is
  named by a secret the caller presented, so an answer distinguishing "that was live" from
  "that was never live" is a way to ask this node whether a token is real, one guess at a
  time, on a route anybody can reach. A realm name is not a secret. `API_VERSION` did not
  move, because the shape did not change - which is exactly why `docs/plugins.md` now says
  the version tracks shape and shape is not the whole contract.
- (2026-10-01) No configured API tokens of any scope, and no setup or bootstrap token.
  The first administrator, and recovery, is `athenasip --add-user` on the host and nothing
  else; on `memory://` that command creates the user in the node's own datastore and
  carries on serving. A file that sets `http.api.tokens` is refused at start. Automation
  signs in as a role-limited user, as `corvus-fi-1`'s provisioning does.
- (2026-10-01) Behaviour that differs between SIP servers is a setting, not a choice made
  for the operator. Each lives in one plainly named per-realm section, with a server-wide
  default in the config. The shipped default is exactly what the standards say, and every
  setting can be set to reproduce Asterisk, Kamailio/OpenSIPS or FreeSWITCH. Established
  servers are the reference for least astonishment, for migrating operators and for their
  clients. AthenaSIP is built for every RFC-compliant client, never tuned to AthenaPhone
  or JsSIP, and where it adds value it does so visibly and never by silent learning. The
  first use is what a callee is offered (Milestone 3): transport default, a per-subscriber
  profile, one re-offer on 488 reported to the operator, then OPTIONS-advertised SDP.
- (2026-10-01) The proxy is not an open relay. A request out of dialog whose From is in a
  domain this node serves is authenticated as that subscriber, whoever it is calling: RFC
  3261 22.3, a 407 with Proxy-Authenticate, answered with Proxy-Authorization for that
  same subscriber, or a reliable connection that carried an authenticated REGISTER for it. A
  From elsewhere may call into this node's domains and nowhere else (403). Requests in a
  dialog this node is on, ACK and CANCEL are not challenged. Found because the proxy had
  no 407 at all and forwarded a stranger's INVITE anywhere. What a client needs to do
  about it is its own session's work.
- (2026-10-02) Contact rewriting is a behaviour setting, off by default (Tom). The NAT item
  had left it as a judgement call; it is `behaviour.rewrite_contact`, Asterisk's
  `rewrite_contact`, and WebSocket clients are never rewritten.
- (2026-10-02) The cluster CA is made by the node with flags, `--ca-init` and `--ca-node ID
  --san NAME`, matching the command line there is rather than the subcommands this plan
  first named.
- (2026-09-29) A route declared with an empty role set means **any authenticated caller**,
  not a public route; a route open to anyone says so with `Router::add_open`. The old
  `public_scope` was an empty string, so a route that forgot to name its scope was open to
  the world. Forgetting now gives "must be logged in and may do nothing", which is the
  direction a mistake should fail in.
- (2026-09-30) A test asserts facts about this node's own configuration, never a client's
  labels for them. The case that settled it: Chrome reports a relayed local candidate as
  `candidateType: "prflx"` once connectivity checks run, so the browser relay test asserts
  the local port is inside coturn's configured range instead. Agreed with the console
  session and written into `docs/testing.md`.
- (2026-09-23) The browser end of the Milestone 3 harness is the admin client's own
  softphone, served by this node's static middleware, rather than a minimal page kept
  here. One page, maintained where the client lives, and tested where it is written;
  the cost is that the harness mounts a build from `../athenasip-admin` rather than
  serving something checked in, which is preferred to a copied bundle going stale. The
  Playwright spec lives beside the page in that repository and this side brings the
  node, rtpengine and the subscribers up for it.
- (2026-10-11) **All state lives in the datastore** (Tom). The nodes share the datastore
  and the event system, and that is what makes them a cluster: it is part of the point of
  the design. So every fact a node keeps is a record through the `Datastore` plugin, in
  `memory://`, `redis://` or whatever driver is configured; a node's memory holds caches of
  records, timers, and its own transport flows; and the bus signals that something changed
  and is never storage, since it may drop a message (MQTT here is at most once) and a
  reader must be able to read the fact back. One fact, one record: a feature that needs a
  fact another record already holds extends that record rather than keeping a copy. The
  retained `nodes/<id>/status` of the discovery decision stays: it is liveness, carried by
  a will, and nothing reads it as a record of anything else. The items under "State into
  the datastore" in Milestone 7 bring the tree to this. Dialog state is included: Tom said yes
  the same day to putting it in the call record, which supersedes principle 1's v1 terms
  for dialogs and leaves only the takeover of a live dialog's signalling parked.
- (2026-10-11) Presence (Tom, answering three questions):
  - **The node is the notifier for the `presence` and `dialog` event packages** of the
    subscribers in its realms: it answers SUBSCRIBE (RFC 6665), accepts PUBLISH (RFC 3903),
    keeps published state in the datastore, and sends the NOTIFYs. Line state for BLF
    (RFC 4235) comes from the dialogs the node already tracks, so desk phones need no client
    support. A SUBSCRIBE for any other package is forwarded to the devices, as before.
  - **Watchers: the same realm by default.** Any subscriber may watch any other in its own
    realm; anyone else is refused 403. A policy hook lets a script decide otherwise, as
    routing does. No XCAP and no per-user rule documents.
  - **No message-waiting indication for now**: nothing stores voicemail. A
    `message-summary` SUBSCRIBE is forwarded like any other package, so a voicemail server
    behind a trunk can still answer it.
- (2026-10-06) The node sends a BYE to each end when it ends a call by its own policy
  (`sip.media_timeout`, `sip.max_call_duration`) or for an administrator
  (`DELETE /api/v1/calls/{call}`, `manage-cluster`), as Kamailio's dialog module does. It
  never does on an RFC 4028 lapse, after which a proxy "MUST NOT send a BYE" (8.3). The BYE
  is the one the far end would send, entering the proxy like any in-dialog request.


## Architecture

The layering follows RFC 3261's own: transport, transaction, transaction user. `Core`
is the composition root and owns the strand; it does not process SIP itself.

```
udp/tcp/tls/ws/wss servers -> Connection -> Channel     transport   s18, RFC 3581
                                              |
                                      Transaction layer   s17: matcher + 4 machines
                                              |
                    +-------------+-----------+-----------+
                Registrar       Proxy       Dialogs       UA        transaction users
               s10, 3327,      s16, 3263   s12, 4028     (BYE to
               5626 flows      loose route              both ends)
                    |             |
                Datastore    MediaEngine       Events: observability only, never the call path
                (async)

Core = composition root + the strand
```

Everything below the transaction users is a plugin: datastores, event systems, media
engines, push services and the policy. They hang off the TU layer
and register through one contract, so adding a kind or an implementation touches nothing
above it. Policy is the one kind the transaction users call upwards into: the proxy and
registrar ask it who a caller is and where a request goes, and act on the answer.

The tree matches the diagram, with the trunk registrar (`src/trunk_registrar.*`) beside
the registrar as a UAC of its own. The local UA (`src/local_ua.*`) is the fourth transaction
user: it sends a BYE to both ends of a call this node ends, through the proxy. The notifier
(`src/notifier.*`) is the fifth, for `presence` and `dialog`; the diagram in
`docs/architecture.md` shows all five. That document describes the shape for a reader;
this section is the target, and the two agree.

### Known deviations from the standards

Each is deliberate and each is either an item below or a setting, and this list is so that
none of them is mistaken for compliance. **Re-checked against the tree on 2026-10-03:**

- RFC 3263: no blacklisting of a server that failed (4.3 permits it), so the next call
  tries it again first.
- RFC 5626: no `Flow-Timer`, by choice - the one value this node could give is longer than
  most NATs keep a UDP mapping. No 430 or 403 for an in-dialog request whose flow token
  names a flow that has gone, also by choice: the token is in every dialog's Record-Route,
  outbound or not, and today that request still reaches a desk phone through its Contact.
  No flow token in a Path this node writes as an edge for another registrar; M4.
- Forking is serial. 16.7 allows it, and parallel forking is Parked.
- A media-anchoring node rewrites the body it forwards, which 16.6 forbids a proxy, and
  re-offers the other profile once on a 488. It is the relay; `behaviour.media_anchor:
  false` turns all of it off, and where it cannot anchor the message travels on untouched.
- With `behaviour.rewrite_contact` on (off by default) the Contact an endpoint wrote is
  replaced with the address its message came from, as Asterisk and Kamailio do.
- Outbound TLS is to cluster peers, with the cluster's certificates, and to trunks,
  verified against the trunk's CA or the system's store. To any other host, such as one
  an arbitrary Request-URI names, it is refused rather than faked.
- Record-Route is written twice on every dialog-forming request rather than only where
  the interfaces differ. RFC 5658 requires the pair in that case and permits it in
  every case; the flow token in each value is what makes a call to a browser routable
  in both directions, so both are always written.

---

## Next

Nothing here waits on a device, a carrier, Tom or another repository. In order. The six
trunk findings from the 2talk run went first (2026-10-26, `COMPLETED.md`), ahead of DTMF and
the rest of Milestone 7, because each was a defect in a shipped feature found by a real
carrier, and small.

**This machine** (2026-10-26) builds the node with Lua and nghttp2 from `external/`
(gitignored, built from source: `cmake --preset tests -DCMAKE_PREFIX_PATH=$PWD/external/prefix`
with `PKG_CONFIG_PATH=$PWD/external/prefix/lib/pkgconfig`) and runs the unit suite, with the
Redis, MQTT and resolver tests skipped. It has no Docker, so no sipp harness, no cluster
and no interop fixture run here; what they alone would prove is under Waiting on something.

### DTMF, every flavour (Milestone 3, decided 2026-10-12)

RFC 4733 telephone-events, SIP INFO (`application/dtmf-relay` and `application/dtmf`) and
in-band tones, passed through when both ends agree and converted when they differ.
Pass-through is proved on the A85 against 2talk (2026-10-12: "3", "*", "0" reached the
voicemail menu, WebRTC to plain RTP). Built on 2026-10-26 (`COMPLETED.md`): the `dtmf`
setting on realms, subscribers and trunks, each leg's method on the call, rtpengine's
conversion every way (events and tones by its transcoding, INFO played in, and the key
presses it hears sent on as INFO through `rtpengine.dtmf_events`), and the documentation.
Unit-proven only; left:

- [ ] A sipp scenario sending RFC 4733 events through the node, with and without
      rtpengine, and one sending INFO to a leg that takes events (Docker: Waiting on
      something).
- [ ] Each conversion against a real rtpengine and a phone: an INFO phone to 2talk's
      voicemail, and 2talk's events to an INFO phone (the A85 or a desk phone set to INFO).
      The `block DTMF` flag on the offer towards an INFO leg and `DTMF-log-dest` per call
      are from rtpengine's ng documentation and `daemon/dtmf.c`, not yet seen working.

### State into the datastore (Milestone 7, decided 2026-10-11)

What the decision finds in the tree, re-checked 2026-10-26. Each item is small on its own;
together they are what lets a node be replaced without anything being lost. All three are
done (2026-10-26, `COMPLETED.md`): the trunk registration status and the re-offer reports
are records, and the call record carries the dialog whole, so a request inside a dialog
reaching a node that does not hold it finds it there. What does not move with it is the
media and the signalling path (Parked).

Per node by design, and not items: rate-limit buckets (`src/api/rate_limiter.h`, a node
protecting itself), metrics counters (scraped per node), the DNS cache, address
discovery (a node's own address), the qualifier's and push refresher's timers (derived
from bindings in the store), channels and flow tokens (transport), the live `Call` and
`Channel` objects (caches of records with sockets attached), and the node directory
(liveness from the bus, as decided).

### Smaller, unblocked

- [ ] A sipp scenario for push (RFC 8599) with a fake provider, so the harness proves
      what `tests/push/` proves only in units: a sleeping client woken, then the INVITE
      delivered down the flow it opens.
- [ ] `on_call_ended(call)` as a notification hook, for concurrency counters: the first
      candidate for a seventh hook, after the six (`authorize`, `route`, `on_failure`,
      `register`, `init`, `watch`) have been used.
- [ ] An arm64 Docker image beside the amd64 one, and the SDK headers in the packages.
- [ ] **A Request-URI whose host differs in case from the realm's name does not find the
      subscriber**, it appears: an INVITE to `sip:bob@EXAMPLE.COM` in a unit test reached
      nobody (2026-10-26, while writing `MediaProfileTest.TheReportNamesTheSubscriberNotWhatWasDialled`;
      not looked into further). RFC 3261 19.1.4 compares hosts case-insensitively. Confirm
      with a test from the RFC, then fix.

### Milestone 7, the rest - conferencing and the web client

After the above. The order is the milestone's own:

- [ ] RFC 4579 conference focus routing: conference URIs per realm, authorisation, route
      to a focus. FreeSWITCH `conference` as the first focus, configured by the admin API.
      `Call` already carries a focus URI and per-participant legs (`src/call.h:94`).
- [ ] RFC 4575 conference event package proxied to participants; roster in the admin
      and client APIs.
- [ ] `MediaEngine::conference` capability implemented for rtpengine publish/subscribe
      (basic SFU), then an SFU driver (Janus / mediasoup / LiveKit) behind the same URI
      scheme. The builtin relay stays `bridge` only: three legs on one latching port
      would forward each to the other two, which works and is not mixing.
- [ ] RFC 6080 client configuration (SUBSCRIBE to `ua-profile`, NOTIFY pointing at
      `/api/v1/subscriber/{realm}/config`): one more package on the notifier, which now
      exists. The profile format is each vendor's, so only our own clients would read it
      (Tom, 2026-10-05: if it costs little).
- [ ] SIP MESSAGE relay.
- [ ] RFC 8599 section 6, `pn-purr` for long-lived dialogs, which nothing needs yet.
- [ ] Web client repository: video calling and conferencing, JsSIP over WSS, served by
      AthenaSIP, configured from `/api/v1/subscriber/{realm}/config` with the subscriber's
      own credentials.

---

## Waiting on something

Each with what it waits on. None is forgotten; none can be worked here today.

**The phone (Waiting on Tom, 2):**

- [ ] The phone's media across the internet: AthenaPhone on mobile data to
      macnessa.athenasip.org. The 2026-10-04 run's phone leg went over the link between
      the two sites.
- [ ] Hold and resume, and blind transfer (REFER proxying), verified against AthenaPhone.
- [ ] DTMF conversion and in-band proved on the A85 against 2talk, once built (Next).
- [ ] Presence with a real client. AthenaPhone agreed on 2026-10-11 to be it once its PJSIP
      engine has presence: one subscription per buddy for `presence` and `dialog`, PUBLISH
      of PIDF, BLF pickup with Replaces from the dialog ids the node sends.
- [ ] Interop matrix documented: AthenaPhone, JsSIP in Chrome, Firefox and Safari,
      Linphone, a hardware desk phone, Asterisk as a trunk.

**The console, `../athenasip-admin` (Waiting on Tom, 3):**

- [ ] Ring the automated browser call: `browser.sh` answers within milliseconds and so
      never rang long enough to hit the DTLS-role bug the live call found; a delay of a
      few seconds before the callee answers would have caught it. A knob on the spec.
- [ ] Flag a node whose `policy.fingerprint` differs from the others'.
- [ ] Pages for trunks and for attributes.
- [ ] The presence screens, from the API (`docs/design/presence.md`, The API and the
      console).
- [ ] A client generated from the OpenAPI document in place of the hand-written one; every
      operation in `docs/api/openapi.yaml` carries an `operationId` for it.

**Docker on this machine** (the sipp harnesses, Redis and MQTT):

- [ ] Everything changed since `0.12.0` run through `docs/testing.md`'s Before a release:
      the CANCEL scenarios now assert the 487's CSeq (`cancel_after_180.xml`, one node and
      `across-cancel-ringing`), which only sipp proves; the trunk harness against the
      `trunks/changed` scan and the OPTIONS answer; the Redis and MQTT unit tests.

**A reproduction:**

- [ ] `across-cancel-ringing` failed once in five runs at `5986eaa` (once in the full run,
      not in four runs on its own; it passed at `67a4b75`). Cause unknown: the logs were
      overwritten by the next harness run before they were read. Not reproduced since
      (2026-10-06, at `ed9141f`): 15 runs on its own and three of the full harness. Left
      open, so the next failure's logs (`cluster-across-cancel-ringing*.log` and both
      nodes') are kept and read before naming a cause.

**Tom's word (not now, 2026-10-12: do not ask):**

- [ ] Push proved against the real Apple, Firebase and Web Push services.

**A tag to build from:**

- [ ] The Homebrew formula tried against a published tag, once the tap exists.

---

## Parked

Kept in the tree or history, not on any list:

- `RTPProxyClient` (`src/rtp/rtp_proxy_client.*`): rtpproxy text protocol. Out of the
  build path since 2026-09-21, the way Lua was. May become an `rtpproxy://` driver with
  `bridge` only; not a priority.
- A node taking over a live dialog's signalling when the node in its Record-Route dies.
  The dialog's state is in the call record and a surviving node that is sent a request in
  it carries the dialog on (2026-10-26), so this is routing and media, not replication: the
  request has to reach a surviving node, whose address is not in the dialog's Record-Route,
  and that node has to take the media over.
- Parallel forking. The fork is serial: one branch at a time, best response wins. 16.7's
  response context is written to hold more than one branch and the CANCEL path already
  walks it, so the change is in `_forward_next` rather than in the shape. Forking
  creates one dialog per answering branch (12.1) and the model holds them, but with one
  branch outstanding at a time the second is untested; parallel forking is what would
  exercise it.
- `Util::is_ipv4` (`src/util.cpp:148`) still uses a regex and does see network data, but
  the pattern is anchored with no nested quantifiers, so it is linear and not the hazard
  the message-path regexes were. Left deliberately.
