# AthenaSIP - Active Work

Milestone 1 is complete. Work happens on `develop`; `main` carries the last release, and
`0.5.0` is the current one. Line numbers refer to the current tree; update them as files
move.

What M2 builds on, all landed in M1: the header, URI, identity and message model follow
RFC 3261; `Core` runs on a single strand with no locks; `memory://` and `redis://` are
the datastores and both implement the full interface; `MediaEngine` and a builtin RTP
relay driver are in; `Call` is multi-party; all four RFC 3261 section 17 transaction
state machines exist and are wired in behind a matcher, with `Registrar` and `Proxy` as
the transaction users.

**Milestone 2 is essentially done as of 0.5.0.** Steps 1 to 6 and 8 to 10 are closed, and
what is left in each is recorded against it as work that defers itself to M3, M4 or the
Parked list. The sipp harness passes all eight scenarios, which is what principle 2 asks
for: compliance proven rather than asserted. 472 tests, clean under asan and tsan, the
Redis suite verified against a real server.

What is left of step 7 is the per-connection flow identity on a binding, which is a plugin
contract change and wants to share an `API_VERSION` bump with the one step 2 left
outstanding. Step 4's RFC 3263 and step 5's remaining items are M3 and M4 by their own
terms.

A node now decides for itself when a call it is holding is over, which nothing before
0.5.0 did: `sip.media_timeout` reads the relay, `sip.session_expires` offers a timer to a
call that asked for none, and `sip.max_call_duration` is the blunt backstop. All three
release the call and send no BYE, which is what RFC 4028 section 8.3 allows a proxy and
no more.

An architecture review on 2026-09-18 compared the Principles, the tree and the RFCs.
Its findings are merged into Milestone 2 below, which is ordered by priority: work the
steps top to bottom. The target layering is under Architecture.

Move items to `COMPLETED.md` as they land, with a one-line note on what shipped.

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

## Decisions (2026-09-17)

- Nodes proxy SIP to each other (Path / Record-Route / Via). MQTT is for events,
  presence, discovery and observability, never on the call setup path.
- Inter-node authentication is mutual TLS from a cluster CA on a dedicated listener.
  Cluster-CA client cert = peer node (Route trust); anything else = endpoint or trunk
  (Digest).
- Cluster discovery: retained MQTT `nodes/<id>/status` messages carrying the node's SIP
  and inter-node TLS addresses; Redis holds registration ownership with TTL.
- MySQL, PostgreSQL and SQLite drivers are deleted. `memory` + `redis` for datastores,
  `local` + `mqtt` for events and `builtin` + `rtpengine` for media are what ship
  in-tree and are tested in v1. The architecture privileges none of them: every driver,
  in-tree or not, registers through the same plugin contract, and DynamoDB, NATS, Kafka
  or anything else is a plugin someone can write, not a roadmap item the core carries.
- The plugin contract is a stability promise (2026-09-18). It is versioned, it is async
  from its first version because it cannot be made async later without breaking every
  plugin built against it, and a plugin receives its own YAML root plus read access to
  the system config. Plugins are compiled in for now and become shared libraries with
  autodiscovery later, through the same contract.
- rtpengine (ng protocol) is the flagship media engine. `RTPProxyClient` (rtpproxy
  text protocol) may remain as a driver but nothing is built on it.
- `MediaEngine` carries capabilities: `bridge`, `conference`, `record`, `transcode`.
  It is not a two-party SDP rewriter.
- `Call` is multi-party from the start (participants, optional conference focus).
- The transaction layer is replaced by the four RFC 3261 section 17 state machines.
  `is_reliable()` only disables timers A/E/G and shortens D/K.
- WSS is required (browsers need a secure origin); WS stays for local development.
- Conferencing is RFC 4579 focus routing. FreeSWITCH `conference` is the first focus;
  an SFU (Janus / mediasoup / LiveKit, or rtpengine publish/subscribe) comes later
  behind the same URI scheme.
- Lua scripting stays parked and out of the build path until there is a real use. When
  it returns it is a routing-policy plugin, not a core feature.
- Core runs on a single strand. Servers post into it.

## Architecture

The target layering follows RFC 3261's own: transport, transaction, transaction user.
`Core` is the composition root and owns the strand; it does not process SIP itself.

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

What the review found the tree does instead, and which step below fixes it. Steps 1 to
5 have landed and their findings have moved to `COMPLETED.md`; what is left:

- For a browser over WS the Contact URI is unroutable, so RFC 5626 flow routing and
  WSS are prerequisites for the first WebRTC call, not cluster features. (Steps 7 and
  M3.)

---

## Milestone 2 - A standards-compliant call on one node

Goal: INVITE / 18x / 200 / ACK / BYE / CANCEL between two subscribers on one node over
UDP, TCP, TLS, WS and WSS, with plain RTP through the builtin engine, provisioned
through the admin API, verified by sipp.

Steps are in priority order. Each one is the prerequisite of the ones below it, or the
thing that would cost the most to leave.

### Step 1 - Transaction users, and the wiring (RFC 3261 sections 10, 16, 17)

Done on 2026-09-18; see `COMPLETED.md`. The matcher, the four machines wired into
`Core::process_message`, `Registrar`, `Proxy`, the event-bus INVITE path deleted and Via
moved out of `Channel`. What it deliberately left for later, so it is not lost:

- [x] RFC 2543 fallback transaction matching, done on 2026-09-21. A request whose topmost
      Via carries no branch, or one with no magic cookie, is keyed by 17.2.3's fallback
      tuple - Request-URI, From tag, Call-ID, CSeq number, the whole topmost Via and the
      method - instead of by branch and sent-by. Both forms are strings in the one table,
      so nothing above the matcher knows which kind it holds. The To tag is the one field
      of the rule left out, with the reason recorded at `_legacy_key`: it would stop an
      ACK ever matching the INVITE that has no tag, and the case it exists for is already
      covered because a 2xx terminates the server transaction (17.2.1). A branchless
      request used to be answered 400 for want of an identifier.
- [ ] Serial forking tries each binding in turn but sends every attempt down the one flow
      the subscriber registered on, because that is all a single node knows. Per-binding
      flow routing is RFC 5626 in M3, and `Location.flow_id` exists for it.
- [x] 423 Interval Too Brief with `Min-Expires`, done on 2026-09-21. `Realm` grew a
      `registration_minimum` beside its `registration_timeout`, provisioned over the API
      and zero by default, because the RFC's own advice in step 7 is that a registrar
      should accept brief registrations unless the refreshes are costing it something.
      When it is set, the three conditions are all of them: greater than zero, under an
      hour, and under the minimum - a zero expiry is a removal and an hour is never too
      brief. The refusal quotes the minimum in `Min-Expires`, which is what makes it
      something a client can act on, and registers none of the contacts.

### Step 2 - Plugin contract v1

Done on 2026-09-19; see `COMPLETED.md`. One registry, one `Plugin` base, the
configuration hand-off, and `Datastore` and `MediaEngine` async in the contract.

What it deliberately left, so it is not lost:

- [ ] Pipelining in `RedisDatastore`: the listing operations walk their index one key at
      a time because each step starts the next from its own completion. Correct, and
      slower than one MGET would be. Worth doing when a node has enough bindings for it
      to show.
- [x] `EventSystem` takes the contract's `Executor` and handlers, done on 2026-09-21 as
      half of contract v2. The fire-and-forget `publish(name, message)` stayed, because
      the bus is observability and is never on the call setup path.

### Step 3 - SIPUri is a real URI (RFC 3261 section 19.1)

Done on 2026-09-20; see `COMPLETED.md`. Structured parameters and headers, escaping,
19.1.4 equivalence, and `realm` renamed to `host`. Hand-written, no regex: the grammar
is a sequence of splits on delimiters that cannot appear unescaped in what they delimit.

What it deliberately left, so it is not lost:

- [x] `SIPIdentity::parse` is hand-written, done on 2026-09-20. The backtracking regex
      it replaces ran over every To, From and Contact that arrived and could not express
      a quoted display name containing `<` or `;` at all. `star` gives a Contact of `*`
      its own home (20.10) and the registrar no longer recognises it by shape.
- [x] `types::URL` (`src/types/url.cpp`) is hand-written, done on 2026-09-21. The last
      regex worth replacing, and it was hiding two things: a port that was not digits
      was read as part of the path and the URL called valid, so a typo in a datastore
      address surfaced as a connection failure much later; and `to_string` dropped the
      port of any scheme with no well-known one, which is every scheme this project
      invents. Parsing into an object now clears what went before, and the default-port
      lookup is case-insensitive the way RFC 3986 3.1 says a scheme is.
- [ ] IPv6 literals in a config URL. `redis://[::1]:6379` parses to nonsense - it always
      has - because the authority is split on the first colon. RFC 3986 3.2.2 puts the
      literal in brackets for exactly this reason, so the fix is to read a bracketed
      host whole, and to put the brackets back in `to_string` when the host holds a
      colon. Small, but it changes what `host` hands the datastore and media drivers,
      so it is its own piece of work.
- [ ] `Util::is_ipv4` (`src/util.cpp:153`) still uses a regex and does see network data,
      but the pattern is anchored with no nested quantifiers, so it is linear and not
      the same hazard. Left deliberately.

### Step 4 - Proxy core, the rest of section 16

Done on 2026-09-20; see `COMPLETED.md`. Loop detection, Route and Record-Route
processing, in-dialog transit, stateless response forwarding and CANCEL forwarding.

What it deliberately left, so it is not lost:

- [ ] RFC 3263: a next hop is resolved from the URI's own transport, host and port, with
      the scheme's defaults for what it does not say. NAPTR and SRV are a step of their
      own and are what a cluster and a trunk both need.
- [x] Outbound flows, done on 2026-09-20 as part of step 7. `Core::channel_connect`
      opens one to a hop this node has none to, rather than answering 480.
- [ ] Double Record-Route (RFC 5658), for a call whose two ends are on different
      transports. One value naming the outbound flow is right for a call that is UDP to
      UDP or WSS to WSS, which is what M2 tests; a browser calling a desk phone needs two
      values and needs both stripped on the way back. Prerequisite for mixed-transport
      calls in M3, not for M2.
- [ ] Parallel forking. The fork is serial: one branch at a time, best response wins.
      16.7's response context is written to hold more than one branch, and the CANCEL
      path already walks it, so the change is in `_forward_next` rather than in the
      shape. Parked deliberately - it is listed under Parked.
- [x] Timer C (16.6 step 11), done on 2026-09-21. The branch that answers 180 and never
      stops is now given up on. Nothing else was watching it: the first provisional
      response moves the INVITE client transaction to Proceeding and cancels timer B
      (17.1.1.2), so from the first 100 Trying onwards the branch had no bound at all
      and held the caller, the response context and the dialog for as long as the node
      ran. The timer hangs off the response context, which is where 16.6 puts it, and is
      reset by a 101 to 199 (16.7 step 2) but not by a 100. On firing it follows 16.8:
      a branch that has answered provisionally is sent a CANCEL and given one more
      interval to answer it, and one that ignores that has its client transaction
      terminated and is treated as though a 408 came back. `sip.timers.c_invite_proxy_ms`
      configures it, and a value at or below the RFC's three-minute floor is refused.

### Step 5 - Dialogs (RFC 3261 section 12)

Done on 2026-09-20; see `COMPLETED.md`. `types::Dialog`, the `Dialogs` observer,
`Call` hanging off it, and RFC 4028 session expiry.

What it deliberately left, so it is not lost:

- [x] RFC 4028 section 8, the proxy's own say in the negotiation, done on 2026-09-21.
      `Config::sip_session_min_se` was configuration nothing read, and the node accepted
      whatever the two ends agreed. 8.1 now applies to every INVITE and UPDATE: an
      interval below the minimum is answered 422 Session Interval Too Small with `Min-SE`
      when the caller advertises `Supported: timer`, and raised on the way through when
      it does not, because a 422 a caller cannot read would only fail the call. A `Min-SE`
      already in the request is raised and never lowered, the `refresher` parameter is
      never touched, and a request that asked for no interval is still not given one -
      that is the next item and a policy decision. 8.2 covers the other end: when the
      caller asked for a timer and the callee answered without one, the 2xx gains the
      remembered interval with `refresher=uac` and `Require: timer`, before the dialog
      tracker reads it rather than after, or this node would watch nothing while the
      caller refreshed on an interval this node handed it. The config floor of 90 the RFC
      sets is enforced.
- [x] A stuck call is cleaned up by something other than a restart, done on 2026-09-21,
      and not by the session timer. A call between two endpoints that never offered one
      has no interval and never lapses, and inserting `Session-Expires` only helps where
      the far end implements RFC 4028 - which is precisely not the case that leaks. The
      media plane answers it instead: `RTPRelaySet` records when it last carried a packet,
      the engine reports the shortest idle of a call's relays as `idle_seconds` through
      `query()`, and `sip.media_timeout` (300s, 0 off) is what the sweep on `Core` acts
      on. RTCP counts with RTP so hold and silence suppression do not read as dead, and
      RFC 4028 section 8.3 is followed to the letter: the node releases the call and sends
      no BYE, because it is on the path of the dialog and not an end of it.
- [x] Inserting `Session-Expires` where a call offered none, 8.1's other half, done on
      2026-09-21. `sip.session_expires`, 1800 by default per section 4's recommendation
      and 0 to leave such a call alone. It covers the gap the media sweep cannot: a call
      whose media goes end to end is invisible to the sweep, and this reaches any call
      whose far end implements RFC 4028. Safe by construction - section 9 Table 2 has a
      timer-aware UAS take the refreshing itself when the UAC cannot, and 8.2 leaves the
      call with no expiration at all when neither end does, which is where it started. An
      interval already in the request is untouched, an inserted one is never below a
      `Min-SE` the request carried, and no `refresher` is ever named: 8.1 forbids it.
- [x] `sip.max_call_duration`, off by default, done on 2026-09-21. The backstop for the
      call neither of the others reaches: media end to end, and two endpoints that have
      never heard of RFC 4028. Measured from the dialog's confirmation on the steady clock
      for the same reason the session deadline is. The sweep on `Core` does both questions
      now, and the cheaper one - how long the call has been up - runs first and needs no
      engine.
- [x] `sip.require_session_timer`, off by default, done on 2026-09-21. RFC 4028 8.1 allows
      `Require: timer` and calls it NOT RECOMMENDED in the same breath, and the reason is
      concrete: an endpoint that does not implement the extension answers 420 Bad
      Extension, so the call fails outright rather than going without an expiry. 8.1 also
      says to add it only where the caller said nothing about session timers, which is
      what the code does. Documented with the failure mode spelled out rather than the
      option alone.
- [ ] Tearing a lapsed call down towards the endpoints. On expiry this node discards its
      state, which is what RFC 4028 section 8 asks of a proxy; sending a BYE to both ends
      would be acting as a user agent in a dialog it only sits on the path of. When the
      local UA arrives (the fourth transaction user in the Architecture diagram), it is
      worth revisiting, because a node that anchored media would rather tell both ends.
- [ ] Forking creates one dialog per answering branch (12.1) and the model holds them,
      but serial forking means only one branch is ever outstanding, so the second one is
      untested. Parallel forking is what would exercise it, and that is Parked.

### Step 6 - Media on the signalling path

Done on 2026-09-20; see `COMPLETED.md`. Offer and answer on the way through the proxy,
release when the dialog ends, flags read from the description, and a builtin relay that
actually bridges.

What it deliberately left, so it is not lost:

- [ ] Media policy. Anchoring happens whenever an engine is configured, and an engine
      that declines means the description travels on untouched and the media goes end to
      end. That is the right failure for a proxy, but it is a policy decision with
      nowhere to say it: `anchor` or `passthrough` per realm is an M3 item and this is
      the same knob.
- [x] The `o=` line carries this node's address, done on 2026-09-21. Every `c=`, `m=`
      port and `a=rtcp` was rewritten and the `o=` was not, so a node anchoring media so
      that neither end learns the other's address handed one of them away in it anyway.
      RFC 8866 section 5.2 allows the substitution in as many words - "for privacy
      reasons, it is sometimes desirable to obfuscate the username and IP address of the
      session originator" - on the condition that the field stays globally unique, so the
      username and session id the endpoint chose are kept and only the address, nettype
      and addrtype are replaced. Those two are what carry the uniqueness. The version is
      the endpoint's as well; incrementing it is the next item.
- [ ] RFC 3264 section 8's version rule: an offer that changes the description must
      increment the `o=` version, and a re-offer this node rewrote does not. It matters
      once a re-INVITE changes the stream rather than repeating it, which is hold and
      resume in M3.
- [ ] The delayed offer - an INVITE with no description, its offer in the 2xx and the
      answer in the ACK - is handled in the shape but untested. No endpoint in M2 sends
      one; a sipp scenario in step 9 is what would prove it.
- [ ] A conference through the builtin relay. Three legs on one latching port would
      forward each to the other two, which works and is not mixing, so the driver still
      advertises `bridge` only. A real focus is RFC 4579 in M6.

### Step 7 - Transports

- [x] WSS listener, done on 2026-09-20. Beast websocket over `ssl_stream`, one listener
      class for both, `websocket.tls` with its own certificate and key, and one TLS
      context loader shared with the TLS SIP listener.
- [x] Outbound flows, done on 2026-09-20. `Core::channel_connect` resolves and dials a
      hop this node has none to, bounded by `sip.connect_timeout_ms`, filed under both
      the address it reached and the name it was dialled by. This was step 4's leftover
      and it is what steps below stand on.
- [x] TCP fallback for UDP requests over 1300 bytes (18.1.1), done on 2026-09-20, with
      the top Via rewritten and a fall back to UDP when TCP is refused.
- [x] Plugin contract v2, done on 2026-09-21. The flow identity and the `EventSystem`
      realignment landed in one `API_VERSION` bump rather than two, so a plugin author
      migrates once. The reason is recorded above the constant in `src/plugins/plugin.h`.

      - `Datastore::subscriber_register` takes the binding as a `types::Location` in
        place of the `contact` + `path` pair, so the flow and the node holding it can be
        recorded. The store still fills `subscriber_id`, `registered_at`, `expires_at`
        and `nat`; everything else on the binding is the caller's. Redis writes
        `flow_id` and `node_id` only when they are set.
      - `Channel::flow_id()` is `transport://host:port`, taken once at construction and
        built by `Core::channel_key`, which is now the only place that key is spelled:
        `Channel::start`, `Channel::close`, `Core::channel_find` and
        `Core::channel_connect` all go through it. A key built two ways is a lookup that
        silently misses.
      - `Core::subscriber_register` keeps its own signature and builds the `Location`:
        contact and path from the registrar, `node_id` from the config, `flow_id` from
        the channel the REGISTER arrived over. The registrar's call site is unchanged.
      - `EventSystem` now has the shape `Datastore` and `MediaEngine` have:
        `connect(Executor, StatusHandler)`, a synchronous `close()`, `is_connected()`,
        and `publish` / `subscribe` / `unsubscribe` / `unsubscribe_all` on the contract's
        executor and handlers. `subscribe` hands the `Subscription` back through its
        handler, because a broker has to be asked before the subscription exists, and
        the MQTT driver drops one the broker refused rather than leaving the caller a
        handle to nothing. The fire-and-forget `publish(name, message)` stays: the bus
        is observability and is never on the call setup path. `main.cpp` connects it
        through the `connect_and_wait` helper it already used for the other two.
      - `tests/helpers/sync_event_system_helper.h` is the blocking view of a bus, as
        `SyncDatastore` is of a store. The tests cover the binding recording the flow it
        was learned over, that flow id being exactly what `channel_find` answers to, a
        binding learned over no channel recording no flow, and the flow and node making
        the round trip through Redis.
- [x] The transport's threading rule, done on 2026-09-21. `Connection::executor()` is the
      executor a connection's stream belongs to, and `Channel` hands every operation on
      the stream over to it: starting a read, starting a write, and the teardown in
      `close()`. A socket is not safe for two threads and each server runs its own
      io_context on its own thread, so the Core strand reaching in was a second thread
      inside the stream. `UDPServer` already worked this way and says so in its own
      comment; TCP, TLS and WebSocket did not.

      Found by tsan as a data race in `WebsocketConnectionFor::is_open()`, called from
      `Channel::close()` while beast's read op wrote the same field finishing the teardown
      the far end started. Measured before and after on
      `WebsocketServerTest.CarriesSipWithoutTls`: 12 failures in 30 runs at the baseline,
      0 in 30 with the fix. Three tests pin the rule itself - the mock connection can be
      given a strand of its own and records whether each call arrived on it.
- [x] A write queue on the channel, done on 2026-09-21. One write in flight at a time,
      the rest queued on the strand, and a short write resumed from where it stopped -
      `async_write_some` is not obliged to take the whole buffer, and nothing was
      checking how much it took. `Channel::write` was a declaration with no definition;
      it is now the way raw bytes go out, and what the tests drive.
- [ ] Outbound TLS. `channel_connect` refuses `tls://` rather than guessing at what a
      node trusts; the cluster CA decision in M4 is what settles it. Until then a TLS peer
      has to connect inwards.
- [ ] Outbound UDP to a host this node has never heard from. The datagram has to leave by
      the listener's own socket so the source port is the one the far end answers to, and
      that socket belongs to `UDPServer` rather than to the channel registry. Needed for a
      UDP trunk, not for anything in M2.
- [x] Connection reuse is tested, done on 2026-09-21: a second in-dialog request goes
      out on the flow the first one used, and the registry still holds one channel for
      that hop. Opening a second connection per request would leave a node holding one
      socket per request, and for a client behind NAT the new one would not reach it at
      all.

### Step 8 - Admin API, part 1 (provisioning)

Done on 2026-09-21. Realms, accounts and registrations under `/api/v1`, with the health
and node endpoints the harness and a web client need.

- [x] OpenAPI 3 document at `docs/api/openapi.yaml`, versioned under `/api/v1`.
- [x] Bearer tokens with `admin` and `client` scopes, from `http.api.tokens`. A route
      names the scope it needs where it is declared, so adding one cannot accidentally
      leave it open, and a request with no matching token is refused - an API enabled
      with no tokens is one nobody can call.
- [x] `GET/POST/PUT/DELETE /api/v1/realms` and `/api/v1/realms/{realm}/accounts`, HA1
      computed here from the password and never given back, and `GET
      /api/v1/registrations`. The datastore's create/update split is what lets these
      answer 409 rather than overwrite.
- [x] JSON body parsing and one error envelope: a code a client branches on and a
      message a person reads.
- [x] `StaticMiddleware` path from `config->http_files_path`.
- [x] The admin API is off the strand: provisioning goes to the datastore with the API's
      own executor. The middleware chain had to learn to keep its session alive first,
      because it only ever held one for a middleware that answered inline.

      What is left for part 2, with the live registries: reads of Core's own state -
      calls in progress, channels open - which do need `call_on_strand`, and nothing in
      provisioning does.

### Step 9 - Test harness

- [x] `test/e2e/` with sipp scenarios, written on 2026-09-21: REGISTER with Digest, a
      wrong password, a retransmitted REGISTER, INVITE/180/200/ACK/BYE, CANCEL after
      180, 486, 408 on timer B, and a call with RTP through the builtin relay. Each
      asserts on what the RFC requires rather than on what the node currently does, and
      `run.sh` provisions the realm and the accounts over the admin API first, which is
      the same path an operator uses.

      Run on 2026-09-21 against sipp 3.7.3: **4 of 8 pass** - `register`,
      `register-wrong-password`, `register-retransmit` and `invite-timeout`. Digest
      authenticates end to end against a real client, and it picks the SHA-256 challenge
      rather than the MD5 one, so the RFC 8760 ordering worry was unfounded: sipp is
      built with SHA256 support and follows section 2.4.

      Four harness bugs were fixed getting there, and each is worth knowing:

      - `run.sh` computed the repository root as `test/` rather than the root.
      - sipp expands a `[field]` before it reads the `[authentication]` keyword around
        it, so the nested form emitted `alice[authentication username= password=...]` -
        a header line with no colon, which the node correctly answered 400. The
        credentials go on the command line now.
      - sipp runs as PID 1 in every container and builds its branch from the pid and the
        call number, so two scenarios in a row sent the same branch from the same
        address. The node treated the second as a retransmission and replayed its last
        answer, which looked exactly like a wrong password being accepted. Each scenario
        gets its own source port now.
      - The port allocator was a function whose result was captured with `$(...)`, so the
        counter was incremented in a subshell and every scenario got the same port.

      **All eight scenarios pass as of 2026-09-21**, including the RTP one. What the four
      call scenarios had been failing on, since each is worth knowing:

      - The node wrote `0.0.0.0` into its own Via and Record-Route on a UDP flow, both
        being built from `connection->local_endpoint()` with the listener on the
        wildcard. This was the one real server bug of the four. `Core::advertised_address`
        now answers it for all three of Via, Record-Route and Service-Route, and
        `channel_register` files the advertised address as one of this node's own, or the
        Record-Route it wrote comes back as a Route it does not recognise and 16.3.4
        catches the BYE as a loop. The rest of the M4 "public contact addresses" item -
        the localnet split, per-transport public ports, and working the address out
        rather than being told - is untouched and still M4.
      - The callee scenarios registered and then waited for an INVITE in one sipp
        scenario, and sipp binds a scenario instance to one call: the INVITE arrives with
        the caller's Call-ID and was discarded as unmappable. Registration is its own
        short run now, on the port the UAS run then listens on.
      - `cancel_after_180.xml` built the CANCEL with `[branch]`, which sipp derives per
        message, so it named a transaction the node had never seen - and the node was
        right to say so (RFC 3261 9.1 requires the CANCEL's Via to be identical to the
        INVITE's, branch included). The branch is read back off the 180, whose top Via is
        the one that went out, and used for the CANCEL and for the ACK to the 487, which
        17.1.1.3 requires to match as well.
      - Every scenario gives its callee a port of its own, and a REGISTER from a new port
        is a new binding rather than a replacement, so the second scenario to run had the
        node forking to the first one's dead port. Each pair now clears the account's
        bindings first with a `Contact: *` and `Expires: 0` (10.2.2) - from a port of its
        own, because sharing the UAS port made the two runs one transaction under 17.2.3
        and the second REGISTER came back answered with the first one's response.
      - `invite_media.xml` named the G.711 capture at sipp's upstream path; Debian's
        package puts it in `/usr/share/sip-tester/`.

      `run.sh` now writes the node's own log to `results/node.log` before it tears the
      containers down. Without it a failing scenario is two sipp traces and a guess.

      **One deviation the harness surfaced and nobody has fixed**: a CANCEL that matches
      no transaction is answered 200 and dropped. RFC 3261 16.10 says a proxy that finds
      no response context "MUST statelessly forward the CANCEL request", because the
      request it names may have been forwarded statelessly too. Worth doing with the
      stateless path in M3.
- [x] `Dialog` unit tests, done with step 5 on 2026-09-20. `tests/dialogs_test.cpp` has
      19 of them, RFC-derived in the same style as the transaction tests: the dialog the
      2xx establishes, each end's target and sequence, re-INVITE moving the target, BYE
      from either end, CANCEL before and after answer, sips over TLS, and the RFC 4028
      session timer. This item was written before they existed.
- [x] `docker-compose.test.yml`: one node and two sipp containers on a network of their
      own, with fixed addresses because a callee has to register at an address the node
      can route back to. Done on 2026-09-21 and running. The node image builds Boost from
      source because Debian ships 1.83 and this needs 1.87 or newer; it is its own layer
      and cached after the first build.
### Step 10 - Smaller items surfaced by the review

- [x] Digest with SHA-256 (RFC 8760) alongside MD5, done on 2026-09-21. An account
      carries a credential per algorithm, both computed while the password is in hand
      because neither can be derived from the other. The registrar challenges with
      SHA-256 then MD5 and checks against whichever the client answered with; an account
      imported as a bare MD5 hash is challenged again rather than let in. Sending the
      algorithm at all needed the Authorization serialiser to stop quoting tokens.
- [x] `Subscriber` is `Account`, decided with Tom and done on 2026-09-21. It would have
      collided with SUBSCRIBE (RFC 6665) the moment presence arrived in M6. The Datastore
      contract kept its shape and changed its vocabulary, so `API_VERSION` is 3; the
      Redis keys moved with it, and the event topic is `account/<uri>/status`.
- [x] `RTPProxyClient` is out of the build path, done on 2026-09-21, the way Lua is: it
      stays in the tree as the start of an rtpproxy driver and is not compiled until
      someone writes one.
- [x] `docs/architecture.md` rewritten on 2026-09-21 from the Architecture section
      above: the strand and what is not on it, the transport layering, the plugin
      registry with the two implementations per kind, the cluster shape and the media
      model. It says in as many words that DynamoDB, NATS and Kafka are things the
      contract makes possible rather than things the core plans to build, and it points
      at `docs/plugins.md`.
- [x] The rest of the documentation audited on 2026-09-21. `design.md` was still right
      about the transport layering and wrong about everything that has happened since, so
      it gained the threading rule, the flow id and the write queue, and its three broken
      source links were fixed. `goals.md` was an empty file and is gone. `scripting.md`
      described a Lua engine that is not compiled; it now says so, and says what routing
      policy comes back as. `modules/` no longer exists. `quick_start.md` told the reader
      to install rtpproxy and log in as accounts that never existed; it is now the actual
      quick start, provisioning over the admin API. `README.md` claimed TLS-only defaults
      the shipped configuration does not set and listed WebSockets as a future feature
      two milestones after they landed.

---

## Milestone 3 - WebRTC and rtpengine

Goal: AthenaPhone and JsSIP make audio and video calls to each other and to plain-RTP
endpoints, media anchored in rtpengine.

- [ ] `RtpengineMediaEngine` (`rtpengine://host:port`): ng protocol (bencode over UDP),
      `offer`, `answer`, `delete`, `query`, `start recording`, `stop recording`.
      Capabilities: `bridge`, `record`, `transcode`; `conference` via publish/subscribe
      in M6.
- [ ] Per-call flags derived from the SDP, not the transport: `ICE=force`/`remove`,
      `DTLS=passive`, `SDES`, `rtcp-mux-offer/accept`, `transport-protocol` mapping
      (RTP/AVP <-> UDP/TLS/RTP/SAVPF).
- [ ] Media policy per realm: `anchor` (default) or `passthrough` for WebRTC to WebRTC.
- [ ] Record which rtpengine instance owns a call in the datastore so any node can
      release it; support a pool of engines with health checks.
- [ ] RFC 5626 outbound: registrations record the node id and flow; requests to a
      WebSocket or NAT'd client go down the registered flow. `reg-id`/`instance`
      parameters, `Flow-Timer`. This is a prerequisite for the first WebRTC call, not
      a cluster refinement: a browser's Contact URI has nothing listening behind it,
      so the flow is the only route to it. `Location.flow_id` exists for this and
      nothing writes it yet.
- [ ] NAT handling for UDP/TCP endpoints: `rport`, `received`, Contact rewrite policy.
- [ ] Client provisioning endpoint: `GET /api/v1/client/config` returning WSS URL, ICE
      servers, and time-limited TURN credentials (coturn shared-secret scheme).
- [ ] Verify against AthenaPhone on UDP, TCP, TLS, WSS: register, audio call, video
      call, hold/resume, DTMF (RFC 4733 passthrough), blind transfer (REFER proxying).
- [ ] Interop matrix documented: AthenaPhone, JsSIP in Chrome/Firefox/Safari, Linphone,
      a hardware desk phone, Asterisk as a trunk.

---

## Milestone 4 - Cluster

Goal: two nodes, one Redis, one Mosquitto, one rtpengine; a subscriber on node A calls
a subscriber on node B; either node can die and re-registration recovers service.

Decided 2026-09-20, on whether the nodes can sit behind a load balancer. They can, and
the shape it forces is this:

- A binding shares; a flow does not. The Redis row is readable by any node, but the
  socket a TCP, TLS, WS or WSS client registered on lives on one node. A node that
  reads a binding it does not own forwards to the node that does. `Location.node_id`
  and `Location.flow_id` are what that turns on.
- Connection-oriented transports balance cleanly: the balancer pins a connection to one
  node for its lifetime, which is the affinity SIP wants, and a node dying drops the
  socket so the client reconnects and re-REGISTERs. UDP does not: an L4 balancer cannot
  see Call-ID, so a retransmission can land on a node holding no transaction state.
  Either front UDP with a SIP-aware dispatcher or do not balance it.
- TLS is passed through, never terminated at the balancer: the cluster-CA peer
  certificate is what distinguishes a peer node from an endpoint.
- Record-Route names the node, not the balancer's address. In-dialog requests then come
  straight back to the node that anchored the media, which is coherent with "in-flight
  dialogs on a dead node do not survive" (Principle 1) and needs no dialog replication.
  The alternative - Record-Route the VIP and look up dialog ownership on every hop - is
  full dialog-state sharing, which is parked. This means every node needs a
  client-reachable address of its own, as well as the shared one.
- RFC 3263 SRV is the failover mechanism for SIP endpoints and needs no balancer at all;
  browsers cannot use it, so they get a balancer or a provisioned list from
  `GET /api/v1/client/config`. RFC 5626 outbound with two flows to two nodes is the
  strongest form of this and removes the reconnect window entirely.

### Client failover without infrastructure

Decided with Tom on 2026-09-21. A realm should survive a node dying without the operator
having to run DNS they control, which is what principle 3 asks for and what RFC 3263
alone does not give. The answer is the standard mechanisms first and an optional
extension for what they do not cover, in this order:

- [x] RFC 3608 Service-Route, done on 2026-09-21. The 200 OK to REGISTER carries this
      node, on the flow the REGISTER arrived over, with `lr`. It names
      `sip.public_address` when one is set: the sipp harness received
      `<sip:0.0.0.0:5060;lr>` on a UDP flow, which is a route no client can use. It tells a client where to
      send everything that follows, which for a client whose Contact is unroutable - a
      browser's always is - is the only thing that makes the next request work. It is
      also half of failover: a client that re-registers on another node is told that
      node's route by that node and needs no DNS to learn it. `Path` and `Service-Route`
      are registered header types now, so both are parsed rather than echoed.
- [x] `GET /api/v1/nodes`, done on 2026-09-21. The cluster as a node knows it, with a
      URI per enabled transport. One entry today, because nothing discovers the others
      yet; the shape is what the discovery bus below fills in. A browser reads it over
      the HTTP it is already speaking and needs no SIP extension at all, which is why it
      is the first thing built rather than the last. `sip.public_address` is what a node
      advertises; a node on a wildcard bind knows every address it answers on and none a
      client should use, so it has to be told.
- [ ] Feed `GET /api/v1/nodes` from the discovery bus: the retained `nodes/<id>/status`
      messages already carry each node's SIP and inter-node TLS addresses, which is the
      same list. `GET /api/v1/client/config` is then the bootstrap blob a web client
      fetches - the node list plus what the realm expects of it - rather than a second
      inventory.
- [ ] RFC 5626 outbound, server side. This is the strongest standard answer and the one
      that removes the reconnect window entirely: a client registers two flows, to two
      nodes, and keeps both up. It needs `+sip.instance` and `reg-id` on the binding, a
      flow token in the Path this node writes, `Supported`/`Require: outbound`, and the
      keep-alives of section 4.4. It changes what identifies a binding - instance and
      reg-id rather than contact alone - so it is a Datastore contract change and an
      `API_VERSION` bump, and it wants doing in one go with the flow routing M3 needs.
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

- [ ] Public contact addresses, which are not the node's local ones. The first half of
      this landed on 2026-09-21, because the sipp harness showed it bites on one node and
      not only behind a balancer: `Core::advertised_address` returns `sip.public_address`
      when it is set and the flow's own address otherwise, and `Via`, `Record-Route` and
      `Service-Route` all go through it. `channel_register` files the advertised address
      alongside the observed one, so a Route naming it is recognised as this node - the
      second consequence below, which was the one that bit. What is left:
      - The public port, which is not always the local one. A node behind port
        forwarding writes its local port into both fields today, and the forwarded port
        is what the far end has to come back to. It has to be configured per transport.
      - A client on the same LAN reaching the public address depends on the router
        hairpinning, and plenty do not. So the address a node advertises depends on who
        is asking: a `localnet` list of private prefixes, the local address to anything
        inside them and the public address to everything else, for Via, Record-Route and
        Contact. `Util::is_ipv4_private` (`src/util.cpp`) and `Location.nat` are the
        groundwork already in the tree.
      The same split applies in the media plane and is step 6's problem, not this one:
      the builtin relay puts its local address in `c=`, which is wrong for the same
      reason and for the same audience, and the RTP port range has to be forwarded as a
      contiguous block.
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
- [ ] Redis location schema: `athena:location:<realm>:<user>` -> set of
      `{contact, node_id, flow_id, expires, path}` with TTL = registration expiry.
      `MemoryDatastore` mirrors it.
- [ ] Inter-node listener: TLS with `verify_peer | fail_if_no_peer_cert` against the
      cluster CA; requests from cluster-CA peers are trusted for Route/Path.
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
      realm and two subscribers, admin UI on 8080, WSS on 9443, TLS on 5061.
- [ ] Ten-line default config: everything else defaulted. `athenasip --config` and
      `--print-config` to show effective values. Config search path as documented.
- [ ] Admin API, part 2: `/api/v1/registrations` (with node and flow), `/api/v1/calls`
      (live and history, hangup), `/api/v1/nodes`, `/api/v1/media` (engines, health),
      `/api/v1/events` (SSE stream bridging `nodes/#`, `subscriber/#`, `calls/#`).
- [ ] athenasip-admin: replace the empty `src/lib/API.js` with a client generated from
      the OpenAPI document; pages for realms, subscribers, registrations, live calls,
      nodes, media engines, and the JsSIP test phone pointed at the server's own WSS.
- [ ] Docs rewritten for the reader without a telecoms background: install, quick start,
      configuration reference generated from the config schema, "how a call works",
      clustering guide, TLS and certificates guide, media engines guide, troubleshooting.
      The `README.md` link and the filename were fixed on 2026-09-20; the contents are
      the M2 step 10 rewrite.
- [ ] Plugins as shared libraries: `plugins.path` in config, scan for `.so`, `.dylib`
      and `.dll`, `dlopen`, call an `extern "C"` describe/create entry point, register
      through the step 2 contract, and refuse to load a plugin whose `api_version` does
      not match. The rule is documented plainly: build against the SDK headers with the
      same toolchain, because `YAML::Node` and `std::shared_ptr` cross the boundary. A
      pure C ABI is more portable and much more work; not the starting point.
      `athenasip plugins list` shows what loaded and why anything did not.
- [ ] Packaging: Docker image, Debian package, Homebrew formula; `athenasip --version`.
      In-tree plugins ship compiled in; the packages also carry the SDK headers.
- [ ] Observability: structured log option, Prometheus `/metrics` on the admin port.

---

## Milestone 6 - Conferencing, presence, web client

- [ ] RFC 4579 conference focus routing: conference URIs per realm, authorisation, route
      to a focus. FreeSWITCH `conference` as the first focus, configured by the admin API.
- [ ] RFC 4575 conference event package proxied to participants; roster in the admin
      and client APIs.
- [ ] `MediaEngine::conference` capability implemented for rtpengine publish/subscribe
      (basic SFU), then an SFU driver (Janus / mediasoup / LiveKit) behind the same URI
      scheme.
- [ ] Presence: SUBSCRIBE/NOTIFY (RFC 6665), `presence` (RFC 3856) and `dialog` packages
      for BLF, `message-summary` for MWI, backed by MQTT fan-out.
- [ ] SIP MESSAGE relay.
- [ ] Push (RFC 8599) parameters on REGISTER and a push gateway hook for AthenaPhone.
- [ ] Web client repository: video calling and conferencing, JsSIP over WSS, provisioned
      from `/api/v1/client/config`, served by AthenaSIP.

---

## Parked

Kept in the tree or history, not on any milestone:

- Lua scripting (`src/script/`): not wired, `on_message`/`send_message` are TODOs,
  `luaL_openlibs` disabled. Revisit when routing policy needs more than location lookup,
  and then as a routing-policy plugin through the step 2 contract.
- `RTPProxyClient` (`src/rtp/rtp_proxy_client.*`): rtpproxy text protocol. May become an
  `rtpproxy://` driver with `bridge` only; not a priority. Still compiled into
  `athena_core` until M2 step 10 takes it out of the build path.
- Full dialog-state replication for mid-call node failover.
- Parallel forking.
