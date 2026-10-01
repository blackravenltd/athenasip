# AthenaSIP - Active Work

Work happens on `develop`; `main` carries the last release, and `0.7.0` is the current
one. Line numbers refer to the current tree; update them as files move.

Milestones 1 and 2 are complete and recorded in `COMPLETED.md`. What M3 builds on: the
header, URI, identity and message model follow RFC 3261; `Core` is a composition root on
a single strand with no locks and no SIP semantics of its own; the four section 17 state
machines sit behind a matcher with `Registrar` and `Proxy` as the transaction users;
`Dialogs` observes calls so media is released and a call record closed when they end;
`Datastore`, `EventSystem` and `MediaEngine` are async plugins behind one registry, with
`memory://` and `redis://`, `local://` and `mqtt://`, and `builtin://` and
`rtpengine://` in tree; a node decides for itself when a call it is holding is over;
provisioning is over a JSON API; and the sipp harness proves a call end to end on UDP.
870 tests.

0.6.0 landed RFC 5626 flow routing, the rtpengine driver and the media profile that
tells it which leg is the browser, and the behaviour tests that found eleven bugs in
code nothing had ever tested. 0.7.0 is the mixed-transport call: a realm says what its
calls ask of the engine, the node record-routes both interfaces and carries a flow
token in each so an in-dialog request can reach a browser, and three recorded
deviations from RFC 3261 and 3264 are closed. `COMPLETED.md` has the detail.

Since 0.7.0, on `develop` and not yet tagged, in three parts, all recorded in
`COMPLETED.md`.

**Milestone 3.** A leg is profiled from what it has said rather than from the transport it
signals over; the interop fixture has rtpengine on its media path and reaches outside
Docker; this node serves the web client that drives its own harness; a browser calls a
browser through it with audio proven at both ends; `GET /api/v1/client/config` tells a
browser where to signal and what to use for ICE, with TURN credentials that expire on their
own; and the relay path is automated, `browser.sh` running a direct phase and a relayed one.
On 2026-09-30 a browser called an AthenaPhone on a real device through this node and
rtpengine, and a person heard it both ways in both directions - once a DTLS-role bug in
the rtpengine driver, which every call that actually rang had been hitting, was found and
fixed. `test/interop/UAT.md` is the record.

**Milestone 5, out of order on purpose.** The node installs, runs as a systemd service,
takes a command line including `--print-config` and `--add-user`, says on the bus that it
is alive and is deployed on a real host. Admin authentication is complete end to end -
sessions, the three auth routes, roles on every route, the users routes, and an OpenAPI
document the console runs a contract test against. `docker/up.sh` brings the whole stack up
with one command: Redis, Mosquitto, rtpengine, coturn, a seeded realm, two accounts and the
first administrator.

**Bugs, several of them found by other people.** Two from the Corvus LoRa Bridge session, a
truncated password hash that verified against almost anything and an MQTT run that could
tear down its successor. Two from the admin console session, a 401 that should have been a
403 and a TURN username carrying our own log prose. And a UDP flow that was never reaped,
found while answering T.O.M.S about the monitoring topics.

The call Milestone 3 was named for has been made and heard. What the run left behind is
the milestone's next work: a callee is still profiled by its transport when it has said
nothing yet, which is decision 3 below and still open - the `+sip.ice` in its registration
looked like the answer and is not one; and the automated browser call never rings, which
is why it missed the bug the live one found. Both are in Milestone 3 below, in order.

Milestones are in priority order and so are the items inside each one: work top to
bottom, and say why when something is taken out of turn. Milestone 5 has been, twice -
the deployment, because a node nobody can install is not one anybody will adopt, and
admin authentication, because the console is built against it and cannot go further
without it. Both are recorded that way. Move items to `COMPLETED.md` as they land, with
a note on what shipped.

## Waiting on Tom

Nothing below can move until these are settled, and each one has somebody or something
stopped against it. They are here rather than scattered through the milestones because a
session that has lost its context needs to see them first.

1. **`status_interval` in the heartbeat payload.** T.O.M.S has its Comms card live on
   FI-1 and hardcodes a 90-second staleness threshold, which is three intervals by
   coincidence rather than by contract. Adding the interval to the retained
   `nodes/<id>/status` message lets the card derive its own. The field would be
   `status_interval`, in seconds, matching the config key it comes from. T.O.M.S is
   holding for a yes or no and will use three times it, falling back to its configured
   value for nodes that do not carry it. See `docs/events.md`.

2. **Who the first user on `corvus-fi-1` is.** The node was deployed on 2026-10-01 with
   everything to that date - Tom's word that day: "THERE IS NO PRODUCTION", so deploying
   there needs no further say-so. It now serves the auth routes (`POST
   /api/v1/auth/login` answers 401 for a user that does not exist, not 404), so the
   console can log in once there is somebody to log in as. That name and password want
   choosing by a person rather than generated: either `POST /api/v1/users` with the
   configuration token already in that node's config, or `athenasip --add-user` on the
   host.

3. **A realm holding both WebRTC endpoints and plain-RTP ones.** The next item in
   Milestone 3's order, and the reason the milestone is stalled. Now that a leg's own
   profile is remembered, the only undecidable case left is the first offer towards an
   endpoint this node has never heard describe itself, and only when its transport
   misleads. Three options, none chosen: a per-account hint, offering by transport and
   re-offering the other way on a 488, or letting the first call fail and remembering.
   The live call of 2026-09-30 hit exactly this, a 488 from an AthenaPhone on TCP, and it
   was written here that the `+sip.ice` it registered with had already answered the
   question. Checked on 2026-10-01, it has not. RFC 5768 defines that one tag and it
   "indicates support for ICE": nothing about DTLS, SRTP or the profile. PJSUA adds the
   same tag by default whenever ICE is on (`PJSUA_ADD_ICE_TAGS`), and a PJSIP softphone is
   mostly plain RTP/AVP, so reading the tag as WebRTC trades AthenaPhone's 488 for theirs.
   No standard feature tag says DTLS-SRTP. What the tag can honestly do is order the
   second option - which profile to try first - or stand as a default a realm can turn
   off. The decision is as open as it was, with one more piece of evidence.

4. **`/accounts` versus `/subscribers`.** The 2026-09-25 vocabulary decision says
   subscriber; the type is `Account` and the resource is `/realms/{realm}/accounts`.
   Renaming is a breaking API change the console and the OpenAPI document follow, so it
   wants doing deliberately and in one go, or not at all. The console is building against
   `/accounts` until told otherwise. Question 5 in `docs/authentication.md`.

5. **Whether deleting a realm cascades to its subscribers.** The console asked; it builds
   against no cascade until told.

6. **Rate limiting on `POST /api/v1/auth/login`.** Recorded as unsolved in
   `docs/authentication.md` since before the endpoint existed, and now there is a reachable
   endpoint behind it. It is the reason the quickstart stack has not been put on a LAN, and
   it should be settled before `corvus-fi-1` sees traffic it did not invite. Not scheduled
   into any milestone, which is the thing to fix.

7. **What a `client` configuration token may read.** The `client` scope maps to
   `view-cluster-status`, so a token meant for a SIP client fetching its own configuration
   can also list every registration and, since 2026-10-01, every live call - who is calling
   whom. Both use the role on purpose (status, not provisioning), and the console reads
   them as a person with that role. The question is whether a client token should: a
   narrower role for `/client/config` alone would close it, at the cost of a role and a
   change to what existing tokens can do.

One more, smaller, and for the two sessions rather than for a milestone: the admin console
session's half of the browser relay phase is built and on disk in `../athenasip-admin` but
uncommitted, waiting on a word from Tom. Nothing here depends on it.

## How to prove it

`docs/testing.md` is the whole of it: the five layers, what each answers that the one
before it cannot, and the exact commands. The short version, cheapest first:

```
./build-tests/athenasip_tests    the unit suite, with Redis and MQTT set or they skip
test/e2e/run.sh [--rtpengine]    the sipp scenarios, the second with a real engine
test/interop/up.sh --rtpengine   a node to point a real client at
test/interop/browser.sh          two browsers calling, direct then relayed through coturn
test/interop/UAT.md              the call a person has to make
```

All of them pass before anything is tagged.

**The sanitizers are not on that list.** Tom's instruction of 2026-09-30: they are for
tracing a fault that cannot be pinned down otherwise, not for routine verification.
`CLAUDE.md` still carries the older, narrower rule - run them before touching the
transaction, channel or media paths - and the two do not agree; this is the live one until
`CLAUDE.md` is changed to match.

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
  provisioned list from `GET /api/v1/client/config`.
- (2026-09-21) Client failover without infrastructure. A realm should survive a node
  dying without the operator running DNS they control, which is what principle 3 asks
  for and what RFC 3263 alone does not give. The answer is the standard mechanisms
  first and an optional extension for what they do not cover, in this order: RFC 3608
  Service-Route and `GET /api/v1/nodes` (both done), the node list fed from discovery,
  RFC 5626 outbound with two flows to two nodes, RFC 3263, and last the
  `AthenaSIP-Alternate-Server` header. The M4 items carry the detail.
- (2026-09-21) `Subscriber` is `Account` **in the code**, because it would have collided
  with SUBSCRIBE (RFC 6665) the moment presence arrived. See the 2026-09-25 vocabulary
  decision, which this now sits under rather than over.
- (2026-09-25) The vocabulary, which every document, endpoint and role name follows: a
  **user** is a thing that can use the API, and the admin interface is only a client of
  the API; a **subscriber** is a thing registered on a realm to make and receive calls.
  Neither is created from the other in either direction and neither credential works as
  the other. The type is still `Account` and the resource is still
  `/realms/{realm}/accounts`; whether to rename them to match is question 5 in
  `docs/authentication.md` and is a breaking change to make deliberately or not at all.
- (2026-09-25) There is no superuser role, and no role implies another.
- (2026-09-29) `Datastore::session_delete` succeeds whether or not that hash was held, and
  the realm and account deletes still report when there was nothing there. A session is
  named by a secret the caller presented, so an answer distinguishing "that was live" from
  "that was never live" is a way to ask this node whether a token is real, one guess at a
  time, on a route anybody can reach. A realm name is not a secret. `API_VERSION` did not
  move, because the shape did not change - which is exactly why `docs/plugins.md` now says
  the version tracks shape and shape is not the whole contract.
- (2026-10-01) The proxy is not an open relay. A request out of dialog whose From is in a
  domain this node serves is authenticated as that subscriber, whoever it is calling: RFC
  3261 22.3, a 407 with Proxy-Authenticate, answered with Proxy-Authorization for that
  same account, or a reliable connection that carried an authenticated REGISTER for it. A
  From elsewhere may call into this node's domains and nowhere else (403). Requests in a
  dialog this node is on, ACK and CANCEL are not challenged. Found because the proxy had
  no 407 at all and forwarded a stranger's INVITE anywhere. What a client needs to do
  about it is its own session's work.
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
  node, rtpengine and the accounts up for it.

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
               s10, 3327,      s16, 3263   s12, 4028     (local
               5626 flows      loose route              responses)
                    |             |
                Datastore    MediaEngine       Events: observability only, never the call path
                (async)

Core = composition root + the strand
```

Everything below the transaction users is a plugin: datastores, event systems and media
engines today, routing policy and others later. They hang off the TU layer and register
through one contract, so adding a kind or an implementation touches nothing above it.

The tree matches the diagram, checked on 2026-09-30, with one box not yet built: the local UA,
which is what would let a node send a BYE to both ends of a call it decided was over.
`docs/architecture.md` describes the shape for a reader; this section is the target.

### Known deviations from the standards

Each is deliberate, each is an item below, and this list is so that none of them is
mistaken for compliance. **Re-checked against the tree on 2026-09-30** - all six still
hold, and nothing since 0.7.0 added or closed one:

- RFC 3263 is in (2026-10-01): NAPTR, SRV and A/AAAA, and the next hop for a target that
  refused a connection, answered 503 or timed out. There is no blacklisting of a server
  that failed (4.3 permits it), so the next call tries it again first.
- RFC 5626 is routing only: a binding records its flow and the fork uses it. There is no
  `+sip.instance`, `reg-id`, `Flow-Timer` or keep-alive; M4.
- Forking is serial. 16.7 allows it, and parallel forking is Parked.
- A media-anchoring node rewrites the body it forwards, which 16.6 forbids a proxy. It
  is the relay, and where it cannot anchor the message travels on untouched.
- Outbound TLS to a host this node has never heard from is refused rather than faked; M4.
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
`test/interop/UAT.md` holds the record and `COMPLETED.md` the account of what it found.
Two things it found are this milestone's next work, in this order.

- [ ] **Offer a callee that has said nothing a profile it can take.** A leg is profiled
      from what it has said, but a callee has said nothing when this node offers to it, so
      the realm decides, and on the default `FromTransport` an AthenaPhone on TCP is read
      as a desk phone and offered plain RTP/AVP - it answered 488, correctly. This is
      decision 3 under "Waiting on Tom", met on a real call, and it waits there. The
      registration carried `+sip.ice` (RFC 5768), which was first written up as the answer
      and is only a hint: it says ICE, a PJSIP softphone says it too, and nothing in a
      registration says DTLS. Two things are true whichever option is chosen. The binding
      does not keep a Contact's feature parameters today - `types::Location` holds the URI
      and nothing else of the header - so any option that reads the tag needs them stored
      first, which is a field on a type the Datastore contract hands to plugins. And until
      it is decided, a realm whose clients are all WebRTC needs `media_profiles: webrtc`
      set by hand, which is how the live call got past it.
- [ ] **Ring the automated browser call.** `browser.sh` answers within milliseconds and
      so never rang long enough to hit the DTLS-role bug the live call found; a deliberate
      delay of a few seconds before the callee answers would have caught it, and would
      catch the next thing of its shape. One knob on the spec in `../athenasip-admin`.

### After that

The first item here is the general form of the one above and needs the same decision
(number 3 under "Waiting on Tom"). The routing half of NAT handling and outbound UDP landed
on 2026-10-01; what is left of NAT is a judgement call, so read it before starting. The
harness gained a delayed offer, hold and resume, and a refused relay the same day.
**The next one that can be started without anybody is the builtin relay's
media assertion.**


- [ ] A realm holding both WebRTC endpoints and plain-RTP ones. Now that a leg's own
      profile is remembered, the only undecidable case left is the first offer towards
      an endpoint this node has never heard describe itself, and only when its transport
      misleads - a WebRTC endpoint on UDP, TCP or TLS. The options are a per-account
      hint, or offering by transport and re-offering the other way on a 488, or
      letting the first call fail and remembering. Not decided.
- [ ] NAT handling for the client side, what is left of it. Routing is in: `rport` and
      `received` are stamped on the way in (RFC 3581), a binding is reached down the flow
      it registered on, and a UDP flow the idle sweep has forgotten is still sent down -
      for a call to its binding and, through the sealed flow token, for a request in a
      dialog it is on, and a stateless response goes to `received` and `rport` whether
      or not the flow is still held. Left: a Contact rewrite policy for a client that is
      neither registered here nor in a dialog through here, which is a judgement call
      because a proxy that rewrites a Contact is changing what an endpoint said about
      itself. The node's own address behind NAT is M4.
- [ ] A harness scenario for a trunk, once a trunk is something the node can be
      configured with: an authenticated subscriber calling a number that leaves by it, and
      a call arriving from it. Today an off-node call goes wherever its Request-URI says.
- [ ] Record which rtpengine instance owns a call in the datastore so any node can
      release it; support a pool of engines with health checks. The driver is proven
      against rtpengine 9.4.0, so this is the cluster's question and not the
      protocol's; it belongs with M4 as much as here.
- [ ] Verify the rest against AthenaPhone: video, hold and resume, DTMF (RFC 4733
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

- [ ] Feed `GET /api/v1/nodes` from the discovery bus: the retained `nodes/<id>/status`
      messages carry each node's SIP and inter-node TLS addresses, which is the same
      list. `GET /api/v1/client/config` is then the bootstrap blob a web client fetches,
      the node list plus what the realm expects of it, rather than a second inventory.
- [ ] RFC 5626 outbound, the rest of it. Routing on the registered flow is done. What is
      left is what lets a client keep two flows to two nodes, which removes the
      reconnect window entirely: `+sip.instance` and `reg-id` on the binding, a flow
      token in the Path this node writes, `Supported`/`Require: outbound`, `Flow-Timer`
      and the keep-alives of section 4.4, and 430 Flow Failed for a binding whose flow
      has gone (section 11), which wants the registrar to act on it. It changes what
      identifies a binding, instance and reg-id rather than contact alone, so it is a
      Datastore contract change and an `API_VERSION` bump.
- [ ] `AthenaSIP-Alternate-Server`, the optional extension, and last because it is what
      the standards above do not cover: a client that cannot do outbound and has no DNS
      still has to learn where else to go. Four conditions, and it is not worth shipping
      without them:
      - Honoured only over a transport that authenticated the server - TLS or WSS with a
        verified certificate. Digest authenticates the client to us, not us to the
        client, so over anything else this is a redirection primitive handed to whoever
        can forge a response.
      - Negotiated by option tag, sent only to a client that advertised
        `Supported: athenasip-failover`. Unknown headers are ignored anyway, so this
        costs nothing and keeps the header off the wire for everyone else.
      - The value borrows Contact's grammar rather than inventing one: name-addr with
        parameters, `q` for preference, `expires` for how long the list is good. A stale
        list is the failure mode to design for, so the lifetime is not optional.
      - The name is vendor-prefixed because the header is ours and unregistered
        (RFC 6648 killed `X-`). If it is ever worth standardising the name changes, which
        is the other reason the option tag rather than the header name is what gets
        negotiated.
      The list itself comes from the same discovery bus as `GET /api/v1/nodes`, so this
      is a way of carrying an answer the node already has, not a new source of truth.

### The node's own address

- [ ] Public contact addresses, the rest of it. `sip.public_address` and
      `Core::advertised_address` landed in M2 because the sipp harness showed the
      wildcard bind bites on one node. What is left:
      - The public port, which is not always the local one. A node behind port
        forwarding writes its local port into both fields today, and the forwarded port
        is what the far end has to come back to. It has to be configured per transport.
      - A client on the same LAN reaching the public address depends on the router
        hairpinning, and plenty do not. So the address a node advertises depends on who
        is asking: a `localnet` list of private prefixes, the local address to anything
        inside them and the public address to everything else, for Via, Record-Route and
        Contact. `Util::is_ipv4_private` (`src/util.cpp`) and `Location.nat` are the
        groundwork already in the tree.
      The same split applies in the media plane: the builtin relay puts
      `media.builtin.public_address` in `c=`, which is one address for every audience,
      and the RTP port range has to be forwarded as a contiguous block.
- [ ] A node determines its own public address and reachability, rather than being told.
      Configuration stays and always wins, because an explicit answer beats a guessed
      one, but a node with nothing configured should work out the answer itself. Three
      sources, cheapest first:
      - From peers, for nothing. A peer that receives an inter-node request already
        stamps `received` and `rport` on the top Via (RFC 3581), which is what
        `Channel::_stamp_via` does in the receive direction. Reading them back off the
        response is a per-transport observation of what a peer actually sees, which one
        STUN answer cannot give.
      - From STUN (RFC 5389) when there is no peer yet, which is every single-node first
        start. coturn is already in the M5 compose, so the client is the only new part.
        STUN reports the mapping the router made for an outbound packet, which on a SOHO
        router doing symmetric NAT is not the port that was forwarded inbound. It gives
        the public address with confidence and the port only as a guess, so it is a
        starting point to be verified, never an answer to act on.
      - From the operator, as today.
      Then verify rather than believe: the node publishes what it thinks it is in the
      retained `nodes/<id>/status` roster, and a peer sends an OPTIONS (11.1) back to
      that address from outside. An address that does not answer is not advertised, and
      the node says so loudly instead of record-routing something unreachable.
      `athenasip check` reports what was found, per transport, and how.
      The honest failure case has to be expressible: a node behind symmetric NAT or a
      connection-pinning balancer has no address peers can reach on their own, only
      flows clients opened. Discovery must be able to answer "not directly reachable",
      and a node in that state must not advertise itself as a routable cluster peer -
      that is the case where Record-Route-the-node stops working and RFC 5626 flows are
      the only way in.

### The second node

- [ ] Inter-node listener: TLS with `verify_peer | fail_if_no_peer_cert` against the
      cluster CA; requests from cluster-CA peers are trusted for Route/Path.
- [ ] Outbound TLS. `Core::channel_connect` refuses `tls://` rather than guessing at what
      a node trusts; the cluster CA is what settles it. Until then a TLS peer has to
      connect inwards.
- [ ] Discovery: retained `nodes/<id>/status` with SIP addresses, inter-node address,
      version, capabilities; node roster maintained from MQTT; heartbeat and expiry.
- [ ] Forwarding: lookup returns owning node; INVITE routed to the peer with `Route`;
      peer delivers on the local flow; responses follow Via; Record-Route keeps both
      nodes in the dialog.
- [ ] Media ownership: the first node anchors media in rtpengine; the engine id travels
      in the call record so BYE from either node releases it.
- [ ] Call records (CDR) in the datastore: start, answer, end, participants, media
      engine, nodes. `GET /api/v1/calls`.
- [ ] `athenasip ca init` and `athenasip ca node <id>` subcommands (or `bin/athena-ca`)
      that generate the cluster CA and per-node certificates with sensible defaults, and
      `athenasip check` that validates config and connectivity to Redis, MQTT, rtpengine
      and peers. No OpenSSL incantations in the docs.
- [ ] `docker-compose.cluster.yml` with two nodes, and the sipp harness run across them.
- [ ] Chaos test: kill node A mid-registration-cycle, assert re-REGISTER on node B and
      a new call completes within one registration interval.

---

## Milestone 5 - Batteries included

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

- [ ] Admin API, part 2, what is left of it. `/api/v1/calls`, `/api/v1/calls/{call}`,
      `/api/v1/media` and `/metrics` landed on 2026-10-01. Left: hanging up a call
      (`DELETE /api/v1/calls/{call}`), which needs the node to send BYEs itself and so waits
      for the local UA in M6; call history, which is M4's CDRs; and `/api/v1/events`, an SSE
      stream bridging `nodes/#`, `account/#` and `calls/#`.
- [ ] athenasip-admin: replace the empty `src/lib/API.js` with a client generated from
      the OpenAPI document; pages for realms, accounts, registrations, live calls,
      nodes, media engines, and the JsSIP test phone pointed at the server's own WSS.
      Mostly that session's work rather than this one's, and well under way: the client is
      hand-written and speaks the whole documented API, with a contract test against
      `docs/api/openapi.yaml` passing. Generation is what is left, and every operation in
      the document now carries an `operationId` for it.
- [ ] Docs for the reader without a telecoms background. The M2 audit made every page
      true; this is the pages that do not exist: a configuration reference generated
      from the config schema, "how a call works", a clustering guide, a TLS and
      certificates guide, a media engines guide, troubleshooting.
- [ ] Plugins as shared libraries: `plugins.path` in config, scan for `.so`, `.dylib`
      and `.dll`, `dlopen`, call an `extern "C"` describe/create entry point, register
      through the contract, and refuse to load a plugin whose `api_version` does not
      match. The rule is documented plainly: build against the SDK headers with the same
      toolchain, because `YAML::Node` and `std::shared_ptr` cross the boundary. A pure C
      ABI is more portable and much more work; not the starting point.
      `athenasip plugins list` shows what loaded and why anything did not.
- [ ] Packaging: Docker image, Debian package, Homebrew formula. In-tree plugins ship
      compiled in; the packages also carry the SDK headers.
- [ ] Observability: a structured log option. Prometheus `/metrics` landed on 2026-10-01.
- [ ] Pipelining in `RedisDatastore`: the listing operations walk their index one key at
      a time because each step starts the next from its own completion. Correct, and
      slower than one MGET would be. Worth doing when a node has enough bindings for it
      to show, which the M4 chaos test is the first thing likely to produce.

---

## Milestone 6 - Conferencing, presence, web client

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
- [ ] SIP MESSAGE relay.
- [ ] Push (RFC 8599) parameters on REGISTER and a push gateway hook for AthenaPhone.
- [ ] Web client repository: video calling and conferencing, JsSIP over WSS, provisioned
      from `/api/v1/client/config`, served by AthenaSIP.
- [ ] The local UA, the fourth transaction user in the Architecture diagram. Its first
      use is tearing a lapsed call down towards both ends: on expiry this node discards
      its state, which is what RFC 4028 section 8 asks of a proxy, and a node that
      anchored media would rather tell both ends than leave them to their own timers.

---

## Parked

Kept in the tree or history, not on any milestone:

- Lua scripting (`src/script/`): not compiled, `on_message`/`send_message` are TODOs,
  `luaL_openlibs` disabled. Revisit when routing policy needs more than location lookup,
  and then as a routing-policy plugin through the plugin contract.
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
- `Util::is_ipv4` (`src/util.cpp:161`) still uses a regex and does see network data, but
  the pattern is anchored with no nested quantifiers, so it is linear and not the hazard
  the message-path regexes were. Left deliberately.
