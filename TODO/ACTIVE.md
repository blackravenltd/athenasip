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
577 tests, clean under asan and tsan.

0.6.0 landed RFC 5626 flow routing, the rtpengine driver and the media profile that
tells it which leg is the browser, and the behaviour tests that found eleven bugs in
code nothing had ever tested. 0.7.0 is the mixed-transport call: a realm says what its
calls ask of the engine, the node record-routes both interfaces and carries a flow
token in each so an in-dialog request can reach a browser, and three recorded
deviations from RFC 3261 and 3264 are closed. `COMPLETED.md` has the detail.

Since 0.7.0, on `develop` and not yet tagged: a fixture for pointing a real SIP client
at, and the sipp harness run against a real rtpengine. Both are recorded at the end of
`COMPLETED.md`.

Milestones are in priority order and so are the items inside each one: work top to
bottom. Move items to `COMPLETED.md` as they land, with a note on what shipped.

## How to prove it

Four things, cheapest first. All of them should pass before anything is tagged.

```
cmake --preset tests && cmake --build build-tests -j8
ATHENA_TEST_REDIS_URL=redis://127.0.0.1:6399 \
ATHENA_TEST_MQTT_URL=mqtt://127.0.0.1:1883 ./build-tests/athenasip_tests
```

The unit suite. Redis and MQTT skip without those, so set both or the two canonical
drivers go untested. Run it directly rather than through ctest: ctest launches a
process per test and takes eleven minutes to do what this does in eleven seconds.

```
cmake --preset asan && cmake --build build-asan -j8 && ...
cmake --preset tsan && cmake --build build-tsan -j8 && ...
```

Both sanitizers, before anything touching the transaction, channel or media paths.

```
test/e2e/run.sh                 the eight sipp scenarios, in-process relay
test/e2e/run.sh --rtpengine     the same, with a real rtpengine on the media path
```

Compliance proven rather than asserted, which is principle 2. The rtpengine run also
asserts the engine relayed the media rather than declining it, which the builtin run
cannot.

```
test/interop/up.sh              a node for a real client to be pointed at
test/interop/smoke.py           registers on UDP, TCP, TLS and WS
```

For interop. Run the smoke test before blaming a client: if it passes and the client
does not, the difference is in the client.

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
- (2026-09-21) `Subscriber` is `Account`, because it would have collided with SUBSCRIBE
  (RFC 6665) the moment presence arrived.

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

The tree matches the diagram as of 0.7.0, with one box not yet built: the local UA,
which is what would let a node send a BYE to both ends of a call it decided was over.
`docs/architecture.md` describes the shape for a reader; this section is the target.

### Known deviations from the standards, as of 0.7.0

Each is deliberate, each is an item below, and this list is so that none of them is
mistaken for compliance:

- A next hop is resolved from its URI's transport, host and port with A/AAAA only. RFC
  3263's NAPTR and SRV are not done; M4.
- RFC 5626 is routing only: a binding records its flow and the fork uses it. There is no
  `+sip.instance`, `reg-id`, `Flow-Timer` or keep-alive; M4.
- Forking is serial. 16.7 allows it, and parallel forking is Parked.
- A leg's media profile comes from the transport of its flow or from the realm, and
  there is no per-endpoint say. A realm with both browsers and SIP-over-WebSocket
  phones in it cannot have both.
- A media-anchoring node rewrites the body it forwards, which 16.6 forbids a proxy. It
  is the relay, and where it cannot anchor the message travels on untouched.
- Outbound TLS and outbound UDP to a host this node has never heard from are refused
  rather than faked; M4 and M3.
- Record-Route is written twice on every dialog-forming request rather than only where
  the interfaces differ. RFC 5658 requires the pair in that case and permits it in
  every case; the flow token in each value is what makes a call to a browser routable
  in both directions, so both are always written.

---

## Milestone 3 - WebRTC and rtpengine

Goal: AthenaPhone and JsSIP make audio and video calls to each other and to plain-RTP
endpoints, media anchored in rtpengine.

What a browser call needs from the signalling is now in: WSS, each binding reached on
the flow it registered over, an rtpengine driver, a media profile that says which leg
is the browser and a realm that can override it, and a route set that carries a flow
token so the ACK and the BYE reach an endpoint whose Contact resolves to nothing. What
is left is a real browser at the other end of it, which is the verification item at the
bottom, and the endpoint-side NAT work.

- [ ] Record which rtpengine instance owns a call in the datastore so any node can
      release it; support a pool of engines with health checks. The driver itself is
      proven against rtpengine 9.4.0 now - `test/e2e/run.sh --rtpengine` runs every
      scenario against one - so what is left here is the cluster's question rather than
      the protocol's.
- [ ] NAT handling for the client side: `rport` and `received` are already stamped on the
      way in (RFC 3581); what is missing is routing the response and the in-dialog
      request to where the request actually came from rather than where its Via and
      Contact claim, and a Contact rewrite policy for a client that cannot be told. The
      node's own address behind NAT is the M4 item, not this one.
- [ ] Outbound UDP to a host this node has never heard from. The datagram has to leave
      by the listener's own socket so the source port is the one the far end answers to,
      and that socket belongs to `UDPServer` rather than to the channel registry. Needed
      for a UDP trunk, which the interop matrix below has.
- [ ] Client provisioning endpoint: `GET /api/v1/client/config` returning WSS URL, ICE
      servers, and time-limited TURN credentials (coturn shared-secret scheme).
- [ ] Harness scenarios for what M2 did not exercise: a WSS call, a call between a
      browser and a UDP phone, a delayed offer (INVITE with no body, offer in the 2xx,
      answer in the ACK, handled in the shape and untested), hold and resume, and a
      trunk.
- [ ] A media assertion for the builtin relay to match the one rtpengine's counters
      give. `test/e2e/run.sh --rtpengine` now fails when nothing was relayed; the
      builtin run cannot tell, because the relay keeps no counters a harness can read
      and a declined description looks exactly like a relayed one from outside. The
      engine's `query` already reports idle time, so the admin API's live-calls page in
      M5 is where this gets its answer.
- [ ] Verify against AthenaPhone on UDP, TCP, TLS, WSS: register, audio call, video
      call, hold/resume, DTMF (RFC 4733 passthrough), blind transfer (REFER proxying).
      `test/interop/` is the node to point it at, and `test/interop/smoke.py` says
      whether the fixture is serving before anything else is blamed. Note that
      AthenaPhone's media is WebRTC on every transport and this fixture offers plain
      RTP, so signalling will be clean and media will not agree; that is the honest
      result and the decision is AthenaPhone's.
- [ ] Interop matrix documented: AthenaPhone, JsSIP in Chrome/Firefox/Safari, Linphone,
      a hardware desk phone, Asterisk as a trunk.

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
- [ ] RFC 3263, properly: NAPTR then SRV then A/AAAA when resolving a next hop.
      `Core::channel_connect` does the last of those and says so. It is the standard way
      a client finds a realm and the standard way this node finds a trunk, and it is
      what makes a node list unnecessary for clients that can do it. The resolver is
      hand-rolled per the dependency rule, which is why it is a step of its own rather
      than a line in another one.
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

- [ ] `docker-compose.yml`: athenasip, redis, mosquitto, rtpengine, coturn, seeded
      realm and two accounts, admin UI on 8080, WSS on 9443, TLS on 5061.
- [ ] Command line. `main.cpp` reads no arguments at all today: `athenasip --version`,
      which `CONTRIBUTING.md` and `CLAUDE.md` both describe, does not exist, and the
      config comes only from the search path. `--version`, `--config <path>` and
      `--print-config` for the effective values, and the ten-line config that
      `docs/configuration.md` already shows as the whole of what is needed.
- [ ] Admin API, part 2, the live registries: `/api/v1/calls` (live and history,
      hangup), `/api/v1/media` (engines, health), `/api/v1/events` (SSE stream bridging
      `nodes/#`, `account/#`, `calls/#`). These read Core's own state and need
      `call_on_strand`, which nothing in provisioning does. `/api/v1/registrations` and
      `/api/v1/nodes` are done.
- [ ] athenasip-admin: replace the empty `src/lib/API.js` with a client generated from
      the OpenAPI document; pages for realms, accounts, registrations, live calls,
      nodes, media engines, and the JsSIP test phone pointed at the server's own WSS.
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
- [ ] Observability: structured log option, Prometheus `/metrics` on the admin port.
- [ ] IPv6 literals in a config URL. `redis://[::1]:6379` parses to nonsense because the
      authority is split on the first colon. RFC 3986 3.2.2 puts the literal in brackets
      for exactly this reason, so the fix is to read a bracketed host whole in
      `types::URL`, and to put the brackets back in `to_string` when the host holds a
      colon. Small, but it changes what `host` hands the datastore and media drivers.
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
- `Util::is_ipv4` (`src/util.cpp:162`) still uses a regex and does see network data, but
  the pattern is anchored with no nested quantifiers, so it is linear and not the hazard
  the message-path regexes were. Left deliberately.
