# AthenaSIP - Active Work

Rewritten 2026-09-17 after the scope reset. Work happens on `develop`, at `af83fb5`:
the snake_case rename, the weak_ptr refactor and the outbound INVITE sketch are all
committed. Line numbers refer to that tree; update them as files move.

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
- MySQL, PostgreSQL and SQLite drivers are deleted. Built-ins are `memory` + `redis`
  for datastores, `local` + `mqtt` for events, `builtin` + `rtpengine` for media.
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
- Lua scripting stays parked and out of the build path until there is a real use.
- Core runs on a single strand. Servers post into it.

---

## Milestone 1 - Foundations

Goal: the tree builds and tests clean, shuts down without crashing, has the right
interfaces to build on, and carries no dead backends.


### New interfaces
- [ ] `Config::db_create` is read from `datastore.create` in the YAML but nothing uses
      it now the SQL drivers are gone. Remove with the rest of the Config cleanup.
- [ ] `Realm::registration_timeout` has no documented unit. `MemoryDatastore` reads it
      as seconds; nothing writes it yet, so pin the unit down when provisioning lands.
- [ ] `Datastore` gains write operations: realm create/update/delete, subscriber
      create/update/delete, location list, call list/update. `RedisDatastore` and
      `MemoryDatastore` implement all of it.

---

## Milestone 2 - A standards-compliant call on one node

Goal: INVITE / 18x / 200 / ACK / BYE / CANCEL between two subscribers on one node over
UDP, TCP, TLS, WS and WSS, with plain RTP through the builtin engine, provisioned
through the admin API, verified by sipp.

### Transaction layer (RFC 3261 section 17)
- [ ] Replace `Transaction` with `InviteClientTransaction`, `NonInviteClientTransaction`,
      `InviteServerTransaction`, `NonInviteServerTransaction`, each a state machine
      (Calling/Trying, Proceeding, Completed, Confirmed, Terminated) with timers A-K
      from `Config::sip_timer_*`. `Connection::is_reliable()` disables A/E/G and
      zeroes D/K.
- [ ] Transaction matching per 17.1.3 / 17.2.3 (branch + sent-by + method, with the
      RFC 2543 fallback deferred).
- [ ] Retransmission absorption, ACK for non-2xx handled in the transaction, 2xx ACK
      passed to the TU.
- [ ] `Channel::send` sets Via transport from `Connection::transport_name()`, generates
      `z9hG4bK` branches, sets `rport`/`received` (RFC 3581).

### Proxy core (RFC 3261 section 16)
- [ ] Request validation, Max-Forwards, loop detection (16.3).
- [ ] Route / Record-Route processing (16.4, 16.6.4), strict/loose routing.
- [ ] Target determination: location lookup in the datastore, serial forking only.
- [ ] Response processing and best-response selection (16.7), CANCEL forwarding (16.10).
- [ ] Dialog tracking for in-dialog routing (BYE, re-INVITE, hold `a=sendonly`).
- [ ] Session timers (RFC 4028): honour `Session-Expires`, refresh via re-INVITE/UPDATE.
- [ ] Responses to REGISTER: `Expires`, `Contact` with expiry, `Path` (RFC 3327).

### Media (builtin)
- [ ] On INVITE offer and 2xx answer, call `MediaEngine::offer/answer`; on BYE/CANCEL/
      timeout, `release`. SDP round-trips every attribute untouched; only `c=`, `m=`
      ports and `a=rtcp` are rewritten by the builtin driver.
- [ ] `SDP` parser preserves unknown session-level and media-level lines verbatim and
      handles multiple `m=` lines, BUNDLE (RFC 8843) groups and `rtcp-mux` without
      mangling them.

### Transports
- [ ] WSS listener: Beast websocket over `ssl_stream`, sharing the TLS context loader.
      `websocket.tls: true` config with cert/key.
- [ ] TCP fallback to fragmentation-safe behaviour for UDP requests over 1300 bytes
      (18.1.1).
- [ ] TCP/TLS connection reuse for responses and in-dialog requests (RFC 5626
      groundwork).

### Admin API, part 1 (provisioning)
- [ ] OpenAPI 3 document at `docs/api/openapi.yaml`, versioned under `/api/v1`.
- [ ] Bearer-token auth middleware with `admin` and `client` scopes; tokens in config
      for now.
- [ ] `GET/POST/PUT/DELETE /api/v1/realms`, `/api/v1/realms/{realm}/subscribers`
      (HA1 computed server-side from password), `GET /api/v1/registrations`.
- [ ] JSON body parsing and error envelope in `AdminAPI` (`src/api/admin_api.cpp`).
- [ ] `StaticMiddleware` path from `config->http_files_path`, not `"../admin"`
      (`src/main.cpp:122`).

### Test harness
- [ ] `test/e2e/` with sipp scenarios: REGISTER with Digest, INVITE/180/200/ACK/BYE,
      CANCEL before and after 180, 486, 408 on timer B, retransmission over UDP,
      RTP through the builtin relay (RTP sequence check, not silence).
- [ ] GoogleTest coverage for `SIPHeader`, `SIPMessage`, `SDP`, each transaction state
      machine (fake clock), `MemoryDatastore`, `LocalEventSystem`.
- [ ] `docker-compose.test.yml`: athenasip + sipp; runs in CI.
- [ ] GitHub Actions: build (Debug + ASan), unit tests, sipp harness.

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
- [ ] RFC 5626 outbound: registrations record the node id and flow; in-dialog requests
      to a WebSocket or NAT'd client go down the registered flow. `reg-id`/`instance`
      parameters, `Flow-Timer`.
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
- [ ] Packaging: Docker image, Debian package, Homebrew formula; `athenasip --version`.
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
  `luaL_openlibs` disabled. Revisit when routing policy needs more than location lookup.
- `RTPProxyClient` (`src/rtp/rtp_proxy_client.*`): rtpproxy text protocol. May become an
  `rtpproxy://` driver with `bridge` only; not a priority.
- Full dialog-state replication for mid-call node failover.
- Parallel forking.
