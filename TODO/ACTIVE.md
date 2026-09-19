# AthenaSIP - Active Work

Milestone 1 is complete and tagged `0.2.0`. Work happens on `develop`; `main` carries
the last release. Line numbers refer to the current tree; update them as files move.

What M2 builds on, all landed in M1: the header, URI, identity and message model follow
RFC 3261; `Core` runs on a single strand with no locks; `memory://` and `redis://` are
the datastores and both implement the full interface; `MediaEngine` and a builtin RTP
relay driver are in; `Call` is multi-party; all four RFC 3261 section 17 transaction
state machines exist and are wired in behind a matcher, with `Registrar` and `Proxy` as
the transaction users. 262 tests, clean under asan and tsan.

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
   registries keyed by URL scheme. Two implementations per interface: one built-in for
   zero-config single node, one canonical for production.
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

What the review found the tree does instead, and which step below fixes it. Steps 1 and
2 landed on 2026-09-18 and 2026-09-19 and their findings have moved to `COMPLETED.md`;
what is left:

- There is no Dialog (section 12). `Call` is the application object and is not one:
  it has tags but no route set, CSeq tracking or remote target. (Step 4.)
- `SIPUri` keeps parameters and headers as opaque strings, so the proxy cannot read
  `lr`, `transport` or `maddr`, and there is no section 19.1.4 comparison. (Step 3.)
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

- [ ] RFC 2543 fallback transaction matching, for a request whose branch carries no magic
      cookie. Deferred by the step; needed for interop with pre-3261 endpoints only.
- [ ] Stray responses are dropped. Forwarding one statelessly on its Via (18.1.2) is
      proxy work and belongs with step 5.
- [ ] CANCEL is answered locally and ends the INVITE server transaction with 487, but is
      not forwarded down the branches already tried (16.10). Also step 5.
- [ ] Serial forking tries each binding in turn but sends every attempt down the one flow
      the subscriber registered on, because that is all a single node knows. Per-binding
      flow routing is RFC 5626 in M3, and `Location.flow_id` exists for it.
- [ ] 423 Interval Too Brief with `Min-Expires` (10.3 step 7). The registrar caps the
      expiry rather than refusing a short one, which is legal but not the whole rule.

### Step 2 - Plugin contract v1

Done on 2026-09-19; see `COMPLETED.md`. One registry, one `Plugin` base, the
configuration hand-off, and `Datastore` and `MediaEngine` async in the contract.

What it deliberately left, so it is not lost:

- [ ] Pipelining in `RedisDatastore`: the listing operations walk their index one key at
      a time because each step starts the next from its own completion. Correct, and
      slower than one MGET would be. Worth doing when a node has enough bindings for it
      to show.
- [ ] `EventSystem` is on the registry and the `Plugin` base but its operations still
      take a completion callback of their own shape rather than the contract's
      `Executor` + `Handler`. It is off the call path by decision, so this is tidiness
      rather than a stall, but the contract should be one contract.

### Step 3 - SIPUri is a real URI (RFC 3261 section 19.1)

Needed before the proxy can do Route processing.

- [ ] Parameters and headers as structured values: `lr`, `transport`, `maddr`, `ttl`,
      `user`, `method`, and the rest by name. Escaping and unescaping (19.1.2, 25.1).
- [ ] URI comparison (19.1.4), for matching a REGISTER's Contact against the bindings.
- [ ] `realm` becomes `host`. It is the name of a different SIP concept.

### Step 4 - Dialogs (RFC 3261 section 12)

- [ ] A `Dialog` type owned by the TU: Call-ID, local and remote tags, route set, local
      and remote CSeq, remote target, secure flag. Created from the 2xx to an INVITE
      (12.1), matched on in-dialog requests (12.2.2), ended on BYE.
- [ ] `Call` keeps a dialog per participant leg rather than bare tags. `Call` stays the
      application object: participants, media, focus.
- [ ] In-dialog routing: BYE, re-INVITE, hold (`a=sendonly`), UPDATE.

### Step 5 - Proxy core, the rest of section 16

- [ ] Request validation, Max-Forwards, loop detection (16.3). A request that fails
      `SIPHeader::is_valid()` gets a 400 (8.2.1).
- [ ] Route / Record-Route processing (16.4, 16.6.4), strict and loose routing, on the
      structured `SIPUri` from step 3.
- [ ] Response processing and best-response selection (16.7). CANCEL forwarding (16.10).
- [ ] Session timers (RFC 4028): honour `Session-Expires`, refresh via re-INVITE or
      UPDATE, on the Dialog from step 4.

### Step 6 - Media on the signalling path

- [ ] On INVITE offer and 2xx answer, call `MediaEngine::offer/answer` for the
      participant concerned; on BYE, CANCEL or timeout, `release`. SDP round-trips
      every attribute untouched, which the parser now guarantees; the builtin driver
      rewrites only `c=`, `m=` ports and `a=rtcp`.

### Step 7 - Transports

- [ ] WSS listener: Beast websocket over `ssl_stream`, sharing the TLS context loader.
      `websocket.tls: true` config with cert and key. A prerequisite for any browser:
      they require a secure origin.
- [ ] TCP fallback for UDP requests over 1300 bytes (18.1.1).
- [ ] TCP/TLS connection reuse for responses and in-dialog requests, and a
      per-connection flow identity written to the binding. This is the groundwork RFC
      5626 flow routing in M3 stands on.

### Step 8 - Admin API, part 1 (provisioning)

- [ ] OpenAPI 3 document at `docs/api/openapi.yaml`, versioned under `/api/v1`.
- [ ] Bearer-token auth middleware with `admin` and `client` scopes; tokens in config
      for now.
- [ ] `GET/POST/PUT/DELETE /api/v1/realms`, `/api/v1/realms/{realm}/subscribers`
      (HA1 computed server-side from password), `GET /api/v1/registrations`. The
      datastore's create/update split is what lets these answer 409.
- [ ] JSON body parsing and error envelope in `AdminAPI` (`src/api/admin_api.cpp`).
- [ ] `StaticMiddleware` path from `config->http_files_path`, not `"../admin"`
      (`src/main.cpp`).
- [ ] The admin API is off the strand; it reaches Core through `call_on_strand`.

### Step 9 - Test harness

- [ ] `test/e2e/` with sipp scenarios: REGISTER with Digest, INVITE/180/200/ACK/BYE,
      CANCEL before and after 180, 486, 408 on timer B, retransmission over UDP,
      RTP through the builtin relay (RTP sequence check, not silence).
- [ ] `Registrar`, `Proxy` and `Dialog` unit tests on `MockConnection` and
      `ManualTimerSource`, in the same RFC-derived style as the transaction tests.
- [ ] `docker-compose.test.yml`: athenasip + sipp.
- [ ] GitHub Actions: build (Debug + ASan), unit tests, sipp harness. Deferred for now
      at Tom's call; listed so it is not forgotten.

### Step 10 - Smaller items surfaced by the review

- [ ] Digest with SHA-256 (RFC 8760) alongside MD5, selected by the `algorithm`
      parameter. `Util::md5` is the only hash today.
- [ ] Decide whether `Subscriber` becomes `Account`. It collides with SUBSCRIBE
      (RFC 6665, M6), and renaming costs more the later it happens.
- [ ] `RTPProxyClient` is compiled into `athena_core` and nothing constructs it. Take
      it out of the build path the way Lua was, per the Parked note.
- [ ] `docs/architecure.md` is stale and contradicts the Decisions (it lists DynamoDB,
      NATS, RabbitMQ, Kafka and SQS as planned). Rewrite it as `docs/architecture.md`
      from the Architecture section above, and fix the README link. Those backends are
      things the plugin contract makes possible, not things the core plans to build;
      the doc should say that or it reads as a roadmap the project cannot keep. `design.md`,
      `goals.md`, `scripting.md` and `modules/` predate the reset and need the same
      audit before anything is assumed from them.

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
- [ ] `docker-compose.cluster.yml` with two nodes; sipp E2E across nodes in CI.
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
      Fix `README.md` link to `docs/architecure.md` (rename the file).
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
