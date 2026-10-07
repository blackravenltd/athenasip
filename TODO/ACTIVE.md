# AthenaSIP - Active Work

Work happens on `develop`; `main` carries the last release, and `0.9.0` is the current
one. Line numbers refer to the current tree; update them as files move.

Milestones 1, 2 and 3 are complete apart from the items left under Milestone 3 below.
`0.8.0` (2026-10-03) carried a good part of Milestone 4 and most of what is now Milestone 6:
`COMPLETED.md` has every item with what shipped.

`0.9.0` (2026-10-07) carried, since `0.8.0`: RFC 6026's
Accepted state, media addresses that may be names, `--reset-password`, `--check`,
`log.level` and `log.format`, `sip.allow_unencrypted: false`, RFC 8599 push (unproven
against a real service for want of credentials), the second node and client failover, the
rtpengine pool, the local UA that ends dead calls with a BYE, and the configuration schema
with `--print-schema`. The README and documentation were checked against the code on
2026-10-07 and the quick-start guides written. 1296 unit tests (one skipped without a
resolver) at the tag. Both nodes run `3ff67db`, which is `0.9.0` without the fix
that lets a plugin module describe its settings.

**Two live nodes**, neither production:
- `corvus-fi-1` (10.35.1.20): builtin relay, AthenaPhone's usual home.
- `corvus-gbni-1` (10.44.1.50) as `macnessa.athenasip.org`, behind the other site's NAT:
  native rtpengine, the domain as `sip.public_address` and as the media address, DNS kept
  current by manannan, the signalling and media ports forwarded and checked from outside.
  A headless browser called AthenaPhone with video through it on 2026-10-04 and the
  console's `e2e/phone-call.spec.ts` passed (`COMPLETED.md`).

**The sipp harnesses pass at `67a4b75`** (2026-10-04, Docker back): single node 12 of 12,
with rtpengine 12 of 12, two nodes 15 of 15. That covers RFC 6026 and `allow_unencrypted`;
push is after it and has no scenario yet.

**Next:** Milestone 5, scripting and trunks, decided on 2026-10-07 and designed in
`docs/design/scripting-1-the-node.md` and `scripting-2-the-engine.md`. Packaging moves
behind it as Milestone 6. Milestone 4's intermittent `across-cancel-ringing` has not
failed again in 18 runs; it stays open for the next failure's logs.

**Once things are stable, video calling is a primary feature** (Tom, 2026-10-03), not a
later extra: principle 6 already says so, and this is the reminder that it is next in line
behind stability rather than behind everything else. The work is the video items under
Milestone 3 (verify video against AthenaPhone) and Milestone 7 (the web client), and
nothing built before then may assume a call is audio only.

Milestones are in priority order and so are the items inside each one: work top to
bottom, and say why when something is taken out of turn. Move items to `COMPLETED.md` as
they land, with a note on what shipped.

## Waiting on Tom

Nothing below can move until these are settled, and each one has somebody or something
stopped against it. They are here rather than scattered through the milestones because a
session that has lost its context needs to see them first.

1. **Push credentials.** `fcm://`, `webpush://` and `apns://` are built and tested against
   local servers only. Proving them needs a Firebase service account (AthenaPhone on
   Android), an Apple APNs key with the app's Team ID and bundle ID (iOS), or a browser
   subscription.

## How to prove it

`docs/testing.md` is the whole of it: the layers, what each answers that the one
before it cannot, and the exact commands. The short version, cheapest first:

```
./build-tests/athenasip_tests    the unit suite, with Redis and MQTT set or they skip
test/e2e/run.sh [--rtpengine]    the sipp scenarios, the second with a real engine
test/e2e/cluster.sh              the same call across two nodes of a cluster
test/interop/up.sh --rtpengine   a node to point a real client at
test/interop/browser.sh          two browsers calling, direct then relayed through coturn
e2e/phone-call.spec.ts           (../athenasip-admin) a headless browser calls a real phone
                                 through a named node, with video; somebody answers it
test/interop/UAT.md              the call a person has to make
```

All of them pass before anything is tagged.

**The sanitizers are not on that list.** Tom, 2026-09-30, and directly again on
2026-10-03: they have a place before a release, or to hunt an especially intractable bug,
and running them on every change is a waste of time. `CLAUDE.md` now says the same.

What replaces them is the thing they were standing in for: reason about lifetimes and
threading directly, and exercise the change against a running node. Nearly every bug found
in the work since 0.7.0 came from running something rather than from a checker - a
connection the node addressed by a name that would never resolve, a UDP flow whose close
was silently skipped, a TURN username no browser could use.

## Principles

1. **Multi-master, clusterable, HA SIP server.** Any node serves any subscriber.
   Registrations survive a node dying; in-flight dialogs on a dead node do not (v1).
2. **Standards first.** RFC 3261 in full (sections 16 and 17, timers A-K), plus 3263,
   3327, 3581, 4028, 5626, 7118, 8866, and for WebRTC/conferencing 4575, 4579, 8843.
   Compliance is proven by a sipp harness, not asserted.
3. **Easy to install, configure and use** without a telecoms background. Sane defaults,
   a ten-line config, one docker-compose, a CLI that generates the cluster CA and node
   certificates, and an admin UI that works out of the box. Every feature ships with
   the tooling and documentation that makes it easy, or it is not done.
4. **IoC / plugin architecture.** `Datastore`, `EventSystem` and `MediaEngine` are
   plugin kinds behind one registry keyed by (kind, URL scheme). Two implementations
   per kind: one built-in for zero-config single node, one canonical for production.
5. **Canonical backends:** Redis (datastore), MQTT (events), rtpengine (media).
6. **Web video calling and conferencing are first class.** AthenaPhone is the client
   for now; a web client follows. Everything in the signalling and media model must
   already allow for it.

## Decisions

Dated, and not reopened without asking.

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
  `route`, `on_failure`, `register`, `init`). Two drivers ship and both stay: `builtin://`
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
engines and push services today, policy (Milestone 5) next. They hang off the TU layer
and register through one contract, so adding a kind or an implementation touches nothing
above it. Policy is the one kind the transaction users call upwards into: the proxy and
registrar ask it who a caller is and where a request goes, and act on the answer.

The tree matches the diagram. The local UA (`src/local_ua.*`) is the fourth transaction user:
it sends a BYE to both ends of a call this node ends, through the proxy.
`docs/architecture.md` describes the shape for a reader; this section is the target.

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
- Outbound TLS is to cluster peers only, with the cluster's certificates. To any other host
  it is refused rather than faked.
- Record-Route is written twice on every dialog-forming request rather than only where
  the interfaces differ. RFC 5658 requires the pair in that case and permits it in
  every case; the flow token in each value is what makes a call to a browser routable
  in both directions, so both are always written.

---

## Milestone 3 - A browser calls AthenaPhone through rtpengine

Goal, narrowed from where it started: a browser on a WebSocket calls an AthenaPhone
registered over UDP, TCP or TLS, and the media goes through rtpengine. Plain-RTP
endpoints, and mixing them into the same realm as these, come after this and not
before it.

The thing to see clearly about this call is that it is not the bridging case. Both ends
are WebRTC: the browser cannot be anything else, and AthenaPhone's media is WebRTC on
every transport it signals over. rtpengine relays like to like. What differs between
the two ends is the signalling transport, and that is entirely this node's business:
the browser's Contact resolves to nothing, so the flow routing and the flow token in
the Record-Route are what make it reachable, and AthenaPhone is an ordinary SIP
endpoint on an ordinary transport. All of that is in. What is not yet in is listed
here, in the order it is needed.

The call was made on 2026-09-30 and heard both ways in both directions;
`test/interop/UAT.md` at `47de4a2` holds the record and `COMPLETED.md` the account of what it found,
including the behaviour profiles it led to. What is left of it is below.

- [ ] **Ring the automated browser call.** `browser.sh` answers within milliseconds and
      so never rang long enough to hit the DTLS-role bug the live call found; a deliberate
      delay of a few seconds before the callee answers would have caught it. A knob on the
      spec in `../athenasip-admin`. The admin session has it queued.

### After that

The trunk scenario is Milestone 5's last item: trunks are built there, and the scenario
with it. Today an off-node call goes wherever its Request-URI says.

- [ ] The phone's media across the internet: AthenaPhone on mobile data to
      macnessa.athenasip.org. The 2026-10-04 run's phone leg went over the link between
      the two sites.
- [ ] Verify the rest against AthenaPhone: hold and resume, DTMF (RFC 4733
      passthrough), blind transfer (REFER proxying).
- [ ] Interop matrix documented: AthenaPhone, JsSIP in Chrome, Firefox and Safari,
      Linphone, a hardware desk phone, Asterisk as a trunk.

---

## Milestone 4 - Cluster

Goal: two nodes, one Redis, one Mosquitto, one rtpengine; a subscriber on node A calls
a subscriber on node B; either node can die and re-registration recovers service.

The shape is the 2026-09-20 balancer decision above. What `Location` records, what
Service-Route tells a client and what `GET /api/v1/nodes` lists are all in; this
milestone is the second node.

### Client failover, in the order decided on 2026-09-21

Done (`COMPLETED.md`). What remains of client failover is the node's own address, below.

### The node's own address

Done (`COMPLETED.md`, `docs/architecture.md`).

### The second node

Both done on 2026-10-04 (`COMPLETED.md`): the full two-node harness, and the chaos test as
its last scenario.

- [ ] `across-cancel-ringing` failed once in five runs at `5986eaa` (once in the full run,
      not in four runs on its own; it passed at `67a4b75`). Cause unknown: the logs were
      overwritten by the next harness run before they were read. Run the full harness until
      it fails again, and read `cluster-across-cancel-ringing*.log` and both nodes' logs
      before naming a cause. Not reproduced since (2026-10-06, at `ed9141f`): 15 runs on its
      own and three of the full harness, the third cut short by Docker Desktop pausing after
      `across-ws-flow`. Left open, so the next failure's logs are kept and read.

---

## Milestone 5 - Scripting and trunks

Goal: a deployment connects to a carrier, routes numbers out by prefix with failover and
in by DID, and expresses any other routing it needs, by writing records over the API and,
where the standard behaviour is not enough, a Lua script; and a node that needs none of
that runs exactly as it does today, with no script loaded.

The design is in `docs/design/scripting-1-the-node.md` (the seam between decision and
mechanism, the hooks, the `policy` kind, and the eleven pieces of mechanism a trunk needs)
and `docs/design/scripting-2-the-engine.md` (the Lua engine, the library, the standard
scripts and the examples). The order below is the design's migration, and each step
leaves the unit suite and the three harnesses green.

Decisions made with it, dated 2026-10-07 under Decisions: `builtin://` stays and is the
default; Lua 5.4 from the system package (Debian `liblua5.4-dev`, Homebrew `lua@5.4`),
bound by hand, no sol2 or LuaBridge; scripts are files per node and the node publishes
their hash so the console can flag drift; the trunk password is stored as given because a
Digest response for the carrier's realm cannot be made from anything less.

### The seam

- [ ] The `policy` plugin kind: `Policy` with `authorize`, `route`, `on_failure`,
      `register_` and the decision structs (`AuthDecision`, `RouteDecision`, `TargetSpec`,
      `FailureDecision`, `RegisterDecision`), `RequestView` as the read-mostly view of a
      request, and `builtin://` holding what today is `Proxy::_authorize`
      (`src/proxy.cpp:211-271`), the lookups in `_determine_targets` (`:457-601`),
      `_authorize_relay` (`:1888`), and the registrar's realm, unserved and expiry steps
      (`src/registrar.cpp:145-188`, `:482-518`). No behaviour changes. Contract version
      bumps.
- [ ] Fix found while mapping: an off-node call gets a default `MediaPolicy` (anchor,
      mirror) because `_determine_targets :539-547` sets none and `Core::_on_dialog_change`
      creates the call without one, so `behaviour.media_anchor: false` is ignored for
      exactly the calls a trunk makes. The route decision carries the policy; a unit test
      pins it.
- [ ] Close before any outbound TLS to a non-peer exists: `Channel::peer_node()` is the
      verified peer certificate's CN (`servers/tls_connection.h:83-91`) and `_authorize`
      trusts any channel with one. Set `peer_identity` only when the chain ends at the
      cluster CA, which means the connection records which context verified it.

### The engine

- [ ] The Lua engine (`src/script/` replaced, not extended): one state per generation
      with a counting allocator, a coroutine per hook call that yields on a store call and
      resumes on the strand, instruction, wall-clock and memory budgets, errors answered
      as 500 with file and line in the log, reload that refuses a broken script and never
      switches a transaction mid-flight, `require` confined to `policy.lua.path`, no `io`,
      `os.date` and `os.time` only.
- [ ] The library: `request` as userdata with the fields in the design, header edits
      with the guard list, `store`, `auth`, `route`, `register`, `node`, `config`, `sip`.
- [ ] `lua://` and the standard scripts: `main.lua` and `athenasip/standard.lua`, every
      line annotated with the C++ it reproduces, installed by `cmake --install`.
- [ ] Proof, kept permanently: the proxy and registrar unit tests run against both
      drivers; the three harnesses pass under `policy.url: lua://`; a differential fixture
      in `tests/policy/` feeds recorded requests to both and asserts identical decisions,
      the 500 paths included.
- [ ] `athenasip --check` compiles the scripts and runs `init()`; `athenasip --explain
      <file>` runs a request through `authorize` and `route` and prints each decision.

### Trunk mechanism

- [ ] `Trunk` in the datastore (`trunk_*` as defaulted virtuals; memory and redis),
      `/api/v1/trunks` with a `manage-trunks` role, the password write-only; free-form
      `attributes` on `Realm`, `Subscriber` and `Trunk`, over the API. Console pages are
      the admin session's.
- [ ] `digest::respond`: the client half (HA1 from username, realm and password; cnonce
      and nc for `qop=auth`; MD5 or SHA-256 as offered).
- [ ] `TrunkRegistrar`: REGISTER to each trunk with registration enabled, on the
      Qualifier's pattern (`src/qualifier.cpp`), challenges answered, refreshed at what the
      200 granted, backoff on failure; one node per trunk by a datastore lease, the others
      taking over when it lapses, on `PushRefresher::_due`'s re-read-the-store pattern;
      `trunks/<name>/status` on the bus; registration state in the API's trunk read.
- [ ] Answering a carrier's 401 or 407 on a call: retry the branch with credentials and
      CSeq + 1, then a `Dialog::cseq_offset` per side applied in `_prepare_forward` and
      `_on_response` for the rest of the dialog, and again on a later challenge. The
      largest piece of mechanism in the milestone; Kamailio's `uac` module is the
      precedent.
- [ ] Outbound TLS to a trunk: a second client context, the system store or the trunk's
      CA, host name verified; chosen by the target. Anything else over TLS stays refused;
      WS and WSS outbound stay refused.
- [ ] The smaller primitives: header edits on the forwarded copy behind the guard list,
      `set_from` without touching the tag, `ring_timeout` per target on timer C's path,
      `counter_incr` and `counter_get` on the datastore, `RequestView::source`, the trunk
      on each leg of a call record, per-trunk metrics, `--check` resolving each trunk and
      `--check-trunks` sending OPTIONS.

### Trunk scripts and the harness

- [ ] `athenasip/trunks.lua` (outbound by prefix with failover, inbound by DID, caller id
      and P-Asserted-Identity, 503 and 480 skip a carrier, 486, 603 and 404 are final) and
      the examples under `docs/scripting/examples/`: peering by address, out of hours, a
      hunt sequence, subscriber-set forwarding, rate limits, a blocklist, emergency
      numbers, following a 302.
- [ ] sipp as the carrier: a registrar that challenges and grants 300 seconds, a UAS that
      answers the INVITE 407 then 180 and 200, an inbound INVITE from the carrier's address
      to a mapped number, a carrier answering 503 so the second trunk is tried. This is
      Milestone 3's trunk scenario.
- [ ] `on_call_ended(call)` as a notification hook, for concurrency counters; first
      candidate for a sixth hook, after the five have been used.
- [ ] Documentation: `docs/scripting.md` as the reference, `policy` in `configuration.md`,
      the README's plugin bullet, the Linux quick start's install step, a paragraph in
      `authentication.md` on the stored trunk password.

---

## Milestone 6 - Batteries included

Goal: a newcomer runs one command and has a working, secure, administrable SIP server
in ten minutes.

Started out of order, because a node nobody can install, run as a service or log in to
administer is not one anybody will adopt, and because deploying it is what found several
bugs that no test had. Everything that made it jump the queue has now landed, so what is
left here sits behind Milestone 4 again.

`COMPLETED.md` has it: the CMake install and the systemd unit, the command line and the
configuration search path, the heartbeat and its will, SPA mode as a choice, the node on
`corvus-fi-1`, both datastores holding users and sessions, the whole of admin
authentication from session issue to the OpenAPI document, and the one-command stack.

- [ ] athenasip-admin: replace the empty `src/lib/API.js` with a client generated from
      the OpenAPI document; pages for realms, subscribers, registrations, live calls,
      nodes, media engines, and the JsSIP test phone pointed at the server's own WSS.
      Mostly that session's work rather than this one's, and well under way: the client is
      hand-written and speaks the whole documented API, with a contract test against
      `docs/api/openapi.yaml` passing. Generation is what is left, and every operation in
      the document now carries an `operationId` for it.
- [ ] Packaging: Docker image, Debian package, Homebrew formula. In-tree plugins ship
      compiled in; the packages also carry the SDK headers.

---

## Milestone 7 - Conferencing, presence, web client

- [ ] RFC 4579 conference focus routing: conference URIs per realm, authorisation, route
      to a focus. FreeSWITCH `conference` as the first focus, configured by the admin API.
- [ ] RFC 4575 conference event package proxied to participants; roster in the admin
      and client APIs.
- [ ] `MediaEngine::conference` capability implemented for rtpengine publish/subscribe
      (basic SFU), then an SFU driver (Janus / mediasoup / LiveKit) behind the same URI
      scheme. The builtin relay stays `bridge` only: three legs on one latching port
      would forward each to the other two, which works and is not mixing.
- [ ] Presence: SUBSCRIBE/NOTIFY (RFC 6665), `presence` (RFC 3856) and `dialog` packages
      for BLF, `message-summary` for MWI, backed by MQTT fan-out.
- [ ] RFC 6080 client configuration (SUBSCRIBE to `ua-profile`, NOTIFY pointing at
      `/api/v1/subscriber/{realm}/config`), once the local UA can be a notifier for
      presence. Little before then: it needs a SUBSCRIBE/NOTIFY UAS, and the profile format
      is each vendor's, so only our own clients would read it (Tom, 2026-10-05: if it costs
      little).
- [ ] SIP MESSAGE relay.
- [ ] Push, RFC 8599, what is left (`COMPLETED.md` has what shipped): proving each service
      for real (Waiting on Tom, 1); a sipp scenario with a fake provider; and section 6,
      `pn-purr` for long-lived dialogs, which nothing needs yet. The client side is
      AthenaPhone's.
- [ ] Web client repository: video calling and conferencing, JsSIP over WSS, served by
      AthenaSIP, configured from `/api/v1/subscriber/{realm}/config` with the subscriber's
      own credentials.

---

## Parked

Kept in the tree or history, not on any milestone:

- `RTPProxyClient` (`src/rtp/rtp_proxy_client.*`): rtpproxy text protocol. Out of the
  build path since 2026-09-21, the way Lua is. May become an `rtpproxy://` driver with
  `bridge` only; not a priority.
- Full dialog-state replication for mid-call node failover.
- Parallel forking. The fork is serial: one branch at a time, best response wins. 16.7's
  response context is written to hold more than one branch and the CANCEL path already
  walks it, so the change is in `_forward_next` rather than in the shape. Forking
  creates one dialog per answering branch (12.1) and the model holds them, but with one
  branch outstanding at a time the second is untested; parallel forking is what would
  exercise it.
- `Util::is_ipv4` (`src/util.cpp:148`) still uses a regex and does see network data, but
  the pattern is anchored with no nested quantifiers, so it is linear and not the hazard
  the message-path regexes were. Left deliberately.
