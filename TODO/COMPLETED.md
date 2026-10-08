# AthenaSIP - Completed Work

What has landed. The releases are indexed below; everything after them is the detail,
oldest first. Each section says what shipped and, where it matters, why it took the
shape it did. `ACTIVE.md` is the plan and this is the record.

The sections down to "Refactors landed" are the state of the tree as it was found on
2026-09-17, reconstructed from 135 commits between 2025-02-06 and 2026-07-12. They use
the names of that time: `Subscriber` became `Account` on 2026-09-21, and the MySQL,
PostgreSQL, SQLite, Lua and tinyxml2 pieces they list were deleted or parked the same
week, under Milestone 1. Everything after that is dated as it landed.

## Releases

Newest first. The detail is below, oldest first.

- **0.9.0** (2026-10-07): a second node that takes over, and phones woken for a call.
  A client whose node dies registers again through the other; an rtpengine pool keeps
  media going when an engine dies; the node ends dead calls itself with a BYE, and an
  administrator can hang one up. RFC 8599 push through `apns://`, `fcm://` and
  `webpush://`, tested against local stand-ins only. Plugins can be shared libraries;
  every setting is described once, giving `--print-schema`, a generated reference and a
  warning naming the key a misspelt one meant. The README loses its alpha banner and
  gains a quick-start guide for each way in. The sipp harnesses were not run on the tagged
  tree (Docker was paused).
- **0.8.0** (2026-10-03): a cluster that carries a call, and a console that can make one.
  A node forwards a call to the node holding the subscriber's flow, over mutual TLS from
  the cluster's own CA, and a call crossed two real nodes over UDP, TCP and WebSocket.
  Installable as a service with users, sessions, roles and rate limits on its admin API;
  HTTPS and a secure WebSocket beside the plain ones; call records; `--check`; behaviour
  profiles; RFC 5626 outbound. Subscriber and user are the words everywhere and account
  is none. A browser called AthenaPhone with video and was seen and heard both ways. The
  sipp harnesses were not run on the tagged tree (Docker was paused).
- **0.7.0** (2026-09-22): the mixed-transport call. A realm says what its calls ask of
  the media engine, the node record-routes both interfaces and carries a flow token in
  each so an in-dialog request can reach a browser, the SRTP profile arrives for a
  phone that wants encryption without DTLS, and RFC 3264's version rule and RFC 3261's
  stateless CANCEL forwarding close two recorded deviations.
- **0.6.0** (2026-09-22): RFC 5626 flow routing, the rtpengine media engine, and the
  media profile that tells it which leg is the browser. The behaviour tests written
  alongside found eleven bugs in code nothing had ever tested, among them a datagram
  that could crash the node, a queue that could segfault it, an event bus that could
  not carry two nodes, and a file server that could be walked out of.
- **0.5.0** (2026-09-21): a node that knows when a call is over, the sipp harness
  passing, the admin API, SHA-256 Digest, `Account`, and the documentation audit.
- **0.4.0** (2026-09-21): section 16 complete in the proxy, dialogs and RFC 4028, media
  on the signalling path, WSS, outbound flows, plugin contract v2.
- **0.3.0** (2026-09-18): the four section 17 state machines, the matcher, `Registrar`
  and `Proxy` as the transaction users, SDP against RFC 8866, injectable timers.
- **0.2.0** (2026-09-18): Milestone 1 complete. The strand, the sanitizers, the media
  engine interface and the datastore write operations.

Work since 0.7.0 is at the end, untagged, in eight dated sections from "The fixture a
browser can actually use" to "The quickstart, the client's own configuration, and two flows
that leaked". It is Milestone 3 and Milestone 5 interleaved: Milestone 5 came out of order
on purpose, because a node nobody can install or administer is not one anybody will adopt,
and admin authentication because the console could not go further without it. Both are
recorded that way where they appear.

## Foundations

- [x] CMake 3.23+ / C++20 build with a static `athena_core` library and `athenasip`
      executable; optional `athenasip_tests` (GoogleTest) behind `ATHENA_BUILD_TESTING`.
      Homebrew prefix discovery on macOS. Builds clean on Darwin 24.6 (`2df31e5`, `2ad0d37`).
- [x] Dependencies: Boost 1.91 (asio, beast, json, mqtt5, redis, charconv, thread),
      OpenSSL 3, MySQL Connector/C++ X DevAPI, libpqxx, SQLite3, yaml-cpp, tinyxml2, Lua 5.4.
- [x] Logging: `Logger` interface, `LoggerStdIO`, `LoggerScoped` (prefix per component).
- [x] Utilities: `Util` (trim, hex, random strings, zulu time, IPv4 checks, path expansion),
      `Version`, `URL` parser (`src/types/url.*`).
- [x] Async primitives on a single process-wide `io_context` thread
      (`src/global_io_context.h`): `DelayedTask<T>` (cancellable timer), `ExpiryMap`,
      `ExpirySet`, `AsyncQueue`.
- [x] YAML configuration (`src/config.*`): sip/node_id, SIP timer values, tls, tcp, udp,
      websocket, datastore, events, rtprelay, http sections.
- [x] GPLv3 licence, README, docs skeleton, logos.

## Transport layer (docs/design.md model: Server -> Connection -> Channel)

- [x] `Server` / `Connection` abstractions (`src/servers/server.h`, `connection.h`).
- [x] TCP server + connection (`tcp_server.cpp`, `tcp_connection.h`).
- [x] TLS server + connection with PEM cert/key loading (`tls_server.cpp`,
      `tls_connection.h`); snakeoil CA and cert in `tls/`.
- [x] UDP server with per-remote-endpoint virtual connections and an `AsyncQueue`
      delivering datagrams into the same `Channel` read path (`udp_server.cpp`,
      `udp_connection.cpp`).
- [x] WebSocket (RFC 7118 style `ws://`) server: HTTP upgrade session then a Beast
      websocket `Connection` (`websocket_server.cpp`, `websocket_httpsession.cpp`,
      `websocket_connection.cpp`) (`e821224`, `9460cda`, `7c6eb1f`).
- [x] `Channel`: byte-stream to SIP message framing (header/body split on CRLFCRLF,
      Content-Length body accumulation), outbound serialisation with Via/Record-Route/
      Content-Length fix-up, registration with `Core` (`4339c7d`).
- [x] `is_reliable()` on all connections (`4590e22`).

## SIP parsing and message model

- [x] `SIPHeader`: request/response first line, ordered header list + lookup map,
      folded-line continuation, add/add_start/remove_value/clear.
- [x] Typed header registry (`Header::create` factory, `register_factory`): `ViaHeader`,
      `CSeqHeader`, `AuthorizationHeader`, `SIPIdentityHeader` (From/To/Contact with tags),
      `SupportedHeader`, `UIntHeader`, `StringHeader` fallback (`27799f0`, `0dc5af8`).
- [x] `SIPUri`, `SIPIdentity`, `Realm`, `Subscriber`, `Authorization` (Digest parsing)
      types under `src/types/`.
- [x] `SIPMessage::generate_response` (copies From/To/Call-ID/Via/CSeq, adds To tag,
      `Allow`), `get_transaction_id` (top Via branch + CSeq method).
- [x] `SDP` parser/serialiser with session and media sections, `Media::get_unique_id`
      (mid + media + codec hash) (`295cf32`, `3ca82bb`).

## Core signalling

- [x] `Core` (formerly `Registrar` + `SIPCore`, merged in `213cf80`): owns config,
      datastore, event system, servers, channels, transactions, calls, RTP relay, admin API.
- [x] Transaction layer: transaction lookup/creation per message, INVITE vs non-INVITE
      typing, single B/F timeout timer, `transaction_end_all` on shutdown
      (`c2b14f8`, `a8387c1`, `056e29c`, `fe7f73b`).
- [x] 400 Bad Request for messages missing Via/CSeq.
- [x] REGISTER with Digest authentication: 401 challenge with HMAC-SHA256 signed nonce
      (realm id + random + timestamp, secret from `realm.nonce_secret`), nonce stored in
      datastore and in an `ExpirySet` cache, subscriber lookup by AoR, HA1 verification,
      200 OK with Contact echo (`782c87f`, `7e19b6f`, `7c9e470`, `160ac51`, `b2f61bb`).
- [x] Per-subscriber event subscription on successful REGISTER; `subscriber/<uri>/status`
      event published with contact, node id, timestamp (`2f3544a`).
- [x] INVITE receive path: 100 Trying, `Call` registered in memory and datastore,
      `subscriber/<uri>/invite` event with JSON `{call_id, from, to, sdp}` (working tree).
- [x] 501 Not Implemented for unknown methods; ACK accepted silently (`3964d73`).
- [x] Node lifecycle events: `nodes/<id>/status` started/stopped,
      `nodes/<id>/channels/<endpoint>` registered/closed, transaction registered/
      unregistered (`72672a2`).
- [x] Signal handling: SIGINT orderly shutdown (transactions, channels, servers, relay,
      admin, events, datastore); SIGHUP logged.

## Datastores (`src/datastores/`, URL-selected driver registry)

- [x] `Datastore` interface + scheme registry (`register_driver` / `create_driver`)
      (`cf361fc`, `1a044c8`).
- [x] MySQL / MariaDB via X DevAPI (`mysql`, `mysqlx`): realm, subscriber, location
      register/unregister, nonce create/check, call create.
- [x] PostgreSQL via libpqxx (`postgres`, `postgresql`): realm, subscriber, location,
      nonce.
- [x] SQLite (`sqlite`, `sqlite3`): realm, subscriber, location, nonce; creates the DB
      file if missing (`baf73f2`).
- [x] Redis via boost.redis (`redis`, `rediss`, `redis+ssl`): JSON-encoded realm/
      subscriber, location, nonce with TTL; own io thread, sync-over-async helpers with
      timeouts (`a4b2f84`, `8a5c6bf`).
- [x] MySQL schema dump in `sql/create.sql` (`call`, `location`, `nonce`, `participant`,
      `realm`, `subscriber`, `new_id()` routine) and `bin/save_db_create.sh`.

## Event systems (`src/events/`, URL-selected driver registry)

- [x] `EventSystem` interface with sync and callback overloads, `Subscription`,
      MQTT-style `TopicFilter` (`+`, `#`, `$` rules) (`cf361fc`).
- [x] `LocalEventSystem` (`athena`, `local`, `memory`): in-memory pub/sub dispatched on
      the global io_context, exception-safe callbacks; unit tested.
- [x] `MQTTEventSystem` (`mqtt`, `mqtt5`): boost.mqtt5 client on its own thread/strand,
      URL parsing (host, port, client_id, keep_alive, username, password, prefix),
      QoS 0 publish, broker-side subscription dedupe, SUBACK reason handling,
      receive loop with session-expiry resubscribe, prefix-stripped local dispatch
      (`156bb1e`, `72f48d5`).

## Media

- [x] `RTPRelay`: port pool allocator over a bind address and range.
- [x] `RTPRelaySet`: single UDP socket per stream that learns remote endpoints from the
      first packet and relays between all others (`287cbaa`, `f866152`).
- [x] `RTPProxyClient`: rtpproxy text protocol client (V, U, L, R, D commands) on a
      strand, async send/receive with response parsing (`src/rtp/rtp_proxy_client.*`).
- [x] `MediaStream` / `Call::streams` model. (Wiring into INVITE was removed in the
      transactions refactor and came back as Milestone 2 step 6, below.)

## Admin / HTTP

- [x] Beast HTTP server with a middleware chain (`AdminAPI`, `HttpSession`),
      status helpers (200/400/404/500 JSON), `StaticMiddleware` serving the built
      athenasip-admin React bundle from `admin/` (`8a2fd5e`, `fbf8242`, `67d6fe9`).

## Scripting

- [x] `ScriptEngine` interface and `LuaScriptEngine`: Lua state lifecycle, `print`,
      `include()` with allowed-path check, `log.{debug,info,warn,error}` table,
      `execute_lua_fn`. Example scripts in `config/`. (Not started from `main`.)

## Tests (`tests/`, GoogleTest)

- [x] DelayedTask, ExpiryMap, ExpirySet, Util, Version, URL, header registry,
      LocalEventSystem. Helpers for pumping the global io_context. (Broken by the
      snake_case rename at the time, and fixed under Milestone 1 below.)

## Refactors landed

- [x] FieldValue -> Header, siptypes -> types (`27799f0`).
- [x] Session -> Channel; TLSSession folded into Session (`4339c7d`, `458e10e`).
- [x] Databases -> Datastores with explicit driver registration (`1a044c8`, `2ad0d37`).
- [x] Registrar + SIPCore -> Core (`213cf80`).
- [x] camelCase -> snake_case across `src/` (`8651ced` + working tree).

## Milestone 1 - Foundations

### Working tree and hygiene (2026-09-17)

- [x] `tests/` follow the snake_case rename (`get_registry`, `get_global_io_context`),
      and the `EventSystemTest` cases connect the event system before publishing, which
      `LocalEventSystem` now requires. ctest is 44/44 green (`ed061b6`).
- [x] Working tree split into three commits: the snake_case rename (`ed061b6`), the
      weak_ptr back-reference refactor and outbound INVITE sketch (`df4fcfb`), and call
      record persistence (`a45e590`).
- [x] `.gitignore` covers `.DS_Store` and `build-*`. `config/config.yaml` is now
      `config/config.example.yaml` with `USERNAME:PASSWORD` placeholders in the
      datastore URL.
- [x] `CMAKE_BUILD_TYPE` defaults to Debug on single-config generators; the
      `Boost 1.91.0 EXACT` pin is now a `1.87.0` minimum.
- [x] `CMakePresets.json` with `debug`, `release`, `tests`, `asan` (ASan + UBSan) and
      `tsan` presets, driven by an `ATHENA_SANITIZE` cache variable.
- [x] Version comes from CMake: `src/build_version.h.in` is configured into the build
      tree and `main.cpp` builds `Version` from `ATHENA_VERSION_*`. The splash reads
      v0.1.0.

### Crash and shutdown bugs (2026-09-17)

- [x] `Core::transaction_end_all` and `Core::channel_close_all` erased from the map they
      were iterating, because `end()` and `close()` unregister. Both copy out under the
      lock, clear, then act. This was the segfault on close from `737ef1a` (`d58b096`).
- [x] `Channel::send` and `_schedule_async_write` dereferenced `_connection` after
      `close()` reset it, and the async write buffer was a local that died before the
      write completed. Guarded, and the buffer is owned by the completion handler
      (`d58b096`).
- [x] The subscriber event subscription used a `subscriber/<uri>/#` filter, which also
      matched the node's own status publish and then threw on `at("call_id")`. It now
      takes the invite topic alone, captures `weak_ptr` rather than a `shared_ptr` to
      the channel and a raw `this`, and validates every field (`d58b096`).

### SIP correctness, header model (2026-09-17)

- [x] `SIPHeader::add_start` updated a copy of the map vector, so `headers_map[f][0]`
      was the oldest value and topmost-Via lookups read the wrong hop (`7b9b4f9`).
- [x] `first_line()` dereferenced a null `request_uri`; `type` defaults to `Request`, so
      any hand-built header without a URI segfaulted on serialisation. The outbound
      INVITE built from an event did exactly that (`7b9b4f9`).
- [x] Field names are matched case-insensitively (RFC 3261 7.3.1). The triple-cased
      `Record-Route` removal in `Channel::send` is gone.
- [x] Compact forms resolve to their long names (RFC 3261 7.3.3, 20).
- [x] Comma-separated values split into separate rows for list-valued fields only,
      respecting quoted strings and angle brackets (RFC 3261 7.3.1). Credential fields
      are excluded, since their commas separate parameters.
- [x] `parse()` strips the CR of each CRLF, so the blank line actually ends the headers,
      and reports malformed start lines and header lines through `is_valid()`.
- [x] Tests: `TopicFilter` (13 cases, previously none), `SIPHeader` behaviour, and
      `SIPHeaderRFCTest` deriving its expectations from RFC 3261 rather than from the
      implementation. 76 tests green.

### SIP correctness, URI, identity and message (2026-09-17)

- [x] `SIPUri` parses IPv6 host references (RFC 3261 19.1.1, RFC 5118 4). The colons
      inside `[2001:db8::1]` were being read as a port separator, so no IPv6 SIP URI
      parsed at all.
- [x] `SIPUri` port parsing is range checked. It used `std::stoi`, which wrapped a
      port above 65535 into a uint16_t and threw `std::out_of_range` on a long run of
      digits: a malformed URI off the wire terminated the process.
- [x] `SIPIdentity` in addr-spec form no longer puts header parameters into the URI.
      `sip:carol@example.com;tag=qwerty` was parsed with `tag=qwerty` as a URI
      parameter (RFC 3261 20).
- [x] `SIPMessage::generate_response` copies every Via, in order (RFC 3261 8.2.6.2).
      It copied only the topmost, so a response could not retrace a proxy chain.
- [x] `generate_response` copies the To identity before adding a tag. It mutated the
      request's own To in place, changing the request that transaction matching still
      needs, and dereferenced the result of `as<SIPIdentityHeader>()` without checking
      it for null.
- [x] `get_transaction_id` includes the topmost Via sent-by as well as the branch and
      method (RFC 3261 17.1.3, 17.2.3), with separators so the parts cannot run
      together, and guards for a missing or untyped Via or CSeq.
- [x] Tests: `SIPUriTest` (9), `SIPIdentityTest` (5), `SIPMessageTest` (7), all derived
      from RFC 3261 rather than from the implementation. 97 tests green.

### Delete (2026-09-17)

- [x] `src/datastores/{mysql,postgres,sqlite}_datastore.*` removed, along with their
      registrations in `datastore_drivers.h`, `sql/`, `bin/save_db_create.sh` and the
      CMake `find_package` blocks for mysql-concpp, PostgreSQL, libpqxx and SQLite3.
      `redis` is the only registered datastore scheme.
- [x] `tinyxml2` dropped: nothing in the tree included it. If presence PIDF needs XML
      in M6 it comes back then.
- [x] Lua taken out of the build path per the parking decision. `src/script/` stays in
      the tree but is filtered out of the source glob and the runtime links no Lua, so
      the mismatched-upvalue bug in `lua_logger.cpp` is moot. The `scripting:` section
      is gone from the example config.
- [x] Dependencies are now Boost, OpenSSL and yaml-cpp, plus GoogleTest for the test
      build. `otool -L` on the binary shows no SQL, Lua or XML libraries.
- [x] Docs updated: `architecture.md` driver table, `configuration.md` datastore table
      and example, `compiling.md` brew line and build commands, `quick_start.md` and
      the README. `docs/versions/v0_0_1_overview.md` is left alone as a record of what
      0.0.1 actually shipped.

### MemoryDatastore (2026-09-17)

- [x] `MemoryDatastore` (`memory://`): realms, subscribers, locations with TTL, nonces
      with TTL and calls, all in process. Registered in `datastore_drivers.h`, and now
      the default in `config.example.yaml`, so a single node runs with no external
      service at all.
- [x] Deviation from the plan, on purpose: locations and nonces carry explicit expiry
      timestamps and are pruned on read, rather than being backed by `ExpiryMap`.
      `ExpiryMap` cannot be enumerated, and target determination needs every contact
      for a subscriber (RFC 3261 16.5). It also drives expiry from the global
      io_context, which would make a datastore's correctness depend on that context
      being polled. Lazy pruning is deterministic in tests and has no such dependency.
- [x] `types::URL` accepts a hostless URL, so `memory://` parses. The regex required a
      non-empty host, which also rejected `file:///path`.
- [x] `Config::sqlite_create_database` removed: dead with the SQLite driver.
- [x] Tests: 14 for `MemoryDatastore` covering realms, subscriber lookup, multiple
      contacts, re-registration, selective unregister, nonce expiry, calls and
      resolution through the driver registry. 111 tests green.

### Core registration and regression coverage (2026-09-17)

- [x] `Core::subscriber_register` tested the subscriber row rather than the binding, so
      `datastore->subscriber_register` was skipped whenever the subscriber existed,
      which is always. No contact was ever stored and the registrar had nothing to
      route to (RFC 3261 10.3 step 7).
- [x] `Core::subscriber_unregister` dropped the per-channel event subscription only on
      the datastore failure path, so every successful unregister leaked it. It now goes
      first, and the channel index is cleared with it.
- [x] `MockConnection` test double for the `Connection` interface: captures what was
      written, counts closes and shutdowns, and parks reads. The M2 transaction tests
      will want it too.
- [x] Regression tests for the shutdown crashes fixed in `d58b096`: 16 transactions
      through `transaction_end_all` and 16 channels through `channel_close_all`, both
      of which previously erased from the map they were iterating. Also covers the
      empty-registry case and that `Channel::close` is idempotent.
- [x] 120 tests green.

### Logic bugs (2026-09-17)

- [x] `Channel::send` hardcoded `SIP/2.0/TCP` on every Via whatever the channel was.
      The transport now comes from `Connection::transport_name()` (RFC 3261 18.1.1),
      and a missing branch is generated with the `z9hG4bK` magic cookie
      (8.1.1.7). The M2 transaction layer takes the branch over.
- [x] `Config` read `rtprelay.min_port`/`max_port` while the YAML and docs said
      `port_min`/`port_max`, so every configured RTP port range was silently ignored
      and the defaults used.
- [x] `main.cpp` ignored `load_from_yaml`'s return and carried on with a
      half-initialised Config. It now reports the path and exits. The `bool` and port
      fields in `Config` have defaults, so a partial load cannot leave them
      indeterminate.
- [x] Event topic drift fixed and the scheme defined once in `src/events/topics.h`,
      documented in `docs/events.md`. The transaction topics had a leading `/`, which
      is an empty first level in MQTT and is not matched by a `nodes/#` filter, and
      call unregister published to `call.unregister` with no call id. Channel topics
      became `nodes/<id>/channels/<transport>/<endpoint>` rather than carrying the
      transport as a `;transport=` parameter.
- [x] `LoggerScoped::raw` called `error` on the underlying logger, so the startup
      splash was logged at error level.
- [x] Tests: `TopicsTest` (6) over the scheme, and Via transport, branch generation and
      branch preservation through `Channel::send`. 129 tests green.

### Core data races and the sanitizers (2026-09-17)

- [x] `_calls` had no mutex at all. `call_register`, `call_unregister` and `call_get`
      mutated and read it from whichever server thread the INVITE arrived on, so two
      concurrent calls on different transports were concurrent `unordered_map`
      mutation.
- [x] `_channels_by_subscriber` was declared in `core.h` as guarded by
      `_channels_mutex`, but nothing ever took it: `subscriber_register`,
      `subscriber_unregister` and `subscriber_get_channel` all reached the map
      unguarded.
- [x] `subscriber_get_channel` used `operator[]`, which inserts an empty entry for an
      unknown subscriber: a mutation inside a getter. It uses `find` now.
- [x] The datastore and event system calls stay outside the locks: both can block and
      neither needs the map held.
- [x] `tests/core_concurrency_test.cpp`: eight threads through call register, get and
      unregister; a reader running alongside registration; concurrent channel creation;
      and concurrent subscriber channel lookup. Verified to catch the bug by reverting
      the locks, where the first test hangs with eight threads spinning at 688% CPU in
      a cycled `unordered_map` bucket chain.
- [x] Whole suite run under both sanitizers: `asan` (ASan + UBSan) and `tsan`, 133
      tests, clean under both. This was the "run both once before M2" item.

### Concurrency model: the Core strand (2026-09-17)

- [x] `Core` runs on a `boost::asio::strand` over the global io_context. `_channels`,
      `_channels_by_subscriber`, `_transactions` and `_calls` are strand-confined and
      every lock is gone: `core.h` and `core.cpp` contain no mutex at all.
- [x] `Core::post()` queues work on the strand; `Core::call_on_strand()` runs work and
      waits, for callers that are not on the strand and need an answer (shutdown, the
      admin API, tests). It calls straight through when already on the strand, so it
      cannot deadlock on itself.
- [x] `Channel` is strand-confined too, and hands over with `dispatch` rather than
      `post`, so a caller already on the strand runs inline and `channel_close_all`
      stays synchronous within strand work. Read and write completions arrive on the
      connection's own io_context thread and hop back to the strand before touching
      anything.
- [x] `UDPServer` has its own strand. Its socket is shared by every UDP channel, unlike
      TCP and TLS where each channel owns one, so sends now arriving from the Core
      strand would otherwise run concurrently with the server thread's receive. The
      socket and the connection map are both strand-confined now.
- [x] Shutdown in `main.cpp` goes through `call_on_strand` rather than reaching into
      Core from the signal handler's thread.
- [x] 133 tests green, and clean under both the `asan` and `tsan` presets after the
      conversion.

### Media engine and multi-party Call (2026-09-17)

- [x] `Call` is multi-party. `from`/`to` are gone, replaced by `participants`, each with
      an identity, a weakly-held channel, an owning node id, dialog tags and its own
      media streams, plus an optional `focus` URI for RFC 4579 conferencing, and
      created/answered/ended timestamps. A two-party call is just the case of two
      participants and no focus; nothing assumes there are only two.
- [x] `MediaEngine` in `src/media/media_engine.h`, registered by URL scheme the same way
      `Datastore` and `EventSystem` are. `capabilities()` returns
      `{bridge, conference, record, transcode}` so a caller can ask rather than assume.
      `offer`, `answer`, `release` and `query` name the participant they act for, and
      `join`, `leave` and `roster` default to declining so a bridge-only driver does not
      have to implement them.
- [x] `Flags` are derived from the SDP, not the transport: `ice`, `dtls`, `srtp`,
      `rtcp_mux`, and the participant index.
- [x] `BuiltinMediaEngine` (`builtin://`) wraps `RTPRelay`/`RTPRelaySet`, claims `bridge`
      only, and declines anything needing ICE, DTLS or SRTP rather than producing a
      broken answer. `_map_media` is ported from `c2b14f8^:src/sip_core.cpp`, now
      keeping relay sets per call so `release()` gives the ports back.
- [x] `Config` reads a `media:` section, and `sip.allow_unencrypted` and
      `sip.event_prefix`, which were in the YAML and the docs but never read, so
      setting them did nothing. The `scripting:` section is gone from the example.
- [x] `main.cpp` creates the engine from `config->media_url`, registers it with `Core`,
      and closes it on shutdown.
- [x] 13 tests for the engine: capabilities, SDP rewriting to this node, the RTCP
      attribute, per-participant streams, relay reuse on a repeated offer, declining
      WebRTC flags, release, query and registry resolution. 146 tests green, clean
      under asan.

### Datastore write operations (2026-09-18)

- [x] `Datastore` gains realm create/update/delete/list, subscriber
      create/update/delete/list, `location_list`, and call update/list. create and
      update are separate throughout, so provisioning can tell "already exists" from
      "changed" rather than silently overwriting, which is what the admin API needs to
      answer 409.
- [x] `types::Location` carries the whole binding, not just a contact URI:
      subscriber id, node id, flow id and Path for the cluster, registered and expiry
      times, and the NAT flag. `MemoryDatastore`'s informal `realm_add`/`subscriber_add`
      seeding helpers are gone, replaced by the real interface.
- [x] `RedisDatastore` implements all of it, with set-backed indexes
      (`athena:index:realms`, `:subscribers:<realm>`, `:locations:<id>`, `:calls`) so
      listing never needs `KEYS` or `SCAN`. `KEYS` blocks the server and `location_list`
      is on the call path for target determination. It also implements calls, which
      were stubs returning false and nullptr, serialising participants.
- [x] Fixed while testing it: Redis wrote no `contact` field but `location_list` read
      one, so every registration lookup threw and returned an empty list. The parse is
      also per record now, so one unreadable binding cannot hide the rest.
- [x] Fixed while testing it: `boost::redis::connection` is not thread safe, its
      channels use a null mutex, but every `async_exec` was being initiated from
      whichever thread called in while the datastore's own io thread ran the
      connection. All four call sites now post onto the connection's executor.
      ThreadSanitizer reported the race against a live server; it is clean now.
- [x] `Realm::registration_timeout` is documented as seconds, matching the Expires
      header it answers (RFC 3261 10.2.1). `Config::db_create` removed: dead with the
      SQL drivers.
- [x] `tests/datastores/redis_datastore_test.cpp` runs against a real Redis when
      `ATHENA_TEST_REDIS_URL` is set and skips otherwise, so a machine without Redis
      still runs the suite green. 163 tests, clean under asan and tsan with Redis.

## Milestone 1 - Foundations: complete (2026-09-18)

Every item is done. The tree builds and tests clean, shuts down without crashing, has
the interfaces M2 builds on, and carries no dead backends.

## Milestone 2 - groundwork

### SDP rewritten against RFC 8866 (2026-09-18)

The old parser decomposed SDP into typed structs and rebuilt it from them, so anything
it did not model was dropped on the way through. For a browser offer that is the ICE
candidates, the DTLS fingerprint and the BUNDLE group. It is now a list of lines kept in
arrival order, with only the fields a proxy rewrites parsed out, which makes
"round-trips every attribute untouched" true by construction rather than by effort.

- [x] `parse()` returned `true` unconditionally: rubbish parsed "successfully" and the
      caller had no way to tell. It now validates the mandatory v=, o=, s= and t=
      (RFC 8866 section 5) and reports through `is_valid()`.
- [x] Unknown line types were silently dropped. RFC 8866 section 5.13 says a parser
      ignores what it does not understand, and for a proxy that means passing it on.
- [x] A second `t=` overwrote the first, losing a time description (section 5.9).
- [x] `m=audio 49170/2 RTP/AVP 0` was mangled: reading the port as an integer stops at
      the slash, so the proto became "/2" and the real proto was read as a format
      (section 5.14). The port count is parsed and re-emitted.
- [x] `to_string()` re-emits every line in arrival order, and never emits a bare `s=`,
      which is invalid (section 5.3).
- [x] `MediaSection` replaces the old flat media struct: `mid()` for BUNDLE (RFC 8843),
      `codec()`, `unique_id()` for matching a stream across re-offers, and
      `set_connection`/`set_attribute` that edit in place rather than rebuilding.
- [x] `BuiltinMediaEngine` follows the new API and no longer adds a media-level `c=`
      where the far end relied on the session-level one.
- [x] 11 tests from RFC 8866, including a byte-identical round trip of a real WebRTC
      offer with BUNDLE, ICE, DTLS and rtcp-mux.

### Injectable timers (2026-09-18)

- [x] `TimerSource` in `src/timer_source.h`: `AsioTimerSource` for production,
      `ManualTimerSource` for tests. `DelayedTask` takes one, defaulting to the real
      clock, so existing callers are unchanged.
- [x] `ManualTimerSource::advance` steps time to each timer as it falls due rather than
      jumping to the target, so a callback sees the time its own timer fired at. Timer A
      doubles by re-arming from inside its callback, and jumping would make every
      interval after the first wrong. The test that caught this is kept.
- [x] This is what makes the section 17 state machines testable: timer B is 64*T1, 32
      seconds at the default T1, and there is a test proving that costs nothing now.
- [x] 182 tests, clean under asan and tsan.

### Server transactions, RFC 3261 section 17.2 (2026-09-18)

- [x] `TransactionBase` in `src/transactions/`: the section 17 states, the timer values
      derived from `Config` as multiples of T1 and T4, and the transport/TU boundary.
      `is_reliable()` disables the retransmission timers and zeroes the waiting ones,
      because a stream transport neither loses nor duplicates.
- [x] `InviteServerTransaction` (17.2.1): Proceeding, Completed, Confirmed, Terminated
      with timers G, H and I. Sends 100 Trying itself when the TU has not answered in
      200ms. A 2xx terminates the transaction immediately rather than going to
      Completed, because a 2xx and its ACK are end to end and belong to the TU. The ACK
      for a non-2xx is absorbed and never reaches the TU, which is the opposite case.
      G doubles to T2.
- [x] `NonInviteServerTransaction` (17.2.2): Trying, Proceeding, Completed, Terminated
      with timer J. A retransmission in Trying is dropped rather than passed up, so the
      TU sees each request once.
- [x] `Config` gains timers J and K, which were missing: J is the non-INVITE server
      wait and K the non-INVITE client wait.
- [x] 19 tests on `ManualTimerSource`, covering retransmission absorption, the 100
      Trying timer, G's backoff, H giving up with no ACK, I and J, and the reliable
      transport cases. Each runs in about 0.16s despite exercising 32 second timers.
- [x] 201 tests, clean under asan and tsan.

### Client transactions, RFC 3261 section 17.1 (2026-09-18)

- [x] `InviteClientTransaction` (17.1.1): Calling, Proceeding, Completed, Terminated
      with timers A, B and D. It builds and sends the ACK for a non-2xx itself
      (17.1.1.3), reusing the request's Call-ID, From, Request-URI and top Via, taking
      the To from the response because that carries the tag the far end chose, and
      keeping the request's CSeq number with the method changed to ACK. A 2xx is not
      acknowledged here: that ACK is the TU's, as a separate transaction that may take
      a different route.
- [x] `NonInviteClientTransaction` (17.1.2): Trying, Proceeding, Completed, Terminated
      with timers E, F and K. Timer E differs from timer A in two ways that are easy to
      miss: it is capped at T2 rather than doubling without limit, and once in
      Proceeding it fires at T2 flat.
- [x] `TransactionBase` gains a timeout callback, so a TU is told when B, F or H expires
      rather than waiting forever. The INVITE server transaction reports timer H
      through it too.
- [x] 18 more tests, including the full header rules for the generated ACK and the
      difference in backoff between timers A and E.
- [x] 219 tests, clean under asan and tsan.


### Transaction users and the wiring, RFC 3261 sections 10, 16, 17 (2026-09-18)

M2 step 1. The four section 17 state machines were finished and unwired; `src/transaction.cpp`
was not a transaction at all but the transaction user, doing Digest auth, the binding write
and the 200 OK for REGISTER, and creating the `Call` for INVITE. This wires the machines in
and gives the TU logic somewhere to live.

- [x] `TransactionMatcher` (`src/transactions/transaction_matcher.*`): the 17.1.3 and 17.2.3
      identity, branch plus sent-by plus method, with the two cases that are not the obvious
      one. An ACK for a non-2xx carries CSeq method ACK but belongs to the INVITE server
      transaction, so its key is computed with the method forced to INVITE. A CANCEL gets its
      own non-INVITE server transaction and separately names the INVITE it cancels, which is
      the same substitution again. RFC 2543 fallback matching is deferred.
- [x] `Core::process_message` is the router and nothing else: response to its client
      transaction, request to its server transaction, retransmissions absorbed before the TU,
      ACK for a 2xx straight through as it travels outside any transaction. `Core` creates,
      files and starts both kinds of transaction and is the composition root for the TUs.
      `src/transaction.{h,cpp}` is deleted.
- [x] `Registrar` (`src/registrar.*`), the TU for REGISTER. Digest against the stored HA1,
      the binding write, and a 200 OK listing every current binding with the time it has left
      plus an `Expires` (10.3 step 8). Expiry is negotiated from the Contact parameter, then
      the `Expires` header, then the realm, and capped by the realm. Zero removes one binding
      and a lone `*` with Expires 0 removes them all (10.2.2). An unserved domain is 404; an
      unknown subscriber inside a served realm is challenged, so a REGISTER sweep cannot
      enumerate accounts. RFC 3327 `Path` is recorded on the binding.
- [x] `Proxy` (`src/proxy.*`), the TU for everything else. Target determination from
      `location_list` (16.5), forwarding with the Request-URI set to the binding, Max-Forwards
      decremented and this node's Via added (16.6 steps 2, 3 and 8), serial forking across the
      bindings, and responses with the node's own Via stripped on the way back (16.7 step 3).
      CANCEL is answered 200 on its own transaction and ends the INVITE with 487 (9.2).
- [x] The event-bus INVITE path is gone: `Core::_invite_from_event`, the per-channel
      `_event_subscription` and the `subscriber/<uri>/invite` topic. With `mqtt://` an INVITE
      used to travel through the broker, which the Decisions forbid. `docs/events.md` now says
      plainly that the bus is observability and never signalling.
- [x] Via moved out of `Channel`. The transport keeps framing, fills in a `z9hG4bK` branch only
      when the layer above left the top Via without one, and adds `received` and `rport`
      (18.2.1, RFC 3581 4), which are transport facts. It no longer strips every Record-Route
      on the way out, which would have broken the dialog it was meant to keep the node in.
- [x] `Datastore::subscriber_register` takes the negotiated expiry and the Path, so a binding
      expires when the registrar told the client it would rather than on a per-realm constant.
- [x] Fixed a shutdown hang the step exposed: releasing the io_context work guard is not enough
      when a timer is pending, so a registration expiry an hour out held `run()` open and the
      process never exited. The global context now stops rather than waits.
- [x] 43 new tests: the matcher against the section 17 identity rules, the registrar against
      section 10 and RFC 3327, the proxy against section 16, and the transport against RFC 3581.
      262 tests, clean under asan and tsan.

## Milestone 2 step 2 - Plugin contract v1 (2026-09-19)

- [x] One `PluginRegistry` keyed by (kind, scheme) (`src/plugins/plugin_registry.*`), replacing
      the three copy-pasted template registries in `datastore.h`, `event_system.h` and
      `media_engine.h`. The pair is the key because `memory://` is a datastore and, separately,
      an event system. `Datastore`, `EventSystem` and `MediaEngine` keep typed
      `register_driver`/`create_driver` helpers over it, so no call site changed shape.
- [x] `plugins::Plugin` (`src/plugins/plugin.h`): `kind()`, `name()`, `version()`,
      `api_version()`, `configure()` and `health()`. `get_driver_name()` is gone - it returned
      prose ("AthenaSIP Redis Driver v0.0.1"), which nothing can key on. The versioning rule is
      enforced rather than documented: the registry refuses a plugin whose `api_version()` is
      not this server's, which is unreachable for a compiled-in driver and the whole point once
      plugins are shared libraries.
- [x] Configuration hand-off: `configure(own_root, system)` with `Config::plugin_root(kind,
      name)`, keyed on the driver's `name()` rather than its scheme so the three Redis schemes
      share one section. `BuiltinMediaEngine` and `MQTTEventSystem` read theirs; the URL stays
      the selector and a one-line `datastore: { url: memory:// }` still works.
- [x] `Datastore` is async in the contract: 21 operations, each taking the caller's executor and
      a handler that is posted rather than called inline. `RedisDatastore` loses its sync
      primitive layer and the `_wait_*` future bridge, and with them the five second timeout
      that ran on the Core strand - a slow Redis used to stall every call on the node.
      `plugins::Status` and `plugins::Result<T>` separate "found nothing" from "could not ask",
      so the registrar and proxy answer 500 to an unreachable datastore where both used to
      answer 404.
- [x] `Core`, `Registrar` and `Proxy` become continuation chains; the registrar reads as the
      numbered stages of 10.3 because each step that needs the datastore returns before the
      next one runs. `TransactionUser` is `enable_shared_from_this` and `Core` holds a
      reference to itself across its own calls: a handler running after its owner was destroyed
      was a use-after-free the suite found once the work outlived the fixture.
- [x] `MediaEngine` follows: `offer`, `answer`, `release`, `query` and the conference operations
      all take an executor and a handler.
- [x] `docs/plugins.md`: the contract, the lifecycle, the versioning rule, the two async rules a
      driver must not break, and how to write one.
- [x] 26 new tests. The registry ones cover what the shape has to guarantee: the same scheme
      under two kinds stays two drivers, an unknown scheme and a known scheme under the wrong
      kind both refuse, and a plugin built against another contract version is not constructed.
      `tests/helpers/sync_datastore_helper.h` and `sync_media_engine_helper.h` are blocking
      views for tests only, with the reason written where they are defined. 273 tests, clean
      under asan and tsan, and the Redis suite verified against a real server.

## Milestone 2 step 3 - SIPUri is a real URI (2026-09-20)

- [x] Parameters and headers are structured (`src/types/sip_uri.*`): `parameter(name)`,
      `has_parameter`, `header(name)`, in the order they arrived so a URI we did not write
      round-trips as it came. Names match case-insensitively (19.1.1) and are kept as written.
      `lr` is present-with-empty-value, distinguishable from absent, which is what 16.12 needs.
- [x] Escaping per 19.1.2 and 25.1, with the per-component allowed sets (user, password,
      param, header) written out from the ABNF. Values are held unescaped; `to_string()` puts
      it back. A '%' not followed by two hex digits is left alone rather than swallowed.
- [x] `equivalent_to()` implements 19.1.4: sip and sips never match, user is case-sensitive,
      host is not, an absent port is not the default port, a parameter in both must match,
      `user`/`ttl`/`method`/`maddr` in only one never match, and headers must be present in
      both and equal. Not `operator==`, because it is not string equality and a reader should
      notice.
- [x] `realm` is `host`. A realm is the Digest protection domain (22.1); the field was the
      host of the URI and had been named for a different concept.
- [x] The regex is gone. The parser is a sequence of `find()`s on characters that cannot
      appear unescaped in the parts they delimit, over attacker-supplied text, with no
      recursion to bound. 16 tests from the RFC, written first and watched fail. 289 tests.

## Milestone 2 - SIPIdentity is hand-written (2026-09-20)

- [x] `SIPIdentity::parse` (`src/types/sip_identity.cpp`) is a hand-written parser. The
      backtracking `std::regex` it replaces ran over every To, From and Contact arriving from
      the network, with a recursion depth bounded by the length of the header and nothing
      else. The message path now has no regex on it at all.
- [x] It parses what the grammar actually allows and the regex could not: a quoted display
      name containing `<`, `>`, `;` or `,` (20.10 allows it, which is the whole point of
      quoting), and `quoted-pair` escapes inside one (25.1).
- [x] `star`: RFC 3261 20.10 has STAR as its own alternative to a contact-param, and 10.2.2
      gives it meaning. A Contact of `*` was previously parsed as a URI and recognised by the
      wreckage it left - no user, host `"*"`, marked invalid. `Registrar::is_star_contact`
      now asks the identity.
- [x] Header parameter names are lower-cased on the way in (7.3.1 makes them
      case-insensitive). A UA sending `;Tag=` or `;Expires=` was previously missed, which for
      Expires meant a binding written with the wrong lifetime.
- [x] `to_string()` emits parameters in sorted order rather than hash order, so the same
      identity produces the same bytes every time.
- [x] 14 tests from section 20.10 and 25.1, written first and watched fail. 303 tests.

## Milestone 2 step 4 - Proxy core, the rest of section 16 (2026-09-20)

- [x] Loop detection (16.3.4). The branch this node writes is now separable, as 16.6 step 8
      asks: the magic cookie, a hash of the fields that decide where the request goes, and a
      value unique to the branch. A request arriving with a Via this node wrote is a loop when
      the hash recomputes the same and a spiral when it does not, so 482 is returned for the
      first and nothing changes for the second. The topmost Via is left out of the hash on
      purpose, against the RFC's own worked example: it is the hop that handed the request
      over, which is exactly what differs between the first pass and the pass that comes back,
      so including it would make every loop look like a spiral. The reason is written where
      the hash is computed.
- [x] Route preprocessing (16.4): a strict router's rewrite is undone - Request-URI naming
      this node with the `lr` this node wrote, and the real target recovered from the last
      Route value - and a first Route naming this node is removed.
- [x] Record-Route (16.6 step 4) on every INVITE forwarded, naming the flow the hop goes out
      on, with `lr` and with `transport` when it is not UDP, and `sips` for a TLS hop or a
      sips Request-URI. This is what brings the ACK and the BYE back through a node that
      anchors media and keeps call records.
- [x] Route postprocessing (16.6 step 6): a top Route without `lr` belongs to a strict router,
      so the Request-URI goes to the end of the route set and the Route into the Request-URI.
- [x] In-dialog requests transit on their route set, with no dialog state consulted, because
      a proxy is transaction-stateful and not dialog-stateful (16.1). Target determination now
      follows 16.5 properly: a route set decides the hop and leaves the Request-URI alone, and
      a Request-URI in a domain this node serves no realm for is itself the only target. That
      second rule is the whole of it - a BYE's Request-URI is the remote target, which is an
      address rather than an address of record, so it is forwarded as it stands.
- [x] `SIPMessage::clone` and 16.6 step 1: each branch of a fork starts from a copy of the
      request as received. Forwarding the one object twice stacked this node's Via and
      decremented Max-Forwards once per attempt, so the second callee saw a different request
      from the first, and a 483 was generated from a request the proxy had already rewritten.
- [x] Stateless response forwarding (16.7 step 1, 18.1.2): a response matching no client
      transaction goes back down its Via chain with this node's Via removed, and is dropped
      rather than relayed when the top Via is not one this node wrote. They were dropped.
- [x] CANCEL forwarding (16.10): the response context is filed under the server transaction it
      answers, so a CANCEL reaches the branch already tried. RFC 3261 9.1 is honoured - one
      Via, the route set it was sent with, CSeq method rewritten, no body - and a CANCEL
      arriving before the branch has answered provisionally waits for the provisional rather
      than naming a transaction the far end does not have yet. A cancelled fork stops trying
      further targets, which is what stops the next phone ringing after the caller hung up.
- [x] 16.7 step 6: a 503 from the last branch goes upstream as a 500, because a 503 is a fact
      about the next hop and not about this node.
- [x] `Route` and `Record-Route` are `SIPIdentityHeader` rather than strings - the same
      name-addr grammar as To, From and Contact (20.30, 20.34) - so `parameter("lr")` from
      step 3 is readable where 16.12 needs it.
- [x] `Core::is_local_address` and `Core::channel_find`: what names this node, and the live
      flow to a next hop. Channels add their local endpoint as they register, so the set is
      what the node is reachable at rather than what it was configured with. The configured
      and discovered public addresses that a NAT'd or balanced node needs are M4.
- [x] 17 tests from sections 16.3.4, 16.4, 16.5, 16.6, 16.7, 16.10, 18.1.2 and 9.1, written
      from the RFC and watched fail. `tests/helpers/proxy_fixture_helper.h` is the shared
      fixture. 320 tests, clean under asan and tsan.

## Milestone 2 step 5 - Dialogs (2026-09-20)

- [x] `types::Dialog` (`src/types/dialog.h`): RFC 3261 12.1.1's list - Call-ID and both
      tags, each end's identity and remote target, the route set, a sequence number per end,
      the secure flag and the state. The two ends are named caller and callee rather than the
      RFC's local and remote, because local and remote are written from inside one UA and a
      proxy that used them would have to pick an end to pretend to be. `Dialog::matches`
      accepts the tags in either order for the same reason (12.2.2 is stated from one side).
- [x] `Dialogs` (`src/dialogs.*`): an observer, not a transaction user. Nothing routes on it
      and every request would still reach its far end with the class deleted - a proxy is
      transaction-stateful, not dialog-stateful (16.1). It exists for the question a proxy
      alone cannot answer: whether a call is still up, which is what releases media, closes a
      call record and feeds a live-calls page.
- [x] An INVITE with no To tag is a call attempt; a provisional carrying a To tag makes it an
      early dialog (12.1); a 2xx confirms it; a BYE ends it (15.1); a CANCEL ends the attempt
      but not a call already answered (9.1). A non-2xx final ends it too, so a call that never
      connected leaves nothing behind. A terminated dialog is out of the table by the time the
      change callback returns, so what is in it is what is live and nothing else.
- [x] The CANCEL case is the one worth naming: a CANCEL carries the INVITE's To, which has no
      tag, so it has to find its call by the caller's tag alone - and it has to do so after a
      180 has already given the callee one.
- [x] `Call::Participant` holds its `Dialog` rather than two bare tags. A proxied two-party
      call has one dialog end to end, so both legs point at the same one; a conference has one
      per leg with the focus (RFC 4579), which is why it hangs off the participant. The Redis
      call record persists the dialog's identity and not its route set or sequence numbers:
      those belong to the node on the path and are no use to another one, and replicating them
      is full dialog failover, which is parked.
- [x] `Core::_on_dialog_change` keeps the `Call` in step - Trying, Ringing, Connected, Closed -
      publishes `calls/<id>/state`, writes the record through and drops the call from the live
      table when it closes. This is where step 6 hangs media off: offer and answer on confirm,
      release on terminate, both already single call sites.
- [x] RFC 4028 session timers: `SessionExpiresHeader` for `Session-Expires` and `Min-SE`
      (section 4's grammar, including the `refresher` parameter and the `x` compact form), the
      interval taken from the 2xx because that is what the two ends settled on rather than what
      the INVITE asked for, refreshed by a re-INVITE or UPDATE passing through, and the dialog
      discarded when it lapses. One timer for the node, armed at the earliest deadline of any
      dialog that negotiated an interval, so a node whose endpoints do not use session timers
      pays nothing. Deadlines are on the steady clock: an interval is a duration, and a wall
      clock stepping backwards must not extend a call by an hour.
- [x] On expiry the node discards its state rather than sending a BYE to both ends. That is
      what RFC 4028 section 8 asks of a proxy; sending one would be acting as a user agent in a
      dialog this node is only on the path of. A call whose ends never agreed an interval never
      lapses at all, because ending a call nobody said would end is the worse failure.
- [x] 19 tests from sections 12.1, 12.1.1, 12.2.1.1, 12.2.2, 15.1, 9.1 and RFC 4028, written
      from the RFCs and watched fail. 339 tests, clean under asan and tsan.

## Milestone 2 step 6 - Media on the signalling path (2026-09-20)

- [x] `Proxy::_anchor_media` (`src/proxy.cpp`): a session description on its way through goes
      to the media engine first, and what comes back names this node. An INVITE carries the
      offer of the end that sent it and the response to it carries the answer of the end that
      answered (RFC 3264 section 5), so the leg the description belongs to is read from the
      dialog rather than assumed from the direction. An INVITE with no description at all
      inverts the exchange - the response becomes the offer and the ACK the answer - which is
      why the ACK is never treated as an offer.
- [x] This is a deliberate departure from 16.6, which says a proxy does not add to, modify or
      remove a body. The node does it because it is the media relay. Where it cannot - no
      engine configured, no call record, or an engine that declines - the message travels on
      exactly as it arrived, which is the proxy behaviour the RFC describes, and the call is
      not failed over it.
- [x] Forwarding waits for the engine. `_send_forward` and `_forward_response` are the far
      side of a round trip that is in-process for the builtin relay and an ng-protocol
      exchange for rtpengine; the contract was made async for this and it is now used that
      way. A CANCEL that arrives while the engine holds a description stops the branch rather
      than sending it late.
- [x] `Flags::from_sdp` (`src/media/media_engine.cpp`): ICE, DTLS, SRTP and rtcp-mux read from
      the description itself, never from the transport. A browser asks for ICE and DTLS
      whether its offer arrived over WSS or over UDP, and a desk phone on WSS is still plain
      RTP. `RTP/SAVP` in the profile is enough to say SRTP on its own (RFC 3711, RFC 5764).
      This is what lets the builtin engine decline a WebRTC offer instead of rewriting it into
      something that cannot work.
- [x] `Core::_on_dialog_change` releases the media when a call closes. A dialog ending is the
      only thing that says a call is over, which is the whole reason this node tracks dialogs
      it does not own. The call is held by the release handler, so dropping it from the live
      table does not take it away from an engine that has not answered.
- [x] The builtin relay actually bridges. Its relay sets are held per call and per stream
      rather than per leg: `RTPRelaySet` learns where a leg is from the first packet it sends
      and forwards to every other leg that has, so one port is a bridge and a port per leg is
      two sinks with nothing between them. Both legs of a stream are handed the same port.
- [x] `a=rtcp` is written whether or not the far end offered one (RFC 3605). The relay's RTCP
      port comes out of the same pool as its RTP port and is not reliably the one above it, so
      an endpoint left to assume the convention would send its receiver reports into another
      call.
- [x] `Call::participant_index` turns "which end sent this" into the index the media contract
      addresses a participant by.
- [x] 12 tests from RFC 3264, RFC 3605, RFC 8866 and the bridging concept, written from the
      standards and watched fail. The bridging one sends real UDP packets through the relay
      from two sockets and asserts each comes out at the other, because a relay that allocates
      ports and forwards nothing passes every assertion about SDP. 351 tests, clean under asan
      and tsan.

## Milestone 2 step 7 - Transports, part 1 (2026-09-20)

- [x] WSS listener (`src/servers/websocket_server.*`). A browser will not open an insecure
      WebSocket from a page served over https, so wss is not a hardening option for a web
      client but the only way in, and without it there is no first WebRTC call to make.
      One listener class rather than two: the only difference is a TLS handshake before the
      HTTP upgrade, and a node running two near-identical listeners would drift between them.
- [x] `WebsocketConnectionFor` and `WebsocketHTTPSessionFor` are templated on what carries
      the bytes, because below the websocket framing ws and wss are identical. The transport
      name is carried rather than deduced: it is the one thing that differs and the layers
      above route on it. Both are header-only now; their implementation files are gone.
- [x] The WSS handshake is asynchronous, unlike the TLS SIP listener's blocking one. It runs
      on the listener's own thread, and a client that connects and then says nothing would
      otherwise stop every other client being accepted.
- [x] `servers/tls_context.h`: one certificate loader for both secure listeners, which also
      turns off SSLv2, SSLv3, TLS 1.0 and TLS 1.1. A node whose two secure listeners were set
      up differently is a node whose security depends on which port you reached it on.
      `websocket.tls` with no certificate or no key is a startup error rather than a silent
      fall back to ws://.
- [x] `Core::channel_connect`: this node can open a connection rather than only accept one.
      Nothing in the tree could before, so a next hop with no live flow was answered 480 - a
      registered client always has one, which is why it never showed, but a trunk or a peer
      node could not be reached at all. The channel is filed under the address it reached and
      under the name it was dialled by, so a second request to a hop named by hostname reuses
      the connection; closing it takes both names away.
- [x] The attempt is bounded by `sip.connect_timeout_ms`, four seconds by default. Not an RFC
      timer, but the operating system's own bound is well over a minute and Timer B gives the
      whole transaction thirty-two seconds, which a fork with several bindings has to share.
      The timer and the connect race and whichever loses is ignored: a transaction told twice
      that its hop is unreachable would try the next target twice.
- [x] No NAPTR and no SRV: RFC 3263 is a step of its own, and this resolves the host the URI
      named the way 16.6 step 7 falls back to without service records. UDP and TLS outbound
      are refused rather than faked; outbound TLS waits on the cluster CA (M4) and
      outbound UDP to an unknown host on the UDP trunk work (M3), both in `ACTIVE.md`.
- [x] RFC 3261 18.1.1: a request over 1300 bytes with the path MTU unknown leaves over TCP
      rather than UDP, with the top Via rewritten to say so, and falls back to UDP when TCP
      is refused. The decision is made where the final bytes exist, after the media engine
      has had the body - which matters more now than it did, because anchoring makes the
      description this node forwards larger than the one it received.
- [x] `Config` defaults `datastore` to `memory://` and `events` to `local://`. A config with
      neither section left both blank and the node failed at startup, which is not the
      ten-line config and sane defaults the project promises. Found by a config test written
      for the WebSocket TLS settings.
- [x] 20 tests. The WSS ones complete a real TLS handshake against the listener, carry a
      REGISTER over the frames and read the 401 back; the 18.1.1 ones read what this node
      actually wrote off a real TCP socket. 371 tests, clean under asan and tsan.

## Milestone 2 step 7 - Transports, part 2 (2026-09-21)

- [x] Plugin contract v2. The flow identity and the `EventSystem` realignment landed in
      one `API_VERSION` bump rather than two, so a plugin author migrates once. The
      reason is recorded above the constant in `src/plugins/plugin.h`.
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
- [x] The transport's threading rule. `Connection::executor()` is the executor a
      connection's stream belongs to, and `Channel` hands every operation on the stream
      over to it: starting a read, starting a write, and the teardown in `close()`. A
      socket is not safe for two threads and each server runs its own io_context on its
      own thread, so the Core strand reaching in was a second thread inside the stream.
      `UDPServer` already worked this way and says so in its own comment; TCP, TLS and
      WebSocket did not. Found by tsan as a data race in
      `WebsocketConnectionFor::is_open()`, called from `Channel::close()` while beast's
      read op wrote the same field finishing the teardown the far end started. Measured
      before and after on `WebsocketServerTest.CarriesSipWithoutTls`: 12 failures in 30
      runs at the baseline, 0 in 30 with the fix. Three tests pin the rule itself: the
      mock connection can be given a strand of its own and records whether each call
      arrived on it.
- [x] A write queue on the channel. One write in flight at a time, the rest queued on the
      strand, and a short write resumed from where it stopped - `async_write_some` is not
      obliged to take the whole buffer, and nothing was checking how much it took.
      `Channel::write` was a declaration with no definition; it is now the way raw bytes
      go out, and what the tests drive.
- [x] Connection reuse is tested: a second in-dialog request goes out on the flow the
      first one used, and the registry still holds one channel for that hop. Opening a
      second connection per request would leave a node holding one socket per request,
      and for a client behind NAT the new one would not reach it at all.

Left for later, and recorded in `ACTIVE.md`: outbound TLS (M4, with the cluster CA) and
outbound UDP to a host this node has never heard from (M3, with the UDP trunk).

## Milestone 2 step 8 - Admin API, part 1: provisioning (2026-09-21)

- [x] OpenAPI 3 document at `docs/api/openapi.yaml`, versioned under `/api/v1`.
- [x] Bearer tokens with `admin` and `client` scopes, from `http.api.tokens`. A route
      names the scope it needs where it is declared, so adding one cannot accidentally
      leave it open, and a request with no matching token is refused - an API enabled
      with no tokens is one nobody can call.
- [x] `GET/POST/PUT/DELETE /api/v1/realms` and `/api/v1/realms/{realm}/accounts`, HA1
      computed here from the password and never given back, and `GET
      /api/v1/registrations` with the node and flow of each binding. The datastore's
      create/update split is what lets these answer 409 rather than overwrite.
- [x] JSON body parsing and one error envelope: a code a client branches on and a
      message a person reads.
- [x] `StaticMiddleware` path from `config->http_files_path`.
- [x] The admin API is off the strand: provisioning goes to the datastore with the API's
      own executor. The middleware chain had to learn to keep its session alive first,
      because it only ever held one for a middleware that answered inline. What part 2
      adds, the live registries, does need `call_on_strand`; nothing in provisioning
      does.
- [x] `GET /api/v1/nodes`: the cluster as a node knows it, with a URI per enabled
      transport. One entry today, because nothing discovers the others yet; the shape
      is what M4's discovery bus fills in. A browser reads it over the HTTP it is
      already speaking and needs no SIP extension at all, which is why it is the first
      failover piece built rather than the last. `sip.public_address` is what a node
      advertises; a node on a wildcard bind knows every address it answers on and none
      a client should use, so it has to be told.

## Milestone 2 step 9 - Test harness (2026-09-21)

- [x] `test/e2e/` with sipp scenarios: REGISTER with Digest, a wrong password, a
      retransmitted REGISTER, INVITE/180/200/ACK/BYE, CANCEL after 180, 486, 408 on
      timer B, and a call with RTP through the builtin relay. Each asserts on what the
      RFC requires rather than on what the node currently does, and `run.sh` provisions
      the realm and the accounts over the admin API first, which is the same path an
      operator uses. `docker-compose.test.yml` puts one node and two sipp containers on
      a network of their own, with fixed addresses because a callee has to register at
      an address the node can route back to. The node image builds Boost from source
      because Debian ships 1.83 and this needs 1.87 or newer; it is its own layer and
      cached after the first build. `run.sh` writes the node's own log to
      `results/node.log` before it tears the containers down, because without it a
      failing scenario is two sipp traces and a guess.
- [x] All eight scenarios pass against sipp 3.7.3. Digest authenticates end to end
      against a real client, and it picks the SHA-256 challenge rather than the MD5 one,
      so the RFC 8760 ordering worry was unfounded: sipp is built with SHA256 support and
      follows section 2.4.
- [x] The one real server bug the harness found: the node wrote `0.0.0.0` into its own
      Via and Record-Route on a UDP flow, both being built from
      `connection->local_endpoint()` with the listener on the wildcard.
      `Core::advertised_address` now answers `sip.public_address` when it is set and the
      flow's own address otherwise, for all three of Via, Record-Route and Service-Route,
      and `channel_register` files the advertised address as one of this node's own, or
      the Record-Route it wrote comes back as a Route it does not recognise and 16.3.4
      catches the BYE as a loop. The public port per transport and the localnet split
      are still M4.
- [x] Harness bugs fixed on the way, each worth knowing: `run.sh` computed the
      repository root as `test/`; sipp expands a `[field]` before it reads the
      `[authentication]` keyword around it, so credentials go on the command line; sipp
      runs as PID 1 in every container and builds its branch from the pid and the call
      number, so each scenario gets its own source port or the second looks like a
      retransmission of the first; the port allocator's counter was incremented in a
      `$(...)` subshell; a callee scenario cannot register and then wait for an INVITE
      in one run, because sipp binds a scenario instance to one call, so registration
      is its own short run; `cancel_after_180.xml` built the CANCEL with `[branch]`,
      which sipp derives per message, and the branch is now read back off the 180 (9.1
      requires the CANCEL's Via to be identical to the INVITE's); every scenario gives
      its callee a port of its own and clears the account's bindings first with a
      `Contact: *` and `Expires: 0` (10.2.2), from a port of its own so the two REGISTERs
      are not one transaction under 17.2.3; `invite_media.xml` named the G.711 capture
      at sipp's upstream path where Debian's package puts it in `/usr/share/sip-tester/`.
- [x] One deviation the harness surfaced and did not fix: a CANCEL matching no response
      context is answered 200 and dropped where 16.10 says to forward it statelessly.
      It is an M3 item in `ACTIVE.md`.
- [x] `Dialog` unit tests, 19 of them in `tests/dialogs_test.cpp`, had already landed
      with step 5.

## Milestone 2 step 10 - Smaller items surfaced by the review (2026-09-21)

- [x] Digest with SHA-256 (RFC 8760) alongside MD5. An account carries a credential per
      algorithm, both computed while the password is in hand because neither can be
      derived from the other. The registrar challenges with SHA-256 then MD5 and checks
      against whichever the client answered with; an account imported as a bare MD5 hash
      is challenged again rather than let in. Sending the algorithm at all needed the
      Authorization serialiser to stop quoting tokens.
- [x] `Subscriber` is `Account`, decided with Tom. It would have collided with SUBSCRIBE
      (RFC 6665) the moment presence arrived in M6. The Datastore contract kept its
      shape and changed its vocabulary, so `API_VERSION` is 3; the Redis keys moved with
      it, and the event topic is `account/<uri>/status`.
- [x] `RTPProxyClient` is out of the build path, the way Lua is: it stays in the tree as
      the start of an rtpproxy driver and is not compiled until someone writes one.
- [x] `docs/architecture.md` rewritten from the Architecture section of the plan: the
      strand and what is not on it, the transport layering, the plugin registry with the
      two implementations per kind, the cluster shape and the media model. It says in
      as many words that DynamoDB, NATS and Kafka are things the contract makes possible
      rather than things the core plans to build, and it points at `docs/plugins.md`.
- [x] The rest of the documentation audited. `design.md` was still right about the
      transport layering and wrong about everything that has happened since, so it
      gained the threading rule, the flow id and the write queue, and its three broken
      source links were fixed. `goals.md` was an empty file and is gone. `scripting.md`
      described a Lua engine that is not compiled; it now says so, and says what routing
      policy comes back as. `modules/` no longer exists. `quick_start.md` told the reader
      to install rtpproxy and log in as accounts that never existed; it is now the actual
      quick start, provisioning over the admin API. `README.md` claimed TLS-only defaults
      the shipped configuration does not set and listed WebSockets as a future feature
      two milestones after they landed.

## Milestone 2 - the leftovers closed (2026-09-21)

Items the steps had deferred to themselves, done before 0.5.0.

### Transactions and registrar

- [x] RFC 2543 fallback transaction matching. A request whose topmost Via carries no
      branch, or one with no magic cookie, is keyed by 17.2.3's fallback tuple -
      Request-URI, From tag, Call-ID, CSeq number, the whole topmost Via and the method -
      instead of by branch and sent-by. Both forms are strings in the one table, so
      nothing above the matcher knows which kind it holds. The To tag is the one field
      of the rule left out, with the reason recorded at `_legacy_key`: it would stop an
      ACK ever matching the INVITE that has no tag, and the case it exists for is already
      covered because a 2xx terminates the server transaction (17.2.1). A branchless
      request used to be answered 400 for want of an identifier.
- [x] 423 Interval Too Brief with `Min-Expires`. `Realm` grew a `registration_minimum`
      beside its `registration_timeout`, provisioned over the API and zero by default,
      because the RFC's own advice in step 7 is that a registrar should accept brief
      registrations unless the refreshes are costing it something. When it is set, the
      three conditions are all of them: greater than zero, under an hour, and under the
      minimum - a zero expiry is a removal and an hour is never too brief. The refusal
      quotes the minimum in `Min-Expires`, which is what makes it something a client can
      act on, and registers none of the contacts.
- [x] RFC 3608 Service-Route. The 200 OK to REGISTER carries this node, on the flow the
      REGISTER arrived over, with `lr`, naming `sip.public_address` when one is set. It
      tells a client where to send everything that follows, which for a client whose
      Contact is unroutable - a browser's always is - is the only thing that makes the
      next request work. It is also half of failover: a client that re-registers on
      another node is told that node's route by that node and needs no DNS to learn it.
      `Path` and `Service-Route` are registered header types now, so both are parsed
      rather than echoed.

### Proxy

- [x] Timer C (16.6 step 11). The branch that answers 180 and never stops is now given
      up on. Nothing else was watching it: the first provisional response moves the
      INVITE client transaction to Proceeding and cancels timer B (17.1.1.2), so from
      the first 100 Trying onwards the branch had no bound at all and held the caller,
      the response context and the dialog for as long as the node ran. The timer hangs
      off the response context, which is where 16.6 puts it, and is reset by a 101 to
      199 (16.7 step 2) but not by a 100. On firing it follows 16.8: a branch that has
      answered provisionally is sent a CANCEL and given one more interval to answer it,
      and one that ignores that has its client transaction terminated and is treated as
      though a 408 came back. `sip.timers.c_invite_proxy_ms` configures it, and a value
      at or below the RFC's three-minute floor is refused.
- [x] The `o=` line carries this node's address. Every `c=`, `m=` port and `a=rtcp` was
      rewritten and the `o=` was not, so a node anchoring media so that neither end
      learns the other's address handed one of them away in it anyway. RFC 8866 section
      5.2 allows the substitution in as many words, on the condition that the field stays
      globally unique, so the username and session id the endpoint chose are kept and
      only the address, nettype and addrtype are replaced.

### Deciding a call is over

Nothing before 0.5.0 let a node decide for itself that a call it was holding had ended.
All of these release the call and send no BYE, which is what RFC 4028 section 8.3 allows
a proxy and no more; `config.example.yaml` has the table of which reaches which call.

- [x] RFC 4028 section 8, the proxy's own say in the negotiation.
      `Config::sip_session_min_se` was configuration nothing read, and the node accepted
      whatever the two ends agreed. 8.1 now applies to every INVITE and UPDATE: an
      interval below the minimum is answered 422 Session Interval Too Small with `Min-SE`
      when the caller advertises `Supported: timer`, and raised on the way through when
      it does not, because a 422 a caller cannot read would only fail the call. A `Min-SE`
      already in the request is raised and never lowered, and the `refresher` parameter
      is never touched. 8.2 covers the other end: when the caller asked for a timer and
      the callee answered without one, the 2xx gains the remembered interval with
      `refresher=uac` and `Require: timer`, before the dialog tracker reads it rather
      than after, or this node would watch nothing while the caller refreshed on an
      interval this node handed it. The config floor of 90 the RFC sets is enforced.
- [x] `sip.media_timeout` (300s, 0 off). A call between two endpoints that never offered
      a timer has no interval and never lapses, and inserting `Session-Expires` only
      helps where the far end implements RFC 4028, which is precisely not the case that
      leaks. The media plane answers it instead: `RTPRelaySet` records when it last
      carried a packet, the engine reports the shortest idle of a call's relays as
      `idle_seconds` through `query()`, and the sweep on `Core` acts on it. RTCP counts
      with RTP so hold and silence suppression do not read as dead.
- [x] `sip.session_expires`, 1800 by default per section 4's recommendation and 0 to
      leave such a call alone: 8.1's other half, inserting `Session-Expires` where a call
      offered none. It covers the gap the media sweep cannot, a call whose media goes end
      to end, and reaches any call whose far end implements RFC 4028. Safe by
      construction: section 9 Table 2 has a timer-aware UAS take the refreshing itself
      when the UAC cannot, and 8.2 leaves the call with no expiration at all when neither
      end does, which is where it started. An interval already in the request is
      untouched, an inserted one is never below a `Min-SE` the request carried, and no
      `refresher` is ever named, because 8.1 forbids it.
- [x] `sip.max_call_duration`, off by default. The backstop for the call neither of the
      others reaches: media end to end, and two endpoints that have never heard of RFC
      4028. Measured from the dialog's confirmation on the steady clock for the same
      reason the session deadline is. The sweep on `Core` does both questions now, and
      the cheaper one, how long the call has been up, runs first and needs no engine.
- [x] `sip.require_session_timer`, off by default. RFC 4028 8.1 allows `Require: timer`
      and calls it NOT RECOMMENDED in the same breath, and the reason is concrete: an
      endpoint that does not implement the extension answers 420 Bad Extension, so the
      call fails outright rather than going without an expiry. 8.1 also says to add it
      only where the caller said nothing about session timers, which is what the code
      does. Documented with the failure mode spelled out rather than the option alone.

### Types

- [x] `types::URL` (`src/types/url.cpp`) is hand-written. The last regex worth
      replacing, and it was hiding two things: a port that was not digits was read as
      part of the path and the URL called valid, so a typo in a datastore address
      surfaced as a connection failure much later; and `to_string` dropped the port of
      any scheme with no well-known one, which is every scheme this project invents.
      Parsing into an object now clears what went before, and the default-port lookup is
      case-insensitive the way RFC 3986 3.1 says a scheme is.

## Milestone 2 - complete (0.5.0, 2026-09-21)

A standards-compliant call on one node: INVITE / 18x / 200 / ACK / BYE / CANCEL between
two accounts over UDP, TCP, TLS, WS and WSS, plain RTP through the builtin engine,
provisioned through the admin API, verified by sipp. 472 tests at the tag, clean under
asan and tsan, the Redis suite verified against a real server. The items each step left
for later are in `ACTIVE.md` under the milestone that picks them up.

## Milestone 3 - A browser calls AthenaPhone through rtpengine, in progress

Everything down to "The mixed-transport call" shipped in 0.6.0 and 0.7.0. What follows it
is on `develop` and not yet tagged, as is all of Milestone 5 below.

What the sections below add up to, against that goal: the browser's side of the
signalling is done - WSS, a binding reached on the flow it registered over, and a flow
token in the Record-Route so the ACK and the BYE reach a Contact that resolves to
nothing; the engine is done and proven against rtpengine 9.4.0; and a realm can say
its endpoints are all WebRTC, which for this goal is true. What is not done is in
`ACTIVE.md` under Milestone 3, first item first.

### Each branch goes down its own binding's flow (2026-09-21)

- [x] RFC 5626's routing half, and the last of step 1's leftovers. A user registered
      from a desk phone and a browser had two bindings and one connection, because
      `Core` kept one flow per account - the last to register - and the fork sent both
      attempts down it, so one device rang twice and the other never. Each `Target` now
      takes the flow its own binding recorded: `Proxy::_flow_for` asks
      `Core::channel_find(flow_id)` for the flow the registration was made over and falls
      back to resolving the Contact when the binding named no flow or the flow has since
      closed, which is right for a desk phone with a routable address and hopeless for a
      browser, whose attempt then fails and the fork moves on. `Core::account_get_channel`
      and the account-to-channel index are gone. 430 Flow Failed for a binding whose flow
      has gone is RFC 5626 section 11 and is in M4 with the rest of outbound.
- [x] `tests/proxy_flow_routing_test.cpp`: the invariant stated so that it holds
      whichever order the bindings come back in - every INVITE written to a connection
      names, in its Request-URI, the device on the other end of that connection. 474
      tests.

### The rtpengine media engine (2026-09-21)

M3's first item, and the engine the milestone is named for: ICE, DTLS and SRTP are what
a browser requires, and the builtin relay declines them rather than answering an offer
it cannot carry.

- [x] `Bencode` (`src/media/bencode.*`), hand-written per the dependency rule and small
      enough that the rule costs nothing. A dictionary is a vector of pairs kept sorted
      by key rather than a `std::map`: canonical bencode wants byte order anyway, only
      `std::vector` is guaranteed to hold an incomplete type, and a request that encodes
      the same way every time is one a test can compare and a retransmission cannot
      accidentally change. The decoder reads datagrams off a socket, so it is written
      the way the SIP parsers are - recursion bounded at 32, every length checked
      against what is actually there, an integer refused rather than wrapped, and the
      two spellings of one number ("i007e", "i-0e") refused because the protocol is
      keyed by exact bytes.
- [x] `RtpengineMediaEngine` (`rtpengine://host:port`), with the ng protocol's `ping`,
      `offer`, `answer`, `delete`, `query`, `start recording` and `stop recording`.
      Capabilities are `bridge`, `record` and `transcode`; not `conference`, because
      rtpengine's publish/subscribe is an M6 item and claiming it would have a caller
      ask for a mix and get a relay.
- [x] `connect()` is a ping that has to be answered `pong`. A UDP socket opens whether
      or not anything is listening, so without it a node would start, report a media
      engine, and fail the first call instead of failing to start.
- [x] Retransmission, because the ng protocol is UDP and a request can be lost.
      rtpengine caches its answer against the request's cookie, which makes asking again
      a request for the same answer rather than for the work twice. `timeout_ms` bounds
      one attempt rather than the operation: three at half a second is well inside the
      thirty-two seconds timer B gives a transaction that a fork has to share.
- [x] The tags. rtpengine files media under the tag of the end that offered it, so an
      offer names the offering participant's own tag and an answer names the offerer as
      from and the answerer as to - the reverse of the participant that handed over the
      SDP. Getting this the wrong way round makes a second, unrelated call rather than
      an error anybody would see, which is why it has a test of its own.
- [x] `replace: [origin, session-connection]` on every offer and answer, which is the
      substitution the builtin engine makes by hand and what keeps a node that anchors
      media from handing one end the other's address (RFC 8866 section 5.2).
- [x] `query` answers `idle_seconds` in the shape the call sweep already reads, from the
      per-stream `last packet` rtpengine reports: the shortest idle across the call,
      because one stream still carrying is a call still up. A stream nothing has ever
      arrived on is idle from when the call was created rather than not idle at all,
      which is what the builtin relay reports too - its counter starts when the relay
      does - and what stops a call that never carried media living for ever.
- [x] The socket is held by `shared_ptr` behind a mutex and closed on the strand by a
      handler that owns it, because `close()` is called from the shutdown path and a
      socket is not safe for two threads. That is the same rule the transport follows
      for a connection's stream, and it was learned there the hard way.
- [x] `MediaEngine::start_recording` and `stop_recording` on the contract, declining by
      default the way the conference operations do, so the `record` capability a driver
      advertises is one a caller can act on. `API_VERSION` is 5.
- [x] Fixed on the way: `register_builtin_media_engines` and its two siblings were
      non-inline functions in headers, so a second translation unit including one was a
      duplicate symbol. Only ever included once until now.
- [x] `gtest_discover_tests` is given a 60 second `DISCOVERY_TIMEOUT`. Listing the
      binary's tests takes 2.3s idle and had been exceeding the 5 second default on a
      loaded machine, which fails the build rather than a test.
- [x] 30 tests. `tests/helpers/fake_rtpengine_helper.h` is an rtpengine that is not
      rtpengine: a UDP socket on localhost that decodes the ng datagram, records it, and
      answers what the test told it to, because a driver for a wire protocol is only as
      good as what it puts on the wire and the only way to assert on that is to be the
      other end of the socket. It also makes the two otherwise untestable cases easy: an
      engine that answers an error, and one that does not answer at all.

What this deliberately does not do, recorded against the next M3 item: no `ICE`, `DTLS`
or `transport-protocol` flag is sent, so rtpengine mirrors what it was handed. That is
right for a call whose two ends are alike and wrong for a browser calling a desk phone,
and choosing between them needs to know what the far leg is - which at offer time is the
flow's transport and the realm's policy, not anything in the description this node was
given.

### The media profile (2026-09-22)

- [x] `media::Flags` carries a target `Profile` - `Mirror`, `PlainRtp` or `WebRtc` -
      saying what the description this node is about to produce has to be, as distinct
      from what the one in hand is. The rtpengine driver had been sending no ICE, DTLS
      or `transport-protocol` flag at all, so rtpengine mirrored what it was handed and
      a WebRTC offer produced a WebRTC offer to the callee: right for browser to
      browser, and exactly wrong for the browser-to-desk-phone call the milestone
      exists for.
- [x] It could not be decided inside the engine, which is why it is on the contract.
      The question is about the far leg and at offer time no description from that leg
      exists; what does exist is the transport of the flow the message is going out on,
      and a browser reaches a node over a WebSocket and nothing else (RFC 7118). The
      proxy knows that flow and the engine sees neither it nor where the message is
      going. `API_VERSION` is 6, and `Mirror` is the default, so a driver that ignores
      the field behaves as it always has.
- [x] The proxy sets it from the outgoing flow: the channel a request is forwarded on,
      and for a response the flow the request arrived over, because that is the end the
      answer goes back to. Reading the wrong end there is how a browser is handed an
      answer with no ICE, so it has a test of its own.
- [x] The driver maps it onto the ng protocol: `ICE` force or remove, `DTLS` passive or
      off, `UDP/TLS/RTP/SAVPF` or `RTP/AVP`, and `rtcp-mux` offered and required or
      demuxed. Passive towards a browser because the browser starts the handshake.
- [x] 7 tests, including a recording engine that answers what the proxy told it, because
      what is being asserted is what the proxy says to an engine rather than what an
      engine does with it.

### Behaviour tests for the code nothing had tested (2026-09-22)

A coverage measurement found 76.9% of the shipped tree covered, with the gap
concentrated in whole files no test linked: the MQTT event system, the TCP, TLS and UDP
listeners, the static file middleware and the async queue. Tests written from the RFCs
and from what each piece promises - not from what the code did - found nine bugs.

- [x] **The transport, RFC 3261 18.3.** A datagram is one message, whole or not at all,
      and Channel was treating it as a stream: whatever a datagram did not finish stayed
      in the buffer waiting for the next one. A datagram whose Content-Length exceeds
      its body MUST be discarded, and instead the next datagram became its body, so one
      sender could swallow the next request on a flow they share. A datagram that was
      not SIP left its bytes in the buffer, the next was appended to them, and the
      result parsed as a request whose start line is nonsense - which `Channel::receive`
      then logged by calling `first_line()`, which throws. That exception unwound out of
      the read handler and through `io_context::run()`, so any peer could terminate the
      node with two packets, or one on an established TCP connection. Framing is split
      in two and named for the rule each half implements, a log line uses a summary that
      cannot throw, and the read handler catches what the message path throws. 13 tests
      over TCP, TLS and UDP against real listeners and real sockets.
- [x] **The async queue.** It is the handover between a datagram arriving on the UDP
      server's thread and a channel asking to read on the Core strand, and both queues
      were mutated outside either mutex: eight threads pushing while eight registered
      readers segfaults the process, reachable from UDP traffic on a live node. One
      mutex covers both, held across the post as well as the pairing, because the order
      handlers are queued in is the order the datagrams arrived in. 6 tests.
- [x] **The static file middleware.** It treated the whole request target as a filename,
      so `bundle.js?v=2` was a request for a file of that name: every built web
      application cache-busts with a query string and the admin client is one, so a
      second deployment served a blank page. Its containment check also compared
      canonical paths as strings, so a directory whose name merely begins with the
      document root's counted as inside it, and a symlink in the root is enough to reach
      one. 10 tests, most of them ways of asking for something on the other side of the
      boundary.
- [x] **The MQTT event system**, which had no tests at all and four bugs that only
      appear with two clients, which is every cluster. The client identifier defaulted
      to a constant, and MQTT makes a repeated identifier mean "disconnect the other
      one", so two nodes would trade the connection back and forth; it is the node id
      now. A message matching two of one client's filters arrived twice at each, because
      the broker sends a copy per subscription and the driver fanned every copy out to
      every matching subscriber; each subscription now carries an MQTT 5 subscription
      identifier and a message is routed by the one it names. `connect()` reported
      success as soon as the client had started, which says nothing about the broker for
      an always-online client, so a node with the wrong address ran with no bus; it
      answers on a round trip now, bounded by `connect_timeout_ms`. Closing asked
      whether the client was connected rather than whether its thread was running, and
      the run loop clears that flag itself when the broker drops the connection, so a
      node whose broker went away terminated on shutdown with a joinable thread. The
      prefix was concatenated without a separator. 11 tests, skipped without
      `ATHENA_TEST_MQTT_URL`, the way the Redis ones are.
- [x] **Dead code deleted.** `SupportedHeader` was registered by a static object in a
      header nothing includes, so it was in neither binary, and it was redundant anyway
      because `Supported` is in the list-valued set and the parser already splits it.
      Five `Call` helpers and `MediaStream::stop` had no call sites. `test/test/` was a
      pair of empty directories left by a harness bug. The Lua engine and the rtpproxy
      client stay: both are parked deliberately, with the reasons in `ACTIVE.md`.
- [x] Three listeners gained the `port()` accessor the WebSocket one already had, so a
      test can bind port zero, and all three log the port they actually got rather than
      the one they were configured with. `register_builtin_datastores` and its two
      siblings were non-inline functions in headers, which only worked while each was
      included once. `gtest_discover_tests` has a 60 second timeout, because listing
      takes 2.3s idle and was exceeding the 5 second default under load.

### The relay's socket, and the coverage the tests bought (2026-09-22)

- [x] `RTPRelaySet::start` called `read()` directly, so `async_receive_from` was issued
      from whatever thread called in while the io thread was already using the same
      socket, and `stop()` closed it from there too. ThreadSanitizer found it once the
      async queue test made the process io thread start earlier in the run than it used
      to. Both are handed to the io context now, with `dispatch` so a caller that is
      already the io thread - which the Core strand is - runs inline; closing then
      waits, because the port is back in the pool the moment `release_relay_set` returns
      and the next allocation may bind it.
- [x] The relayed payload is copied out of the receive buffer. A send holds a reference
      to its buffer until it completes and the next read was armed immediately
      afterwards, so a relay under load could forward whatever arrived next in place of
      what it meant to.
- [x] Coverage of the shipped tree, measured with llvm-cov across both binaries so that
      a file no test links counts as zero rather than disappearing: 76.9% line and
      81.0% function before this work, 86.3% and 90.5% after. Nine files had no
      coverage at all and now only `main.cpp` has none. What is left below 80% is the
      admin API's error paths, configuration parsing and the standard-output logger.

### The mixed-transport call (2026-09-22)

0.7.0. A browser calling a desk phone is the call Milestone 3 exists for, and what it
needed was the signalling around the engine rather than the engine itself.

- [x] **A realm says what its calls ask of the engine.** Two decisions had nowhere to be
      made: anchoring happened whenever an engine was configured, and a leg's profile
      was read from the transport of its flow, which is right wherever a WebSocket
      means a browser and wrong for a realm whose WebSocket clients are SIP phones,
      because RFC 7118 is SIP over WebSocket and requires no WebRTC. `media_anchor` off
      leaves every description untouched and the media end to end, which is what a node
      with no engine does and is now a choice rather than a default. `media_profiles`
      is `transport`, `mirror`, `rtp`, `webrtc` or `srtp`. A name nobody recognises
      leaves the policy alone, because changing what a node does to media over a
      misspelling is the worse failure.
- [x] The policy is read where the realm is already in hand, in target determination,
      and kept on the call. Reading it again per message would put a datastore round
      trip on the media path, and an in-dialog re-INVITE never looks a realm up, so
      hold and resume would behave differently from the INVITE that started the call.
      Provisioned over the admin API, persisted by both datastores, in the OpenAPI
      document and the configuration guide.
- [x] **The SRTP profile**, for a phone that wants its media encrypted and has never
      heard of DTLS: RFC 4568 puts the keys in the description, so it is RTP/SAVP and
      not a browser's UDP/TLS/RTP/SAVPF, and an endpoint offered the wrong one cannot
      answer. No transport tells the two apart, so unlike the others this profile can
      only be asked for. `API_VERSION` is 7.
- [x] **Record-Route on both interfaces, with a flow token in each.** RFC 5658 requires
      the pair of a proxy whose request arrives on one interface and leaves on another,
      which a browser calling a desk phone always is. The pair is written in every case
      because of the second reason: a browser's Contact resolves to nothing, so once
      the route set was spent an in-dialog request had only the remote target to go on
      and the ACK and the BYE of a call to a browser went nowhere however well the
      INVITE had been routed. A call that cannot be hung up is worse than one that
      cannot be made.
- [x] Each value carries a flow token in its user part, where RFC 5626 section 5.1 puts
      one. The topmost names the interface the request is sent on and the second the one
      it arrived on, which leaves each end holding the value facing it: a UAS reads the
      route set from the request in order and a UAC from the response reversed. This
      node strips both of its own on the way back (RFC 5658 section 3.2, or it would
      forward the request to itself) and the last one stripped names the flow. The token
      is random rather than derived: the flow id is the far end's address, and a route
      set that spelled it out would hand one end of an anchored call the other end's
      address, which is what anchoring is there to prevent. It lives on the channel and
      dies with it, and a token naming a closed flow falls back to the Contact.
- [x] **RFC 3264 section 8's version rule.** An offer that changes the description must
      carry a higher version and one that does not must carry the same, and the rule is
      about what an offerer emits. Towards each far end this node is the offerer, so
      passing the endpoint's version through said nothing: two offers an endpoint
      considered identical can come out of here different, because a stream released
      and re-anchored gets other ports. The builtin engine keeps the last description
      it emitted per call and per leg with the version blanked, and emits the same
      version when nothing else changed and one higher when it did; the first keeps the
      endpoint's, so a call that never re-offers looks as it did before. rtpengine has
      the rule natively and is asked for it with `sdp-version` in `replace`.
- [x] **A CANCEL with no response context is forwarded** (RFC 3261 16.10), which the
      sipp harness had recorded as a deviation in September. Answering it 200 and
      dropping it told the caller the branch had been cancelled when nothing downstream
      had been told anything. The 200 is now sent only where there is a context, because
      answering and forwarding both sends the caller two answers to one request, and
      where there is nowhere to forward it the answer is 481, which is what 9.2
      prescribes and is true of this node in a way that 200 was not.
- [x] 26 tests. 576 in all, clean under asan and tsan, and the sipp harness still passes
      all eight scenarios.

### A fixture to point a real client at, and rtpengine on the media path (2026-09-22)

"Does AthenaSIP work with X" had been a thing to believe rather than a thing to run,
and the rtpengine driver had only ever spoken to a fake socket of my own writing.

- [x] `test/interop/`: a node published on loopback, provisioned over the admin API,
      listening on UDP, TCP, TLS and WebSocket, for a client under test to be pointed
      at. The ports are the ones AthenaPhone's integration harness already uses for its
      Asterisk fixture, deliberately, so that one client can be pointed at either
      without being rewritten - which also means only one of them can hold those ports,
      so every port is overridable and the container binds the same number the host
      publishes. A node writes its own local port into Via and Record-Route, so a
      mapping that moved the port would tell a client to come back to one nothing is
      listening on.
- [x] `test/interop/smoke.py`: a dependency-free client that registers on every
      transport. REGISTER, the 401, the Digest, the 200. Run it before blaming
      anything: if it passes and a real client does not, the difference is in the
      client. It earned its place twice before it was committed.
- [x] **The WebSocket listener insisted on a path of `/`** and answered anything else
      400. RFC 7118 names no path and every client picks its own; `/ws` is what
      Asterisk, Kamailio and FreeSWITCH serve and what a client arrives configured
      with, so an ordinary client could not connect at all. The listener has a port to
      itself and serves nothing but SIP, so there is nothing for a path to distinguish.
- [x] **The shipped CA was unverifiable.** It carried `basicConstraints CA:TRUE` and no
      `keyUsage`, which a strict verifier refuses outright: RFC 5280 section 4.2.1.3
      requires `keyCertSign` to be asserted where the extension is present. These files
      exist so TLS and WSS can be tried without a certificate authority, and if a
      client that verifies properly cannot verify them then "try TLS" means "turn
      verification off". Regenerated by `tls/generate.sh`, which is new; the server
      certificate also gained `extendedKeyUsage serverAuth` and names every address a
      local test reaches it on.
- [x] `test/e2e/run.sh --rtpengine`: the same eight scenarios with rtpengine 9.4.0 on
      the media path instead of the in-process relay, as a compose overlay rather than
      a second harness. rtpengine's own log shows the offer, the answer and the delete
      arriving and being answered, both sides of the call bridged, and 101 packets of
      G.711 relayed with no errors.
- [x] And the assertion that run needed. A media scenario passes whether the engine
      relayed the call or declined it, because a declined description travels on
      untouched and the two sipp containers reach each other directly. The engine's
      counters are the only thing that says which happened, so the run now fails when
      they are zero or when anything was rejected. Verified by putting a fault back.
- [x] Two harness faults it found, neither of them the node's. Both ends of every
      paired scenario tagged themselves `[call_number]`, which is 1 in each container,
      so every call ran with a dialog whose two tags were identical: the in-process
      relay does not key on tags and bridged anyway, and rtpengine collapsed the call
      into one side with no endpoint. The media scenarios also offered PCMU and played
      sipp's A-law capture, so every packet carried a payload type the SDP did not
      name; the relay does not inspect payloads and rtpengine, correctly, dropped all
      101 of them.

### A leg is profiled from what it has said, not from its transport (2026-09-23)

The first Milestone 3 item, and the thing that made a browser calling an AthenaPhone
fail: AthenaPhone's media is WebRTC on every transport it signals over, so reading a
leg from its flow offered it RTP/AVP it cannot answer.

- [x] **A participant remembers its own profile.** `Call::Participant::profile` is set
      from every description that arrives from that leg, read before the engine
      rewrites the body, and read back when producing for that leg. A description says
      exactly whether the end that wrote it asked for ICE, DTLS or SRTP, which is not a
      guess in the way its transport is. `media::Profile` moved out of `Flags` into
      `media/media_profile.h` so a `Call` can hold one; `Flags::Profile` is still the
      name the plugin contract uses.
- [x] **The realm no longer contradicts a leg that has spoken.** The policy was applied
      to every description including answers, so under `media_profiles: webrtc` an
      answer going back to a plain-RTP caller was told WebRTC - worse than the
      transport guess it replaced. The realm's setting, and where it has none the
      transport of the outgoing flow, now decide only for the first description
      produced towards a leg this node has never heard describe itself.
- [x] **The delayed offer falls out of the same memory.** Where the INVITE carries no
      description the 2xx is the offer and the ACK the answer, and the ACK's answer is
      now produced for what the callee said rather than for a request body that says
      nothing.
- [x] **A description that will not parse teaches this node nothing.** `Flags::stated()`
      is empty unless the SDP was readable, so a leg is never quoted on something it
      did not say.
- [x] Six tests, four of which failed first. 583 in all, clean under asan and tsan.
      Removes the deviation of the same name from `ACTIVE.md`.

### The fixture a browser can actually use (2026-09-23)

Two Milestone 3 items that turned out to be one problem: a browser on the host and a
phone on the LAN cannot reach anything the end-to-end harness builds, because it keeps
every container on a closed bridge. Everything below is about being reachable.

- [x] **rtpengine, reachable.** `test/interop/docker-compose.rtpengine.yml`, selected by
      `up.sh --rtpengine`. The engine is given `--interface=<its own address>!<this
      host's address>`, so it binds what it has inside Docker and writes into the
      description what the client can send to, and its media range is published to the
      host - an address a packet cannot arrive on being no better than the container's
      own. Proven by asking it for an offer over ng: `c=IN IP4 10.35.1.132` on port
      23000, with all 21 published ports held open on the host.
- [x] **The fixture moves off loopback.** `up.sh` works out this host's address on its
      own network, advertises it, names the realm after it and publishes every port on
      `0.0.0.0`. `ATHENA_INTEROP_PUBLIC_ADDRESS` and `ATHENA_INTEROP_BIND` override both
      halves; loopback is still the default without `--rtpengine`.
- [x] **TLS off loopback did not verify, and the fixture's own smoke test found it.**
      The checked-in certificate names loopback and nothing else, so a fixture
      advertising this machine's address served one that did not name what the client
      dialled - the same failure as the unverifiable CA, arrived at from the other side.
      `tls/generate.sh` gained `--server-only --out DIR <name>...`, which signs for the
      addresses asked for with the existing CA, so a client that already trusts
      `tls/ca/snakeca.crt` needs nothing new; `up.sh` runs it whenever the fixture is
      off loopback, with a random serial so a fixture run never rewrites the checked-in
      `snakeca.srl`. All four transports registered after that.
- [x] **The engine is one line of config.** `config.yaml.template` carries both driver
      blocks and a rendered `media.url`, because a plugin reads its own YAML root and
      the one that was not selected reads nothing. The node is the same node either
      way, which is the plugin contract doing its job.
- [x] **A reload of a client-side route answered 404.** `StaticMiddleware` served files
      and nothing else, so every page of the admin console worked only if you had
      navigated to it from somewhere else. A path with no extension, no empty segment,
      not under `/api/` and asked for with GET or HEAD now gets `index.html` - the
      document that knows how to route it. A missing asset still 404s, because a bundle
      that answers 200 with HTML in it is far worse to debug; an unknown path below the
      API still 404s, because answering it with a web page would be a lie about what
      this node serves; and `//etc/hosts` is not a route either, having an empty
      segment, which is somebody trying the root as a prefix rather than a page being
      opened.
- [x] **The MIME map could not name what a built bundle is made of.** `.svg` was
      `image/svg` rather than `image/svg+xml`, and `.mjs`, `.map`, `.ico`, `.woff`,
      `.woff2` and `.wasm` were absent, so they arrived as `application/octet-stream` -
      which a browser refuses for a module script.
- [x] `up.sh --admin` mounts the admin client's build read-only at `/admin` and turns
      `http.files` on, refusing to start a node that would serve nothing and saying how
      to build it. Mounted rather than checked in: a bundle copied into this tree is one
      that goes stale.
- [x] `smoke.py --realm` follows `--host`, since the realm is named for the address
      dialled and that is no longer always `127.0.0.1`.
- [x] Checked against the real node with the admin build mounted: `/` and
      `/diagnostics/softphone` 200 as `text/html`, the bundle 200s as
      `application/javascript`, a missing asset and an unknown API path 404,
      `/api/v1/health` still answers, all four transports still register. Four new
      tests, one of which failed first, and both harness runs unchanged.

### A browser calls a browser through this node and rtpengine (2026-09-23)

The first "to the first call" item, and the first time media has gone end to end
through AthenaSIP between two real WebRTC endpoints rather than between two sipp
containers reading a capture file. The decision that the admin client's softphone is
the harness's browser page is dated 2026-09-23 in `ACTIVE.md`.

- [x] `test/interop/browser.sh`: brings the interop fixture up with `--rtpengine
      --admin`, runs the Playwright spec that lives beside the page in
      `../athenasip-admin`, and takes the fixture down on the way out so a failed run
      never leaves a node published on `0.0.0.0`. It checks what the other repository
      owes it first - the built page, Playwright, a downloaded Chromium - and says how
      to fix each, rather than starting containers it cannot drive.
- [x] `up.sh` writes `generated/fixture.env` every run: what the fixture actually turned
      out to be, including the LAN address it detected. The wrapper sources it, so
      nothing guesses an address or a port a second time and the alt-port case needs no
      special handling anywhere.
- [x] **Two calls, both directions, passing.** 1001 calls 1002 and hangs up; 1002 calls
      1001 and the callee hangs up. Both ends of both calls: `call connected`, ICE
      connected, DTLS connected, Opus, packets in both directions, none lost. The
      selected remote candidate is `10.35.1.132:23000`, `:23010`, `:23014` - the LAN
      address rtpengine advertises and the ports the host publishes, which is the item
      above proven from the endpoint's side rather than from the engine's.
- [x] rtpengine's own view of the same calls, which is the second witness: ~2500 packets
      per leg, `UDP/TLS/RTP/SAVPF`, `AEAD_AES_256_GCM`, flags including `DTLS
      fingerprint verified`, `rtcp-mux`, `ICE`, `trickle ICE`, 0 errors and 0 loss.
- [x] **One bug, and it was in the page.** The first run failed on both tests with the
      media flowing: `iceConnectionState` never reported connected because JsSIP
      announces an outgoing call's peer connection before it announces the session, so
      the page's controller subscribed too late. Found by putting the engine's counters
      beside the browser's, which is the whole reason for having two witnesses; fixed in
      `athenasip-admin` with a regression test.

### The counters, and a runbook for the call a person has to make (2026-09-23)

Everything the last Milestone 3 item needs except the device and the person.

- [x] `test/interop/media-stats.py`: what rtpengine did with the media, asked of
      rtpengine over its ng protocol, with nothing installed. Lists the calls the engine
      is holding or queries one, and prints per leg the protocol, the crypto suite,
      where the packets came from, how many there were, and the flags that matter -
      `DTLS fingerprint verified`, `ICE`, `rtcp-mux`. `--watch` follows a call live. A
      call with no relayed packet says so in as many words, because that is the failure
      the tool exists to make visible: a declined description travels on untouched and
      the two endpoints reach each other directly, which looks identical from outside.
      Reading these previously meant hand-writing bencode inside the container.
- [x] The ng control port is published, on loopback and only ever on loopback whatever
      the media is published on, because anything that can reach it can redirect
      anybody's media.
- [x] **Its own first bug.** `--watch` wrote nothing at all when its output was a file
      rather than a terminal: Python block-buffers to a pipe and being stopped lost the
      lot. Flushed every time round now, and SIGTERM raises rather than killing it where
      it stands, because watching is the mode whose output is read while it is still
      being written.
- [x] `test/interop/UAT.md`: the runbook for the call nothing can automate, and the
      record to fill in. What to bring up, how to point AthenaPhone at it, what to
      listen for, what to capture, and what each failure means. It says plainly that the
      node logs first lines and not bodies, so the session descriptions have to come off
      the two endpoints - and that gap is now an item of its own.
- [x] `docs/testing.md`: the five layers in one place, what each answers that the one
      before it cannot, and the exact commands. `ACTIVE.md` points at it rather than
      carrying a second copy that drifts.
- [x] Proven by running the browser call three more times through the wrapper, passing
      each time, with the counters read live: 209 packets and climbing across two legs,
      `UDP/TLS/RTP/SAVPF`, `AEAD_AES_256_GCM`, 0 errors.

### The media proved, not only the signalling (2026-09-25)

- [x] The browser call re-run on a node built fresh in the container, and passing: two
      headless Chromium contexts registered, called each way and hung up from each end.
      What is new is that it can no longer pass on silence. The softphone readout gained
      `totalAudioEnergy`, the far end's accumulated `inbound-rtp` energy, which only
      grows and so cannot land in the gap between the fake device's beeps the way an
      instantaneous `audioLevel` can; the spec asserts it above zero at each end. Energy
      was 0.41 and 0.55 one way, 0.47 and 0.57 the other, no packets lost, and every
      remote candidate was the fixture's LAN address. The records are written by the spec
      itself, in `../athenasip-admin/e2e/results`.
- [x] The run needs alternate ports on a machine where AthenaPhone's Asterisk fixture is
      up: both use 5060, 5061, 8088 and 8089 on purpose so one client can be pointed at
      either, so `up.sh`'s own port check stops the run before a container starts.
      `ATHENA_INTEROP_NAME=athenasip-interop-alt` with the port variables it prints is
      the whole of the remedy, and `docs/testing.md` already says so.

---

## Milestone 5 - Batteries included, in progress

Out of order deliberately: a node nobody can install, run as a service or log in to
administer is not a node anybody will adopt, which is principle 3, and the deployment
came first because it is what found several of the bugs below. Milestone 4, the second
node, is still ahead of the rest of this.

### A node on a real host, and a health signal something else can read (2026-09-25)

The first AthenaSIP deployment that is not a test fixture, and the work that had to
exist before it could be one.

- [x] **A CMake install and a systemd unit**, neither of which existed. `cmake --install`
      puts the binary, the unit, the annotated example and the live configuration -
      the last only where there is not one already, because an install that replaced it
      would take the node down at the worst possible moment. `DESTDIR` is honoured by
      hand in the `install(CODE)` that does it, or a packager staging a build would have
      the real `/etc` written. The unit is generated from
      `packaging/athenasip.service.in` so `ExecStart` cannot drift from the prefix, and
      assumes a SIP server is a thing strangers can send bytes to: its own unprivileged
      user, no capabilities at all, two writable directories, `AF_INET` and `AF_INET6`
      only, and memory, task and file limits. `systemd-analyze security` scores it 1.1.
- [x] **A command line.** `main.cpp` ignored its arguments entirely, so
      `athenasip --version` - which the directives treat as the version source of truth
      - silently started a server. `--config PATH`, `--version`, `--help`, and a search
      path of `$ATHENASIP_CONFIG`, `/etc/athenasip/config.yaml`, `~/.athenasip/config.yaml`
      that lets a package and a checkout both work untold. An unknown option stops the
      node rather than being ignored.
- [x] **Deployed to corvus-fi-1** (10.35.1.20, Debian 13, aarch64): Boost 1.89 built
      statically into a private prefix so nothing landed in the shared `/usr/local/lib`,
      every listener bound to the LAN address rather than `0.0.0.0`, Redis on loopback
      with a password and its own database, and the admin console served from the node
      itself. Redis replaced `memory://` because a restart was wiping every realm and
      account - found by the restart in my own verification.
- [x] **A heartbeat that answers "is it alive".** `nodes/<id>/status` was published once
      at startup, fire and forget, which answers "did it start" and is a different
      question. It is now published on an interval (`events.status_interval`, default
      30s), as retained state so a monitor arriving late still learns the answer, and
      with a will so a node that is killed, loses power or loses its network is reported
      down by the broker rather than remembered as healthy.
- [x] **The will was set too late, and deploying it is what found that.** A broker takes
      a will when the session opens and never afterwards; it was being set from `Core`,
      which is built after the bus has already connected, so it was refused every time
      and the node ran with none. `SIGKILL` on the real node left the broker reporting it
      healthy. The status report is now a static function taking its parts, so `main` can
      arm the will before connecting, and the recording bus in the tests refuses a late
      will exactly as a broker does - a double that accepted one at any time is what let
      this pass.
- [x] **SPA mode as a choice.** `http.files.spa`, default true. The routing fallback
      already existed; a file server that invents `index.html` for a missing page hides a
      broken link behind a 200, so it is now something a deployment says rather than
      something it gets.
- [x] `docs/installation.md` and `docs/testing.md`, neither of which existed - the second
      replacing three places that disagreed about how to run the tests.
- [x] T.O.M.S, the site monitoring dashboard, consumes the heartbeat at
      `athenasip/nodes/+/status` on the site broker and has dropped its TCP probe. The
      broker had to change: the node was publishing to the mosquitto on its own host,
      which bridges only `cerbo/#` inbound and was invisible to the monitoring.

### Admin authentication, the datastore half (2026-09-25)

Specified in full in `docs/authentication.md`, which the two decisions below amend. What
is left of it - session issue, the auth routes, roles on the routes, the users routes,
`--add-user` and the OpenAPI document - is in `ACTIVE.md`, in the order it has to happen.

- [x] **Admin authentication, the datastore half** (`dbbf203`, `4790bc0`, `c2cf031`).
      `types::Password`, `types::User` and `types::Session` landed with the contract in
      `f2b1552`; this is both drivers implementing the nine user and session operations
      behind it, written from `docs/authentication.md` rather than from the shape the
      contract happened to have. Thirty-one tests - sixteen against the memory driver and
      fifteen against Redis - all of which failed first. The suite is 645 green with Redis
      and MQTT up, and clean under both sanitizers.

      The two drivers are held to the same statements deliberately, because a login that
      behaves differently on `memory://` and `redis://` is worse than one that only works
      on one of them, and the deployed node runs on `redis://`:

      - create refuses an existing username and update refuses a missing one, which is
        what lets the API answer 409 rather than overwrite somebody, and 404 rather than
        create a user without applying the rules that creating one applies.
      - Usernames are one case-folded namespace. `Tom` and `tom` cannot both exist,
        either spelling logs in, and the spelling it was given is kept for display.
      - Writing a token hash that is already held replaces it. A hash is 32 bytes from a
        CSPRNG and does not collide by accident, so this is how `last_seen_at` moves, and
        why the contract needs no `session_update`.
      - Absolute expiry is the store's, because it is written on the record. Idle expiry
        is not: how long a session survives unused is configuration the store is not
        given, so the caller asks `Session::has_expired`.
      - A read hands back a copy. The memory driver returned the stored record itself,
        which meant changing a user that had been read without writing it back changed
        the roles on a `memory://` node and not on a `redis://` one - and the user record
        is what authorises every request, which is the worst place for two drivers to
        diverge. Realms still return the record itself; a smaller problem, left alone.
      - An already-expired session is refused rather than stored and then withheld, for
        the same reason `nonce_create` refuses an expired nonce. Redis could not store
        one at all, because `SETEX` has no non-positive expiry to give it, so tolerating
        it in memory was a divergence waiting to happen. `expires_at` of zero goes with
        it: zero is what a record written before expiry existed carries, and reading it
        as "never ends" would be the one session nothing can time out.
      - A role this build does not know is carried rather than dropped. Dropping it was
        the first thing the Redis driver did, and it is wrong in a cluster mid-upgrade:
        an older node rewriting a user would silently strip a role a newer node gave
        them. It grants nothing either way, because every authorisation check asks
        whether a specific known role is held, so a string nothing recognises can never
        match one. Validating what may be granted is the API's job, on the way in.
      - Deleting a user revokes its sessions, as deleting an account drops its bindings:
        a live token against a user that no longer exists is a session nobody can
        revoke. Revoking for a user that holds none succeeds, because holding none is
        the state the caller asked for.

      In Redis a session is a key with a TTL of its own absolute expiry, plus a per-user
      index set so revoking every session a user holds never needs `KEYS` - `KEYS` blocks
      the server, and revoking is on the path of disabling somebody. A session that
      reaches its expiry without being logged out leaves its hash in that index:
      harmless, bounded by how often that user logs in, and cleared by the next revoke,
      which drops the index whole rather than a member at a time. The tests leave Redis
      exactly as they found it, no stray keys and no stray index members, which is itself
      a statement about the delete paths.
- [x] **Two decisions written down where they are looked for** (`8f4704a`, `e50fd3e`).
      `docs/plugins.md` says under Versioning that a contract operation may be defaulted
      rather than pure, which is why a datastore written against an earlier contract keeps
      compiling and says which operation it cannot hold - and which way round the risk
      runs: leaving one defaulted is fine, implementing one with the create/update split
      wrong is not. `docs/authentication.md` settles what a session revoke answers: 204
      with no body as every other delete in this API does, 404 for no such user, and 204
      for a user who has never logged in, with the existence check belonging to the
      handler exactly as `_with_realm` does it. Agreed with `athenasip-admin`, which had
      already built it that way.

### Admin authentication, the API half (2026-09-30)

The rest of `docs/authentication.md`, in the six steps `ACTIVE.md` had them in, and with
it the thing the console was blocked on: a fresh node has no users, the configuration
token creates the first, and that user creates the rest. Nine commits, 744 tests.

- [x] **Session issue and lookup** (`555a4ba`). `api::Sessions` is the half of admin
      authentication the datastore contract deliberately does not know about: a login to a
      token, a token back to the user holding it, and a logout. 32 bytes from the CSPRNG,
      hex, and the driver never sees one - only its SHA-256.
      `http.api.session_lifetime` and `http.api.session_idle` joined the schema.

      Three calls it made that everything after inherited. The clock is injectable here
      and stays real in the drivers, because the only thing a driver does with the time is
      prune on an expiry a test chose, while an idle timeout against a real clock is an
      hour of waiting per case. The idle window is rewritten once per tenth of itself
      rather than on every lookup, so an authenticated request stays a read. And a session
      that has gone idle is deleted on the spot rather than left to a store that prunes on
      absolute expiry alone and would otherwise hold it for the rest of a twelve-hour
      lifetime.

- [x] **A session delete stops saying whether the token was real** (`e6a9d83`). A contract
      behaviour change, and the one worth arguing about. `session_delete` reported failure
      for a hash it was not holding, on both drivers, deliberately, with a test each way.
      That is right for a realm, where the caller names a thing by a name that is not a
      secret and 404 is useful. It is wrong for a session, where the caller identifies it
      by a token it presented, so distinguishing "that was live" from "that was never
      live" is a way to ask this node whether a token is real, one guess at a time, on a
      route anybody can reach. `API_VERSION` did not move, because the shape did not
      change - and `docs/plugins.md` now says that the version tracks shape and that shape
      is not the whole contract, because a driver reporting the old answer would compile,
      load, pass the version check and reintroduce the oracle.

- [x] **The three auth routes** (`b89c642`, `8062000`). `/auth/login`, `/auth/logout` and
      `/session`, in `api::AuthAPI`, wired in `main.cpp`. A refused login is one body byte
      for byte for an unknown user, a wrong password and a disabled user, because between
      them the alternatives are a list of who holds an account here; an empty password is
      that same 401 while a missing field is a 400. A logout is 204 whether or not the
      token named a session. A store that cannot be asked is 503 with a fixed message, not
      a 500 carrying the store's own words to an unauthenticated caller.

      `8062000` then fixed what the console caught reading the handler: a wrong
      `old_password` answered 401, which to any client means the credential is dead and
      ends the session, when the bearer was fine and a body field was wrong. 403
      `wrong_password` now.

- [x] **Roles on the routes** (`cdbcaca`). Routes named a scope, a scope belonged to a
      configuration token, so a user who logged in could reach nothing. Now a route names
      roles and both credentials resolve to a set of them. That made the router's
      authorisation step asynchronous, since resolving a session token is a datastore round
      trip: everything a handler needs is copied out of the request first, because the
      request does not outlive the lookup, and `RouteContext::caller` carries who is
      calling.

      An empty role set means any authenticated caller, not a public route; `add_open` is
      how a route says it is open. The old `public_scope` was an empty string, so a route
      that forgot to name its scope was open to the world - now forgetting gives you "must
      be logged in and may do nothing", and two tests catch the difference. No deployment
      lost access: `admin` maps to every role and `client` to `view-cluster-status`, which
      is exactly the two routes it reached before.

- [x] **The users routes** (`7bf137c`). `api::UsersAPI`. Everything needs
      `manage-admin-users` except changing a password, which is declared for any
      authenticated caller and decides for itself, because "it is mine" is not something a
      role can express.

      Nobody locks themselves out of the door they are standing in: a user cannot disable
      itself, demote itself out of `manage-admin-users`, or delete itself. The third was
      not in what was agreed with the console, and is there because a user that deleted
      itself is locked out exactly as thoroughly and a rule that blocks one route to the
      same place is decorative. All three are about self and not about the role, because a
      node with no administrators left is recovered with the configuration token.

      Changing a password ends every session that user held including the caller's own: a
      password is changed because the old one is no longer trusted, and resetting a
      compromised account would otherwise leave whoever compromised it logged in. The
      PBKDF2 iteration count became a constructor parameter rather than configuration,
      because 600000 iterations per created user put eight seconds on the suite and a
      setting whose wrong value is invisible until the database is stolen is not one to
      offer before there is a reason.

- [x] **`athenasip --add-user`** (`d0d105b`). The other half of recovery: a node whose
      passwords are all lost, or whose HTTP listener is unreachable, still has a datastore
      and somebody with shell access. It writes the user and exits, touching no listener,
      no bus and no Core. The password is read from the terminal with the echo off or from
      stdin when piped, and there is deliberately no option that takes one, because a
      command line is readable by every other process on the host.

      "Already there" is found by asking the store rather than by reading the failure
      message: the contract says create refuses an existing username but not what a driver
      calls that, and Redis says "already exists" where the memory driver says
      "user_create failed", so matching on words told an operator the wrong thing on one of
      the two drivers.

- [x] **The OpenAPI document** (`03ebd0f`). `docs/api/openapi.yaml` carries the eight new
      routes and says roles rather than scopes throughout, with an `operationId` on every
      operation because the console generates its client from it. Checked three ways
      rather than read over: it lints clean, every property of `User`, `LoginResponse` and
      `Session` was compared field by field against what a live node emits, and every
      documented status code was walked against that node - which caught a wrong sentence
      of mine about `old_password`, which is required only from a caller that does not
      hold `manage-admin-users`.

Still not solved, and still recorded at the end of `docs/authentication.md`: the admin
listener is plain HTTP, so a login puts a password on the wire; a login endpoint without
rate limiting is a password oracle, and now it is a reachable one; and identity is only
worth having if what each identity did is written down.

### The quickstart, the client's own configuration, and two flows that leaked (2026-09-30)

What landed after the authentication work, in the order the plan had it, plus three bugs
that came in from sibling sessions and one found by watching a live node.

- [x] Client provisioning endpoint: `GET /api/v1/client/config` returning the WSS URL,
      ICE servers and time-limited TURN credentials (coturn shared-secret scheme).
      `http.api.ice_servers`, `turn_shared_secret` and `turn_credential_ttl` are the
      configuration; `types::TurnCredential` is the scheme, and its expected HMAC was
      computed with `openssl dgst` rather than by this code, because a test that derives
      the answer the same way the code does agrees with the code about being wrong - and
      the thing that has to agree is a TURN server that is not in this repository.

      Taken out of turn, above the manual call it sits under, for two reasons: that call
      needs a person with a device and cannot be done from here, and the compose stack now
      runs a coturn that nothing could hand credentials for. It is wired into that stack,
      so the quickstart serves working ICE rather than a relay nobody can reach.

      A `turn:` URL configured with no shared secret is still reported - it is what the
      operator configured, and hiding it would lie about the deployment - but no credential
      is invented for it, and the node warns at startup rather than letting a call fail
      later. `websocket_uri` is absent rather than empty when there is no `wss` listener,
      because a browser handed `ws://` from an https page fails further away than one told
      there is nothing.

      Proven end to end after a real browser found it broken: two Chromium contexts against
      the quickstart stack registered over WSS, called, and got `400 TURN allocate error`
      with `iceTransportPolicy: "relay"`. The cause was the name this code put after the
      colon in the TURN username - `Caller::describe()`, which is prose for our own logs -
      and coturn refuses a username with a space in it, reporting 401 "wrong username" and
      then 400, which at the client is indistinguishable from a bad secret. Filtered at the
      source now, and a credential the live node mints allocates a relay, binds a channel
      and refreshes on both coturn 4.6.2 and 4.18.0.

      Then proven to carry audio, which is the part only a browser can show: two Chromium
      contexts, relay forced, 399 and 402 packets each way with nothing lost and
      `totalAudioEnergy` above 2 at both ends, relayed to rtpengine on the compose network.
      The same browsers with relay *not* forced also carried audio, because having the TURN
      server from `/client/config` let ICE fall back to it when the direct path failed -
      which is the behaviour to want and is better than what was predicted.

- [x] Relay-only media cannot work on a loopback quickstart, which is a limitation to
      document rather than a bug to fix. rtpengine advertises `sip.public_address`, and when
      that is `127.0.0.1` the address coturn is asked to relay to is coturn's own loopback
      rather than rtpengine - and coturn refuses loopback peers anyway without
      `allow-loopback-peers`. A stack whose `ATHENA_PUBLIC_ADDRESS` is this host's LAN
      address has no such problem, which is what `docker/up.sh` picks when it is not told
      otherwise. Found by the admin console session; the fix is a paragraph in
      `docs/quick_start.md` saying which address to run with when the point is to exercise
      TURN. Done: `docs/quick_start.md` has it, `up.sh` prints a note when the
      combination cannot relay, and `ATHENA_RTPENGINE_ADVERTISE` is the way to exercise a
      relay without putting anything on the LAN - TURN needs only the TURN server to reach
      the peer, so the engine advertising its bridge address is enough.

- [x] A UDP flow is reaped when it goes quiet. `sip.flow_idle_timeout`, five minutes by
      default and zero to keep the old behaviour, swept by `Core::_flow_sweep` against the
      injectable clock. Only unreliable flows: a TCP, TLS or WebSocket flow ends when its
      socket does, and sweeping one that is merely quiet would close a registration's path
      home.

      Two bugs behind the one that was written down. `AsyncQueue` had no way to release a
      pending reader, and on the UDP path that reader holds a `shared_ptr` to its channel -
      nothing ever completes a UDP read, so the channel outlived its own close whatever else
      let go of it. And `UDPServer` was the one server that never called `start()` on the
      connections it made, so an inbound UDP flow answered `is_open()` false for its whole
      life and `Channel::close()`, which only tears a connection down if it says it is open,
      skipped it silently. That second one is why the first attempt looked right in the unit
      tests and did nothing on a live node: the flow left the registry and stayed in the
      server's map, and the next datagram from that address was handed to the closed
      connection instead of making a live one.

      `docs/events.md` no longer says a UDP flow is never closed, because it is not: the
      `closed` event fires, and the channels topics balance.

- [x] **The relay is in the automated browser layer's reach.** `test/interop/up.sh
      --rtpengine` brings up coturn alongside rtpengine, serves STUN and TURN through
      `GET /api/v1/client/config` with a secret generated per run, and exports what the spec
      needs in `generated/fixture.env`. `docs/testing.md` has the whole contract.

      Option B of the two written up here, and both sessions preferred it: the interop
      fixture keeps `memory://` and `local://`, which its own template asks for - "a fixture
      that needs them is a fixture that fails for reasons that have nothing to do with SIP" -
      rather than being folded into the quickstart and made to depend on Redis and Mosquitto
      for a media test. It needed no decision from Tom in the end, because it changes nothing
      in the 2026-09-23 decision: the fixture was already this side's to bring up, and coturn
      is one more service in it.

      Two runs, which is forced: no single advertised address serves the direct and relay
      cases without putting the fixture on the LAN. The relay case asserts the pair by the
      local port being inside the configured range rather than by `candidateType`, because
      Chrome reports `prflx` for a relayed local candidate - a general rule agreed with the
      console session, that a test asserts facts about our configuration and never the
      browser's labels.

      **Both phases pass.** `browser.sh` runs direct then relay, rebuilding the fixture
      between them, and the console session's spec asserts the relayed pair by its local port
      being inside the configured range. The record holds ports 22326 and 22312 inside
      coturn's range against a remote address of `172.32.0.30`, which the browsers have no
      route to, with DTLS connected and audio energy at both ends. The relay is in the
      automated layer.

- [x] `docker-compose.yml`: athenasip, redis, mosquitto, rtpengine, coturn, a seeded
      realm and two accounts, WSS on 9443, TLS on 5061, the API on 8080. `docker/up.sh`
      is the one command: it renders the config, brings the stack up, waits for health
      and provisions the realm, the accounts and the first administrator over the API,
      which is the same path an operator uses and a second check that the API works.
      Running it is also what proved it, twice over - a real Digest REGISTER from sipp
      lands a binding in Redis, and the heartbeat is on the broker in the stack.

      The admin console is the one part that is not here: it is built in its own
      repository, so `--console` mounts a build when there is one and nothing is served
      when there is not. A bundle checked into this tree would be a copy that goes stale.

      coturn runs but nothing hands its credentials out. The node has no TURN
      configuration and no `GET /api/v1/client/config` to serve one from - that is the
      Milestone 3 item - so a browser is configured with the shared secret by hand today.
      The secret is in the file the endpoint will read when it exists.

- [x] Command line, the rest of it. `--print-config` prints the effective values as
      YAML - the file, the search path and the defaults resolved - says which file it came
      from, and exits without starting a listener or constructing a driver, so a node that
      cannot reach its datastore can still say which one it was trying to reach. Tokens are
      redacted and their scopes are not. A plugin's own section is copied through rather
      than interpreted, which is what caught the one bug in it: `events.status_interval` is
      a field the server parses, so emitting it from the field and again from the document
      put the key in twice, and there is a test that counts it now.

      `docs/configuration.md` had the search path wrong - it listed
      `~/.athenasip/config.yaml` first and `/usr/etc/athenasip/config.yaml`, neither of
      which is what the code does - and now says the four places in the order they are
      tried and why.
- [x] **Three bugs reported by sibling sessions, each verified here before it was
      believed** (`3dd4c9b`, `cab52da`, `8062000`). The Corvus LoRa Bridge session found
      that `Password::verify` derived to the *stored* hash's length, and PBKDF2 to a
      shorter length is a prefix of the longer output - so a hash truncated to one byte was
      matched by about one password in 256, measured at six of 512 wrong passwords before
      the fix. It also reported that a finished MQTT run could tear down the next; the
      reported cause was the reused client, and the actual mechanism is that `connect()`
      sets `_connected` true *before* joining the previous thread, so a stale completion
      passes its own guard. Each run carries a generation now, which holds whatever order
      the flag is set in.

      The console session found that a wrong `old_password` answered 401, which every
      client reads as "your session is dead". 403 `wrong_password` now, with a code of its
      own so it is distinguishable from the missing-role 403 on the same route.

- [x] **A TURN username carrying our own log prose** (`68bfc90`). The name after the colon
      was `BearerAuth::Caller::describe()` - "a configuration token", spaces and all - and
      coturn refuses a username containing a space, reporting 401 "wrong username" and then
      400 Bad Request, which at the client is indistinguishable from a bad shared secret.
      No browser could allocate a relay and neither session could see why. Found by the
      console session running two real Chromium contexts and then reading coturn's verbose
      log. The name is filtered where the credential is minted rather than trusted from the
      caller. The other hypothesis, that `coturn:latest` was at fault, was ruled out: 4.6.2
      gives the same 400 with the old username and both versions allocate with the new one.

### The first live call, heard - after a DTLS-role bug (2026-09-30)

The call Milestone 3 was named for: a browser on this Mac called an AthenaPhone on a
Blackview A85, through the interop fixture with rtpengine on the media path, and Tom heard
it both ways; the phone then called the browser and he heard that both ways too, with a
BYE from each end. `test/interop/UAT.md` at `47de4a2` carries the record - fixture, versions, counters
from the silent attempts and the working ones, both ends' SDP, and the defects found on
both sides.

- [x] **The call.** Register both ends, call each way, hear audio, hang up from each
      end, read rtpengine's counters. AthenaPhone signalled on TCP, moved off UDP before
      the first attempt on its session's warning that UDP does not survive Docker Desktop's
      NAT ageing on macOS; the fixture ran on 15060/15061/18088/18080 because the phone's
      own Asterisk held the defaults. Registration, inbound INVITE over the flow with the
      `.invalid` Contact never consulted, CallKeep ringing and answering on hardware, the
      phone's DTLS-SRTP and opus all worked on the first build. Nothing was heard.

      The browser half had been re-run on 2026-09-25 to prove media rather than only
      signalling - `totalAudioEnergy` asserted above zero at each end - and was passing.
      It never rang.

- [x] **The DTLS-role bug** (`src/media/rtpengine_media_engine.cpp`, `apply_profile`).
      The WebRTC profile put `DTLS=passive` on every ng command, the answer included.
      rtpengine, as the answerer towards an offerer that said `actpass`, chooses active and
      starts the handshake the moment ICE comes up - seconds before any 200 OK exists when
      a person has to pick up. The answer's `passive` reset that handshake, the SDP told
      the offerer the engine was passive, and neither end started again: ICE `connected`,
      `connectionState` `connecting`, the offerer's leg at 4 packets and 336 bytes in every
      attempt, whichever end was the offerer. The automated browser run answers within
      milliseconds and wins the race, which is why it had passed for a week. Fixed by
      saying `DTLS=passive` only in the offer; the RFC 5763 section 5 test in
      `tests/media/rtpengine_media_engine_test.cpp` was watched failing first.

      Two wrong explanations were written into the record before the right one and are
      kept there: Docker Desktop's NAT, real and irrelevant, recorded on four reproductions
      and no positive control until `browser.sh --direct` passed the moment it was run;
      and unlike legs, ruled out by reproducing the stall browser-to-browser with a
      9-second ring. The AthenaPhone session's observation that the stall followed the
      offerer, and the engine's own per-leg DTLS timeline at log-level 7 read against the
      commands this node sent, are what settled it.

- [x] **The realm profile offered a WebRTC client plain RTP/AVP.** The first attempt was
      refused 488: the callee had said nothing yet, the realm was on `FromTransport`, and
      TCP reads as a desk phone. Worked around for the run with
      `PUT /api/v1/realms/{realm} {"media_profiles":"webrtc"}`, and not fixed: found, and
      carried in `ACTIVE.md` as decision 3. The `+sip.ice` the phone registered with was
      written up that evening as the fix and is not one - RFC 5768 has it mean ICE and
      nothing more, and PJSUA sends it by default from clients that are plain RTP.

      Found on AthenaPhone's side and recorded in the runbook: a verbose-SIP toggle wired
      to nothing, a Contact naming a `.invalid` host on non-WebSocket transports, a
      self-managed ConnectionService that never rings on Android, no `rport`, and a
      default Google STUN server that puts a public address in every offer and holds the
      INVITE for forty seconds of gathering. Its session built a transport-level SIP trace
      during the run, which is where the phone's half of the record came from.

### A forgotten UDP flow is still a way home, and outbound UDP (2026-10-01)

The idle sweep of 2026-09-30 forgets a UDP flow after five quiet minutes, and its commit
said that cost a peer nothing because "a UDP contact is routable, which is what an
in-dialog request falls back to". Neither half held. Outbound UDP was refused outright, so
nothing could be sent to any Contact over UDP; and a phone behind a NAT has a Contact on
its own LAN, so even with outbound UDP the fallback goes nowhere. Five minutes after a UDP
phone last spoke a call to it failed 480, and a call through this node lasting longer than
five minutes could not be hung up from the far end.

- [x] **Outbound UDP** (`Core::channel_connect`, `UDPServer::open_datagram_flow`). A
      datagram to a host this node has never heard from leaves by a listener's own socket,
      so the far end answers to the port it listens on and a NAT in front of it recognises
      the source (RFC 3261 18.1.1, RFC 3581). The listener makes the connection and files
      it, so the first datagram back arrives on the same flow rather than making a second
      reader; Core makes the channel, as it does for TCP. A literal address skips the
      resolver, a name is bounded by `connect_timeout_ms`. Outbound TLS is still refused.
- [x] **A binding whose UDP flow was forgotten is reached at the flow's address**
      (`Proxy::_target_for`). RFC 5626 section 3.1: a UDP flow is the pair of addresses, and
      it does not end because this node stopped keeping a record of it. The Request-URI
      stays the Contact (section 5.3); only the hop is the flow's. Only from the node that
      held the flow - from another node the datagram comes from an address the NAT has
      never seen.
- [x] **The flow token is sealed rather than random** (`src/flow_tokens.h`). It carries the
      flow id under AES-256-GCM with a key made at startup, so a token whose channel has
      gone still says which UDP flow it was, and a BYE in a long call goes back to where the
      caller's INVITE came from. Sealed because the flow id is the far end's address and
      the Record-Route goes to both ends; authenticated because the Route it returns in is
      anybody's to write. Hex, so it is safe in a URI user part and cannot spell an address.
      A token from before a restart or from another node opens to nothing, which leaves the
      Contact, as before.
- [x] **A stateless response goes to `received` and `rport`** (`Proxy::on_stray_response`,
      RFC 3261 16.7 step 1 and 18.2.2, RFC 3581 section 4) through `channel_connect`, so a
      forgotten UDP flow is reopened and a closed connection is opened again as 18.2.2 says.
      Its `rport` was cast straight to 16 bits, so `rport=70000` sent the response to port
      4464; a value that is not a port now drops the response.
- [x] Two things underneath: `UDPServer` handed a datagram to a connection that had closed
      but not yet left its map, and a closing connection's removal erased whatever held its
      key by then, which after a reopen was the new flow. Both fixed.

18 new tests, from RFC 5626 sections 3.1, 5.2 and 5.3 and RFC 3261 18.1.1 and 18.2.2, each
seen failing without the change it tests - the rport one by taking the range check back out. Mutating the holding-node guard to check its test made a different test fail, and
tracing that turned up a race in the first version of the datagram connect - the
resolver's thread against the fixture's fixed settle - so literal addresses no longer go
through the resolver at all. Verified on a
running node with a 20-second idle timeout: a client registered from a private Contact, its
flow forgotten, called, answered, the caller's flow forgotten in turn, and the callee's BYE
delivered to the caller's NAT address. 795 tests.

### Two harness scenarios from RFC 3264, and an open relay found (2026-10-01)

- [x] **`delayed-offer`** (`test/e2e/scenarios/invite_delayed_offer.xml`,
      `uas_delayed_offer.xml`). An INVITE with no body; the callee offers in its 200 and the
      caller answers in the ACK (RFC 3261 13.2.1, RFC 3264 section 5). Asserts the INVITE
      reaches the callee still empty, the offer reaches the caller pointing at the relay,
      and the answer reaches the callee pointing at the relay.
- [x] **`hold-resume`** (`invite_hold.xml`, `uas_hold.xml`). Two re-INVITEs by the route
      set: `sendonly` answered `recvonly`, then `sendrecv` (RFC 3264 8.4, RFC 6337 5.3).
      Asserts the direction arrives at each end and every description names the relay.
      In-dialog requests in these two go to the remote target with the route set (RFC 3261
      12.2.1.1), which the older scenarios do not.

Both passed on their first run, against the builtin relay (10 of 10) and rtpengine (11 of
11), on an image built from the tree that carries the outbound UDP and sealed flow token
work. The node's log shows each re-INVITE, ACK and BYE crossing it.

Writing the trunk scenario is what found the relay: the proxy authenticated nobody, and
forwarded a stranger's request to any domain it did not serve. Tom decided the policy the
same day; it is the next entry.

### The proxy is not an open relay (2026-10-01)

There was no 407 anywhere in the tree. A stranger's INVITE to `sip:+15551234567@198.51.100.99`
was forwarded over UDP with no challenge - over TCP it always had been, and outbound UDP
landing the same day removed the accident that had stopped it on the default transport.
The policy is the 2026-10-01 decision in `ACTIVE.md` and the table in
`docs/authentication.md`.

- [x] **`Proxy::_authorize` and `_authenticate`** (RFC 3261 22.3). A From in a realm this
      node serves is proved, whoever it calls: 407 with Proxy-Authenticate (both
      algorithms, RFC 8760), answered as that same subscriber - credentials for somebody
      else are 403 - or a reliable connection the registrar authenticated a REGISTER for
      it over (`Channel::authenticated_as`). A From elsewhere may call into this node's
      realms and nowhere else. The credentials for this realm are removed before forwarding.
- [x] **The Digest arithmetic is shared** (`src/digest.h`): the registrar's check and
      challenge moved there unchanged and the proxy uses the same code.
- [x] **A BYE would have been challenged.** The dialog table ends a dialog when it sees the
      BYE, which Core does before the proxy runs, so "is this in a dialog we are on" was
      already false by the time it was asked. Core now records it on the request first
      (`SIPMessage::in_known_dialog`). Found by the in-dialog test.
- [x] 17 tests in `tests/proxy_authentication_test.cpp`; the identity match and the
      UDP-is-not-a-connection rule each checked by taking them out and watching their test,
      and only theirs, fail. Existing tests that model a subscriber calling now give that
      subscriber an authenticated connection, or credentials on UDP, as a real one has.
- [x] **The sipp harness answers the 407** in every caller scenario, and `relay-refused`
      is new: 11 of 11 with the builtin relay, 12 of 12 with rtpengine. Because the 407 is
      not optional in those scenarios, every passing run is a real client answering it.

What a client needs to do about this is its own session's work. A browser or AthenaPhone
on TCP that registers over the connection it calls on sees no change; one calling on UDP
answers a 407 on every call. Deployed to `corvus-fi-1` the same day: every transport
registers (`test/interop/smoke.py`), and a stranger's INVITE to an outside number from the
LAN is answered 403.
811 tests.

### RFC 3263, IPv6 config URLs, and Boost.Redis through the node's logger (2026-10-01)

- [x] **RFC 3263 locating** (`src/dns/`). A hand-written DNS client per the dependency
      rule: the RFC 1035 codec with SRV (RFC 2782) and NAPTR (RFC 3403), tested against real
      responses captured from 1.1.1.1 and read by hand; a UDP resolver on the system's
      nameservers with a fresh port and a random id per query (RFC 5452), a retry round and
      a five-second try as resolv.conf(5) has them, TCP when truncated (RFC 7766), and a TTL
      cache (RFC 1035 7.4, RFC 2308); and `SipLocator`, sections 4.1 and 4.2 - transport
      parameter, numeric host and explicit port each skipping what they make unnecessary,
      NAPTR by order and preference with only `SIPS+D2T` for a sips URI, SRV for each
      transport when there is no NAPTR, A and AAAA last, RFC 2782's weighted choice, "." as
      no service, and nothing under `.invalid` ever asked (RFC 6761). The proxy locates a
      target naming a host and tries each hop in turn, so one that refuses a connection is
      passed over (4.3).
- [x] Three things found by running it rather than by the tests: a resolver that asked each
      nameserver once lost a whole transport to one dropped datagram on a machine with one
      server; a two-second try gave up on SRV answers that 8.8.8.8 takes two and a half to
      fetch cold; and a self-clearing recursive lambda in the first locator was a
      use-after-free that crashed the suite. Each is fixed and the first two have tests. The
      DNS and proxy tests ran clean under ASan and UBSan afterwards; GoogleTest from Homebrew
      needs `detect_container_overflow=0` for that, now in `docs/testing.md`.
- [x] `ATHENA_TEST_DNS=1` runs the locator against the real DNS on sip2sip.info, which
      publishes the full NAPTR, SRV and A set.
- [x] **IPv6 literals in a config URL** (`types::URL`, RFC 3986 3.2.2): a bracketed host is
      read whole and kept without its brackets, which go back on in `to_string`; an
      unbracketed one is refused rather than guessed at.
- [x] **Boost.Redis logs through the node's logger.** Its connection chatter reached stderr
      with its own prefix and no level - into the output of `athenasip --add-user`. It goes
      to the datastore's scoped logger now: lifecycle at debug, warnings and errors as such.

- [x] **4.3's failover, the rest of it.** A 503 or a transaction timeout from a hop sends
      the same target to the next hop DNS listed, as a fresh branch; a 486, a 404 or any
      other answer still ends that target, because another server for the domain would
      give it too. When every hop fails the caller gets a 500, not the 503 (RFC 3261 16.7).

870 tests.

### Live calls, the media engine and /metrics on the admin API; a relay that waited (2026-10-01)

- [x] **`GET /api/v1/calls`, `/api/v1/calls/{call}`, `/api/v1/media`, `/metrics`**
      (`src/api/calls_api.cpp`), shaped with the admin console session before they were
      written. A call is its id, state, times and participants, and the media its engine
      reports: per end per stream, cumulative, `packets_in`/`bytes_in` from the end and
      `packets_out`/`bytes_out` to it, so one-way audio shows as one direction standing
      still. A direction the engine does not report is absent, not zero; rtpengine's query
      documents only what each end sent. `participant` is null, because a relay knows an end
      by its source address and behind a NAT that is not one anybody described. A Call-ID
      holding `/` routes as one segment. `/metrics` is Prometheus text: calls, dialogs,
      channels by transport, transactions, and media packets relayed when the engine can
      count. Everything is read from Core's strand by snapshot and answered on the API's
      executor. `docs/api/openapi.yaml` describes all four, and a new test parses it and
      checks no operationId is used twice.
- [x] **The builtin relay counts** what arrives from and goes to each end, and the relay
      keeps a whole-life total; `MediaEngine::packets_relayed` reports it, optional in the
      contract. The rtpengine driver reads each stream's `stats` into the same shape.
- [x] **The harness now asserts the builtin relay carried media**, from `/metrics` before
      and after the media scenario - the check only the rtpengine run could make before.
- [x] **And that check failed at once, because it had not.** The builtin relay learned each
      end from the first packet it sent and forwarded nothing until both had, so a leg that
      only listens - the harness's echoing callee, a muted phone with silence suppression,
      an IVR, a recorder - got no media at all, and the media scenario had been passing on
      a call that carried nothing. Each end now starts at the address its description gave
      (RFC 8866 5.7, 5.14) and moves to where its packets come from once heard (symmetric
      latching, for NAT); once two ends are heard, an end that was only ever described is
      dropped. Unit tests for the listen-only leg and for following an end behind a NAT
      were watched failing first.

### A behaviour section, server-wide and per realm (2026-10-01)

- [x] **`behaviour:` in the config and on every realm**, with `media_anchor` and
      `media_profile`. The config holds the server's default (`types::MediaPolicy`); a
      realm's `types::Behaviour` holds only what it chose, and `Behaviour::over` gives
      what that comes to, so changing the file changes every realm that has not chosen
      otherwise. The admin API returns both, `behaviour` and `behaviour_effective`; null
      in an update goes back to inheriting. The Redis driver writes only what a realm
      chose. Plugin contract version 8.
- [x] **The shipped default is anchor on, profile `mirror`**, the decision of
      2026-10-01: a callee is offered what the caller offered, and anchoring is the one
      recorded deviation from RFC 3261 16.6. Kamailio's transport rule, which was the
      default, is now the `transport` value.
- [x] **Nothing unreadable is guessed at.** An unknown profile is an error at startup and
      a 400 from the API that changes nothing; it had been read as the default. The old
      top-level `media_anchor` and `media_profiles` are a 400 saying where they went.
- [x] `docs/configuration.md`, `config/config.example.yaml`, the OpenAPI realm schemas
      and the interop README describe the section; the admin session has the shape.

### An account says what its endpoint is (2026-10-02)

- [x] **`behaviour.media_profile` on an account**, step 3 of the 2026-10-01 decision and
      Asterisk's `webrtc=yes`. The proxy reads it where it already has the callee's
      account in hand and keeps it on the leg (`Call::Participant::account_profile`), so
      the first offer towards an endpoint nothing else can read - AthenaPhone on TCP - is
      produced for what the operator says it is. What a leg has said in its own offer or
      answer still outranks it; the realm and the server default decide only where it is
      empty. The admin API takes and returns it in the account's own `behaviour` section,
      refusing any other setting or value with a 400 that changes nothing. The Redis
      driver stores it only when chosen. Plugin contract version 9.

### Every branch of a serial fork is anchored (2026-10-02)

- [x] **A failed branch no longer ends the call.** The dialog tracker treated any final
      failure as the end of the attempt, which is right for a UA and wrong for a forking
      proxy: RFC 3261 16.7 ends the attempt with the final response this node sends
      upstream. So the first binding refusing closed the call record and released its
      media, and every later branch - a second binding, an RFC 3263 failover - went out
      with its description unanchored and came back the same. `Dialogs::observe_branch_failure`
      now ends only the failed branch's early dialog, putting the attempt back when it was
      the only one, and `_send_best` ends the attempt when the answer goes upstream. Once
      the caller has had its answer (a CANCEL's 487, an earlier 2xx) a branch's failure
      is observed as before. Found while building the re-offer below, and positively
      controlled: a 486, with or without a To tag, closed the call the same way.

### One re-offer on 488, and the operator told (2026-10-02)

- [x] **Step 4 of the 2026-10-01 decision.** A 488 to an offer the engine produced for a
      leg that had not described itself is answered by offering the same target the
      other profile, once (`Proxy::_reoffer`), as a Kamailio failure route would. Under
      `mirror` the other profile is the other of what the caller offered, which is the
      browser calling a desk phone. A description passed through untouched - no engine,
      anchoring off, or the engine declining - is the caller's, and its 488 goes to the
      caller. A 488 to the re-offer is the answer.
- [x] **`GET /api/v1/media/reoffers`** (`media::Reoffers`, per node, bounded) lists each
      account that needed it, what it refused, what it took, and the
      `suggested_media_profile` that would save it the round trip. Nothing sets it.

### Registered clients qualified with OPTIONS (2026-10-02)

- [x] **Step 5 of the 2026-10-01 decision.** `behaviour.qualify_interval`, server-wide and
      per realm, is the seconds between OPTIONS to each registered client (Asterisk's
      qualify, Kamailio's nathelper ping); 0, the shipped default, sends none, since RFC
      3261 does not ask for it. `Qualifier` probes down the flow the client registered on,
      once at registration and then on the interval, in one conversation per binding
      (same Call-ID, CSeq counting up), and stops when the binding is removed, expires or
      its flow closes. Any final answer counts as there; silence is counted and the
      probing goes on. A 200 carrying a session description (RFC 3261 11.2) is the
      client's own word on its media, and the proxy ranks it with what a leg says in a
      call: above the account, the realm and the transport. `GET /api/v1/qualify` lists
      the probed clients per node.

### The behaviour section documented for an operator (2026-10-02)

- [x] **`docs/behaviour.md`**, which Tom asked for: the three levels, each setting, the
      precedence a leg's profile is decided by, the 488 re-offer and the qualify probe,
      and "configure it like" a plain proxy, Kamailio/OpenSIPS with rtpengine, Asterisk
      and FreeSWITCH, with the defaults each starts from. Linked from the README and
      `docs/configuration.md`.

### The caller's account for a delayed offer, and the behaviour profiles finished (2026-10-02)

- [x] **An INVITE with no description reads the caller's account** (`_read_caller_profile`),
      because RFC 3264 section 5 has the callee offer in its 200 and that offer is
      produced for a caller that has said nothing yet. Only that INVITE pays for the read.
      With it, all five steps of the 2026-10-01 behaviour-profile decision are in.

### The node list fed from the discovery bus (2026-10-02)

- [x] **Each node's status says where it listens** (`transports`, from
      `Config::advertised_transports`, which the node list now shares), and every node
      subscribes to `nodes/+/status` into a `NodeDirectory`. `GET /api/v1/nodes` lists
      this node from its own configuration and every other as it last described itself,
      with its status, and `stale` once its report is three status intervals old. The
      first item of Milestone 4's client failover.
- [x] **`GET /api/v1/client/config` carries the usable nodes** - this one, then every other
      that is up and current - and their `wss` URIs in that order as `websocket_uris`.
- [x] **A `UDP/TLS/RTP/SAVPF` m-line is WebRTC without a fingerprint** (RFC 5764 section 8).
      It had been read as SDES-SRTP, which a capability description in an OPTIONS 200,
      with no DTLS session to fingerprint, would have hit. Found answering AthenaPhone's
      question about what such a description has to carry.

### Contact rewriting, configurable and off (2026-10-02)

- [x] **`behaviour.rewrite_contact`**, server-wide and per realm, off by default - Tom's
      call on 2026-10-02 for the judgement the NAT item left open. On, each Contact in a
      forwarded request or response is rewritten to the address and port the message came
      from (Asterisk's `rewrite_contact`, Kamailio's `fix_nated_contact`), keeping the
      user part and parameters; WebSocket clients are never rewritten. Decided where the
      realm is in hand and kept on the call for the requests inside it. Plugin contract
      version 11.

### RFC 5626 outbound: keep-alives and the registrar (2026-10-02)

- [x] **A double CRLF on TCP or TLS is answered with a CRLF** (section 4.4.1). It had been
      swallowed, so a client waiting for its pong would decide the flow had failed.
- [x] **A STUN Binding request on the SIP UDP port is answered** with XOR-MAPPED-ADDRESS
      (section 4.4.2, RFC 5389), by `src/stun.cpp`, written by hand and only that.
- [x] **The registrar honours outbound** (section 6): with `Supported: outbound`, a
      contact's `+sip.instance` and `reg-id` are the binding's identity, kept on
      `Location` (plugin contract version 12); the same pair from a new flow replaces the
      old binding, a second reg-id is a second flow, expires=0 removes by the pair, the
      200 says `Require: outbound` and lists both parameters, and a first hop in `Path`
      without `ob` is answered 439. No `Flow-Timer`: the only value this node could give is
      longer than most NATs keep a UDP mapping.
- [x] **The proxy's half of RFC 5626 section 5.3**: an outbound client's flows are one target,
      most recently registered first, with the rest kept back (`Proxy::_add_targets`); a 408
      or 430 moves to the next flow, any other final answer is the client's and its other
      flows are dropped, a flow that has gone is that flow failing and never a reason to
      try the Contact, and a 430 is never passed to the caller (480 instead).

### The node's address as each peer sees it (2026-10-02)

- [x] **`sip.localnet` and a `public_port` per listener.** `Core::advertised_for(channel)`
      decides what the node calls itself on a flow: a peer inside `localnet`, or any peer
      when no public address is set, is given the local address and port, with a
      wildcard bind resolved to the interface that reaches that peer; everyone else the
      public address and the forwarded port. Via, both Record-Route values (each facing
      its own end), Service-Route, the qualify probe and the node list all use it, and
      every public name is recognised as this node's own in a Route.
- [x] **And for media**: `media::Flags::address` (plugin contract version 13) carries the
      local address for a leg inside `sip.localnet`, and the builtin relay writes it in
      `c=`, `o=` and `a=rtcp` in place of its one public address. rtpengine picks its own.

### The cluster certificate authority, made by the node (2026-10-02)

- [x] **`athenasip --ca-init` and `--ca-node ID [--san NAME]... [--replace]`**
      (`src/cluster_ca.cpp`, OpenSSL's API with no new dependency), flags rather than the
      plan's subcommands to match the command line there is. EC P-256 keys written 0600 with
      O_EXCL; a CA:TRUE pathlen:0 authority for ten years that is never replaced; node
      certificates for two years, CA:FALSE, server and client authentication, the node id
      and every `--san` as DNS or IP names. Checked independently with `openssl verify`
      and a mutual TLS handshake between two issued nodes. `docs/certificates.md`.

### The inter-node listener and outbound TLS (2026-10-02)

- [x] **The TLS listener survives a bad handshake.** It handshook synchronously on the
      accept thread and returned before re-arming the accept on a failure, so one failed
      handshake stopped TLS until a restart and one silent connection held up every
      other. Found by the cluster tests hanging; reproduced on the client-facing listener
      and fixed there: an asynchronous handshake with a ten-second deadline.
- [x] **`cluster:`** starts a second TLS listener that requires a certificate from the
      cluster CA (`TLSServer::require_peer_certificates`); the verified peer's common name
      is `Channel::peer_node`. **Outbound TLS** (`Core::cluster_tls_set`, `_secure_flow`)
      shows this node's certificate and checks the peer's against the CA and the dialled
      address; without the cluster's certificates `tls` is still refused. Checked against
      the real binary with `openssl s_client`: a cluster certificate is let in and named,
      none is "certificate required", another authority's is "unknown ca".
- [x] **A peer that resets before its connection is made no longer stops the node.** The
      TCP, TLS and WebSocket connections read the peer's address with the throwing
      `remote_endpoint()`, inside the listener's handler where nothing caught it, so a
      reset at the wrong moment terminated the process. Found as an abort in the cluster
      TLS test; reproduced deterministically with a socket that has no peer.

### The words, the realm delete and the rate limits (2026-10-03)

Tom's answers to the questions that were waiting on him, and what they led to.

- [x] **`status_interval` in the node status**, in seconds, in the will too, so a monitor
      derives staleness from the node. T.O.M.S reads it.
- [x] **Deleting a realm deletes what is in it.** `Datastore::realm_delete` takes the
      realm's subscribers and their bindings, in both drivers, and the contract says every
      driver must. In Redis the subscribers go first and the realm record last, so a
      delete that fails part way can be run again. Live calls and open connections are
      left alone.
- [x] **Every admin API route is rate limited** (`src/api/rate_limiter.h`): token buckets,
      per node, in memory. Open routes, unknown endpoints and credentials that do not
      resolve by source address (30, then 30 a minute); the login on top by source address
      (10, then 5 a minute) and by username (5, then 1 a minute), counting every attempt;
      a signed-in caller by session (60, then 300 a minute). 429 with Retry-After. The
      limits are not configurable yet and static files are not limited.
- [x] **One word for one thing, and `docs/glossary.md` to say which.** A subscriber belongs
      to a realm; a user signs in and manages the cluster; account names nothing. The API
      resource is `/realms/{realm}/subscribers`, the event topic `subscribers/<uri>/status`,
      the type `types::Subscriber`, the datastore operations `subscriber_*` (contract
      version 14) and the Redis keys `athena:subscriber:*`. What reads the event bus is a
      consumer. A Redis store written before the rename is recreated, not migrated.
- [x] The sipp harness passes in full on the renamed tree: 12 of 12 at `8e3ba0e`.
- [x] **No re-offer in a profile the engine cannot make.** sipp as 1001 (RTP/AVP) to
      AthenaPhone through the builtin relay on `corvus-fi-1`: the phone refused with 488,
      the node "offered the other profile", and what it sent was the refused offer byte
      for byte, which the phone answered with 482. `MediaEngine::produces(Profile)`
      (contract version 15, defaulted to yes) is what the proxy now asks first; builtin
      says plain RTP only. The 488 goes to the caller, and the node warns when a
      subscriber's profile asks for what the engine cannot produce. The phone's 180 before
      its 488 and its 482 were AthenaPhone's own, and that session has them.

### The second node, begun (2026-10-03)

- [x] **A request from a cluster peer is not challenged.** `Proxy::_authorize` lets through
      what arrives on a channel whose certificate the cluster CA signed
      (`Channel::peer_node`): the peer challenged the caller or took the call from its own
      subscriber, and a second challenge would go to a caller with no way to answer through
      this node. Trust is the certificate and nothing in the message. Path is left as RFC
      3327 has it, accepted from anybody on a REGISTER that authenticated: it only routes
      calls to the subscriber that registered, and an edge proxy in front of the cluster
      that is not a node has to be able to add one.
- [x] **A node says where its peers reach it.** `cluster` in the node status carries the
      inter-node listener's address and port, from `cluster.advertise`, or the bound
      address, or `sip.public_address` where the listener is bound to every address.
      `NodeDirectory::find` reads it back, and `GET /api/v1/nodes` lists it for an
      administrator and not for a client.
- [x] **A call for a flow another node holds goes to that node.** `Proxy::_add_targets`
      turns a binding whose flow is held elsewhere into one target per peer: the request as
      it arrived, address of record and all, to the peer's inter-node listener. No Route
      header, which the plan first named: the next hop is this node's to choose and the
      Request-URI is already what the peer has to look up. The receiving node delivers to
      its own flows and never forwards a peer's request on. A peer that is down, stale or
      unknown is not forwarded to, and the binding is treated as a single node treats a
      flow it has no channel for. `Call::media_elsewhere` keeps the second node's engine
      out of a call the first anchored, and `Core::advertised_for` names the inter-node
      listener to a peer. Unit-tested only; two real nodes are the next item.
- [x] **A call crosses two real nodes.** `docker-compose.cluster.yml` and
      `test/e2e/cluster.sh`: two nodes on one Redis and one Mosquitto, certificates from
      the node's own `--ca-init` and `--ca-node`, the administrator from `--add-user`. A
      control call with both ends on node A, then Bob held by node B called through node
      A, the same the other way, and a media call: 5 of 5 on the first run. The node logs
      say what the result cannot: each node opened a mutual-TLS flow to the other and
      named it from its certificate, the INVITE reached node B still addressed to
      `sip:bob@example.com` and was delivered to Bob's Contact, the ACK and the BYE
      crossed the same way, node B challenged nothing a peer sent, and 101 packets went
      through node A's relay and none through node B's. Over UDP only, and only these
      scenarios.
- [x] **A call reaches a connection another node holds.** `test/e2e/flow_callee.py`
      registers over TCP or a WebSocket, keeps the connection and answers one call on it,
      which sipp cannot do; `cluster.sh` runs it on the caller's node as a control and then
      held by the other node, for both transports: 4 of 4. The WebSocket callee's Contact
      is a `.invalid` name, so the INVITE, the ACK and the BYE can only have arrived down
      the flow node B holds. The image for it is `python:3-alpine`, standard library only.
- [x] **A browser called AthenaPhone on `corvus-fi-1` and was heard both ways.** The
      console softphone as 1001 to the A85 over TLS: one INVITE, the browser's own
      `UDP/TLS/RTP/SAVPF` offer, no 488, ICE and DTLS up, media direct on the LAN, 0 lost.
      The first call was silent one way because the Mac's default microphone was a
      loopback device. `test/interop/UAT.md` at `47de4a2` has both calls and what it took to make them.

### HTTPS on the admin listener and configurable rate limits (2026-10-03)

- [x] **`http.tls`**: an HTTPS listener beside the plain one, serving the same chain, with
      the `tls` section's certificate unless it names its own (`AdminAPI::tls_enable`).
      The handshake is asynchronous with a ten-second deadline, as the SIP TLS listener's
      is. Plain HTTP stays (Tom). A node asked for HTTPS with no certificate does not start.
- [x] **`http.api.rate_limits`**: `open`, `login_source`, `login_user` and `session`, each
      with `burst` and `per_minute`, zero for off. The defaults are what shipped on
      2026-10-03.
- [x] **`websocket.secure_port`**: a wss listener beside a plain ws one, advertised with
      it. HTTPS on the console was not enough on its own: a page served over HTTPS may open
      no insecure WebSocket, and `corvus-fi-1` had only ws.

### Call records, and the rest of the scenarios across two nodes (2026-10-03)

- [x] **Call records.** One per call, written by the node the caller reached
      (`Call::node`); a node a peer forwarded the call to (`Call::from_node`, from
      `Dialog::from_node`) carries it and writes nothing, so two nodes never overwrite one
      record with half of it each. The record names the node that held the callee, from
      the channel the answer arrived on, and the engine that anchored the media.
      `GET /api/v1/call-records`, newest first, read from the datastore. Kept for
      `calls.history_retention`, thirty days: Redis expires an ended record, the memory
      driver prunes when it adds one. The record is complete when the call ends; while it
      is up the callee's node and the engine may lag the state by one write.
- [x] **Cancel, busy and the timeout across two nodes**, in `cluster.sh`, with the TCP and
      WebSocket callees: 9 of 11. Delayed offer and hold are in the script and failed, on
      the scenarios' own check of the relay's address against the single-node harness's
      subnet; the two-node harness now uses that subnet, and the re-run is in the plan.
- [x] **`athenasip --check`** (`src/cli_check.cpp`): the datastore, the event bus, the
      media engine, each certificate a listener asks for, and a mutual-TLS handshake with
      every peer that says it is up, tried one at a time and reported one line each. Exit
      0 or 1. A flag rather than the subcommand the plan first named, as the CA commands
      are. Nothing is started, so it is safe beside a serving node.
- [x] **The end of a call reaching either node releases the media.** Already true and now
      tested: every node a call passed through calls `MediaEngine::release` when its dialog
      ends, anchoring node or not, so with one rtpengine between the nodes whichever sees
      the BYE first gives the ports back, and the call record carries the engine. With the
      builtin relay the media is in the anchoring node's own process, and Record-Route
      keeps that node on the path of every BYE. What is not built is more than one engine:
      a pool, and knowing which instance holds a call, is the item under Milestone 3.

### Video through the builtin relay (2026-10-03)

- [x] **A declined stream stays declined.** The builtin relay gave every m-line a relay
      port, including one answered with port zero, so a caller offering audio and video to
      a phone that declined the video was told the video had been accepted. RFC 3264
      sections 5.1, 6 and 8.2: a port of zero keeps its place and gets nothing, and what
      the stream held goes back. Found by writing the video tests from the RFC
      (`tests/media/builtin_media_video_test.cpp`); the other three passed as they were:
      each stream gets a relay port of its own, video crosses both ways beside the audio
      without leaking into it, and adding video to a call leaves its audio port alone.
- [x] **A video call, browser to AthenaPhone, seen and heard both ways.** The console
      softphone at `https://10.35.1.20:8443` over `wss`, no tunnel, to the phone on TLS:
      VP8 and opus, bundled, direct on the LAN, 464 frames decoded in forty seconds, and a
      call record written. `test/interop/UAT.md` at `47de4a2`. Through the builtin relay's pass-through
      only: video through rtpengine, and plain-RTP video end to end, have not been run.

### After 0.8.0 (2026-10-03)

- [x] **The "cannot produce" warning only when something had to be made**, and naming the
      subscriber rather than the Contact the copy went to. Every browser call to
      AthenaPhone on `corvus-fi-1` logged it for an offer that was already WebRTC.
- [x] **`athenasip --reset-password NAME`**: a new password for a user that exists, from
      the host, and every session it held ended. On 2026-10-03 the only way to reset an
      administrator's password was to edit Redis by hand.
- [x] **The node's media in its status**: the engine, its capabilities and the profiles
      it can produce, `null` with none. The last of discovery.
- [x] **`log.level` and `log.format`.** JSON lines with `at`, `level`, `scope` and
      `message`, the scope taken off the front of the message where a scoped logger put
      it. The node logged everything as text, always, with no way to say otherwise.
- [x] **`media.builtin.public_address` may be a name**, resolved at start and every
      minute after, the address written into descriptions. A node behind NAT on a dynamic
      address (corvus-gbni-1, as `macnessa.athenasip.org`, kept current by manannan) has
      its media follow the name. A name that resolves to nothing is declined, never written.
- [x] **rtpengine's advertised address follows sip.localnet and may be a name.** A leg inside
      `sip.localnet` is given the local address and every other leg `media_address`, resolved
      at start and every minute if it is a name (`src/media/public_address.h`, shared with the
      builtin relay). It had told every leg one fixed address, which a LAN phone reaches only
      through a hairpinning router and which goes stale on a dynamic site address.
- [x] **Video through rtpengine, automated, through a named endpoint behind NAT.**
      corvus-gbni-1 as `macnessa.athenasip.org`, native rtpengine: a headless browser with a
      fake camera called AthenaPhone, 167 frames decoded in the browser and the pattern
      moving on the phone, the browser's media through the public address and a forwarded
      port. `test/interop/UAT.md` at `47de4a2`.
- [x] **RFC 6026: the INVITE transactions' Accepted state.** A 2xx moves both INVITE
      transactions to Accepted for 64*T1 rather than ending them, so a callee's 2xx
      retransmissions still match: the client transaction passes each to the proxy, which
      sends the caller the answer it already sent, through the server transaction, on the
      connection the INVITE came in on. Before, a retransmitted 2xx arrived as a stray, was
      routed by the caller's Via, and for a WebSocket caller - whose Via names a `.invalid`
      host - was dropped. Found through macnessa.athenasip.org on 2026-10-04.
- [x] **Removing a binding that is not there is not logged as an error** (RFC 3261 10.3
      step 7). The store still says there was nothing to remove; Core logs it at debug.
- [x] **A TLS peer closing without close_notify is logged as a disconnect**, not an ERROR.
      Every browser hang-up through macnessa's secure WebSocket logged one.

### The documentation describes the code (2026-10-04)

- [x] **Documentation and comments rationalised.** Comments state intent, invariants and
      RFC references in a line or two, with no history. The documents were checked against
      the source and corrected where they had drifted: listener keys are `enable`, an
      enabled listener needs a `port`, timers G and H are INVITE server timers, the
      `cluster` and `websocket` sections and a dozen keys were undocumented, and the guides
      still described configured API tokens. `docs/design.md` is folded into
      `docs/architecture.md`; `docs/versions/` (MySQL, PostgreSQL, NSQ and NATS goals that
      never shipped) is gone; `test/interop/UAT.md` is the procedure only, its run records
      left at `47de4a2`.
- [x] **`sip.allow_unencrypted` does what it says.** It was read and never used. When
      false, an enabled `udp`, `tcp` or plain `websocket` listener (a plain `port` beside
      `secure_port` included) is a configuration error, and `Core::channel_connect` dials
      only TLS.
- [x] **The example configuration shows the `cluster` section**, off, with the files
      `--ca-init` and `--ca-node` make.
- [x] **Every Lua log function takes the engine as its upvalue.** `debug`, `warn` and
      `error` were given the logger, which `get_script_engine` reads as the engine. Not
      compiled, since scripting is parked; the example scripts under `config/` are removed.
- [x] **Tests renamed** where the name described configured tokens and scopes rather than
      users and roles.

### Push notifications, RFC 8599 (2026-10-04)

Tom asked that a call can wake a registered client that is asleep, after a phone was
missed because its connection had silently died.

- [x] **The `push` plugin kind** (`src/push/push_service.h`), one driver per push service
      named by its `pn-provider`, with what it accepts, its Feature-Caps indicators and
      `send`. Contract version 16, because a binding now records whether it is pushed.
- [x] **The registrar** answers a REGISTER asking for push with
      `Feature-Caps: *;+sip.pns=...` (plus `+sip.vapid` and `+sip.pnsreg` where they
      apply), 555 for a service this node does not run or a Contact missing what it needs,
      and 423 for a binding too brief to be woken in time; a query (no `pn-prid`) is
      answered for the service named or all of them; a `+sip.pns` already in the request
      leaves push to the proxy that put it there.
- [x] **The proxy's push bucket.** A request for a new dialog, or a standalone one, to a
      push binding pushes and waits until the client registers again, found by the
      registrar telling the proxy or by polling the store every 250 ms (a cluster), then
      goes down the new flow. A failed push or `push.timeout` is a 480 for that target and
      the fork moves on; a CANCEL ends the wait. Any node pushes, so a request from a peer
      is not pushed twice.
- [x] **Refresh pushes** (5.5): `push.refresh` seconds before a push binding expires,
      unless it was refreshed meanwhile here or elsewhere.
- [x] **`fcm://`** (OAuth 2.0 service-account token, cached, FCM HTTP v1 high-priority data
      message) and **`webpush://`** (RFC 8030 with no payload, VAPID per RFC 8292), on a
      small HTTPS client of our own. Tested against local HTTPS servers; not yet against
      the real services.
- [x] **The `pn-*` parameters stay private** (section 13): stripped from
      `/api/v1/registrations`, the qualify listing, the subscriber-status event and the
      log.

### Milestone 4: client failover and the second node (2026-10-04)

- [x] **The two-node harness in full**: 15 of 15 at `67a4b75`, the call records check
      included, after Docker came back.
- [x] **The chaos test** (`failover` in `test/e2e/cluster.sh`, last because it kills node A):
      Bob registers through node A, node A is killed, Bob registers again through node B,
      and a new call through node B reaches him. 11 seconds from the kill on the first run,
      against a bound of one minute.
- [x] **`GET /api/v1/client/config?realm=`** says what the realm expects of a client: the
      lifetime the registrar grants and the shortest it takes, how many RFC 5626 flows to
      keep (one per node, up to two), and the push services with the VAPID key and the
      shortest registration push takes.
- [x] **`AthenaSIP-Alternate-Server`** in the 2xx to REGISTER: the other nodes that are up,
      on the same transport, in Contact grammar with `expires`; only over TLS or WSS, and
      only to a client that sent `Supported: athenasip-failover`.
- [x] **The image builds in an 8 GB Docker VM again**: one compiler per 1.5 GB of available
      memory, and the cluster harness builds the node image once rather than twice at once.
- [x] **A REGISTER for a domain not served here is forwarded** (RFC 3261 10.3 step 1, Tom
      2026-10-04: configurable, default to subscribers, UDP as well). `sip.forward_register`
      is `subscribers` or `never` (403). A subscriber is one registered over a reliable
      connection, or one answering a 407 offered for each of this node's realms, since the
      From is the foreign address of record. The forwarded REGISTER carries no
      Record-Route; it carries a Path naming this node, with the flow token and `ob` at the
      first hop (RFC 3327 5.2, RFC 5626 5.1), when the client supports Path or asks for
      outbound. A request routed back through a Path or Record-Route this node sealed is
      admitted and goes down the flow. A REGISTER whose address of record is in one of this
      node's realms is registered here, whatever its Request-URI, as before.
- [x] **The node's own address, from STUN and peers** (Tom, 2026-10-04: the `stun:` servers in
      `http.api.ice_servers` and no others). A node asks them from its SIP UDP socket every
      five minutes and reports `discovered` in its status and in `--check`, which fails when
      `sip.public_address` disagrees. Every node probes every other node's discovered address
      with an OPTIONS and publishes the ones that answer as `reaches`; a node whose address a
      peer reached advertises it (`Config::public_address()`), and one nobody reached is
      never advertised. `sip.public_address` always wins.
- [x] **A node answers an OPTIONS for itself** (RFC 3261 11.2): its own, public or
      discovered address, or a realm with no user, with Allow, Accept and Supported; and an
      OPTIONS with no hops left (16.3 step 3) instead of 483. Before this a monitor's OPTIONS
      to the node was refused or forwarded.
- [x] **`apns://`** (Tom, 2026-10-04: nghttp2 rather than a hand-rolled HTTP/2 client). Token
      authentication with the team's P-256 key, one HTTP/2 connection to Apple kept open, a
      PushKit push for a `.voip` topic and a background push (priority 5) for any other. A
      VoIP binding is never pushed to refresh: iOS 13 stops PushKit delivery to an app that
      takes a VoIP push without ringing (`PushService::refreshes()`, contract version 17).
      Tested against a local nghttp2 server; not yet against Apple.
- [x] **The rest of the node's own address.** A public address a peer node stamps as
      `received` on this node's Via (RFC 3581) is a finding when STUN has none; a private
      one is ignored. Each node tries every peer's inter-node listener every ten minutes
      (`cluster_probes`); a node a peer tried and none reached marks itself
      `"reachable": false` and is not forwarded to, and recovers when one gets through.
      `--check` reports what peers found and fails on a listener none reaches.

### Milestone 5 (2026-10-05)

- [x] **`GET /api/v1/events`**: `nodes/#`, `subscribers/#` and `calls/#` as Server-Sent
      Events, one event per message named by its topic, a keep-alive comment every fifteen
      seconds, `view-cluster-status`, at most 32 streams a node (503 past that). The HTTP
      session can now hold a response open: a route asks for it with
      `RouteContext::stream`.
- [x] **Redis listings in two round trips**: realms, users, subscribers, bindings and calls
      are read with SMEMBERS and one MGET instead of a GET per member in sequence; a member
      whose record has expired is still dropped from its index.
- [x] **Plugins as shared libraries.** `plugins.path` (a directory or a list) is scanned for
      `.so`, `.dylib` and `.dll`; a module declares itself with `ATHENASIP_PLUGIN_MODULE`
      (`src/plugins/plugin_module.h`), which exports its contract version, name and a
      register function; a version mismatch or a non-module is refused with the reason and
      never fatal. `athenasip --list-plugins` (a flag, as the rest of the command line is)
      shows the outcome and every driver. The server exports its symbols, a module must not
      link `athena_core`, and `create_as` casts by kind, not `dynamic_cast`. Built and
      loaded by the tests (`tests/modules/`). Windows loading is written but not built.
- [x] **Guides for a reader without a telecoms background**: how a call works, clustering,
      media engines, troubleshooting, and certificates for clients.
- [x] **Found while writing them, and fixed**: the callee's 100 Trying is no longer forwarded
      (RFC 3261 16.7 step 5); SIGTERM, which `systemctl stop` sends, shuts the node down
      cleanly and publishes "stopped" instead of killing it; the harness configs lose keys
      nothing reads.

### The rtpengine pool, and calls a dead node left (2026-10-06)

- [x] **A pool of rtpengine engines.** `media.rtpengine.engines` adds engines to the URL's.
      A new call goes to one chosen from its Call-ID among those answering, the same on every
      node, and the engine is recorded on the call (`Call::media_engine`,
      `rtpengine://host:port`), so every later request for it goes there from any node. An
      engine that gives no answer is out of the pool until it answers a ping
      (`ping_interval`); the call that found it out moves to another after one timeout, and a
      call already on it stays. Proven by `test/e2e/run.sh --rtpengine`, which now runs two
      engines and stops the first mid-run (`engine-failover`), 14 of 14.
- [x] **Call records a gone or restarted node left behind.** The live node with the lowest
      id closes them (`Core::_orphan_sweep`): at once when the media was not anchored, and
      for anchored media once the engine holds nothing for the call or it has been silent
      for `sip.media_timeout`. A node judges others only after three status intervals.
      Engines now say `held: false` for a call they hold nothing for. Proven in
      `test/e2e/cluster.sh` (`failover-orphan-closed`). `docs/clustering.md` no longer
      claims phones hang up when a node dies: with rtpengine the media carries on.
- [x] **The whole suite green in both phases** (`20261006-113507`): unit 1268, smoke on
      every transport, the console 260 and 5, AthenaPhone 88 with werift calls carrying
      RTP both ways, direct and through TURN.

### The local UA (2026-10-06)

- [x] **The fourth transaction user** (`src/local_ua.*`). `LocalUA::hang_up` sends each end
      of a confirmed dialog the BYE its peer would send (RFC 3261 12.2.1.1, 15.1.1): the
      end's remote target, the peer's tag in From, one past the peer's CSeq, and the route set
      from this node's own entries onward. It enters the proxy at `Core::local_request` as if
      the peer had sent it, through a server transaction that keeps the answer, so flow
      tokens reach a browser's flow and dialog tracking ends the call as for any BYE.
- [x] **Ending calls by policy says so.** `sip.media_timeout` and `sip.max_call_duration`
      send each end a BYE; an RFC 4028 lapse still sends none (8.3).
- [x] **`DELETE /api/v1/calls/{call}`**, under `manage-cluster`: 202 with the BYEs on their
      way, 404 for no live call, 409 for one not yet answered.


### The configuration schema (2026-10-07)

- [x] **`--print-config` prints what the loader reads.** The SIP timers were printed flat
      under `sip` (`timer_t1_rtt_ms`), eight of them not at all, nor the ICE and TURN
      settings; the output now loads back as the same configuration
      (`ConfigTest.TheEffectiveConfigurationLoadsBackAsTheSame`). A reliable-transport
      retransmit flag that nothing read is gone.
- [x] **Every setting described once** (`src/config_schema.cpp`, `plugins::Setting`): type,
      default, limits, meaning. Tests hold it to the loader: every setting set to something
      other than its default comes back where the schema puts it, every stated default is
      the node's, and nothing `--print-config` prints is undescribed.
- [x] **`athenasip --print-schema`** prints a JSON Schema (draft 2020-12) for an editor;
      `=markdown` prints `docs/configuration-reference.md`. Both are checked in and a test
      fails when they fall behind. The shipped example, the harness configurations and both
      nodes' files validate against it.
- [x] **A misspelt key is warned about**, naming the one probably meant, from the same
      section or by name from another (`'sip.media_anchor' ... did you mean
      'behaviour.media_anchor'?`). A driver's own section is left to the driver.
- [x] **Drivers describe their own sections** with a static `settings()`, which
      `PluginRegistry::add<T>` picks up (contract version 18). `mqtt`, `builtin`,
      `rtpengine`, `apns`, `fcm` and `webpush` do; their sections are in the reference and
      the JSON Schema, and a misspelt key inside one is named
      (`'media.rtpengine.timeout' ... did you mean 'media.rtpengine.timeout_ms'?`). A
      driver that says nothing has its section left alone. `docs/plugins.md` shows how.
- [x] **A module's driver describes its section too.** `ModuleHost::add<T>` forwards
      `T::settings()` as the registry's does, and a module's section is checked for misspelt
      keys once the module has loaded (`ModuleLoaderTest.AModulesDriverDescribesItsSection`,
      `ConfigSchemaTest.AMisspeltKeyInAModulesSectionIsNamed`). Found by the documentation
      audit below; v18 had not been released, so the contract version stays 18.

### The README and documentation, as for a release (2026-10-07)

- [x] **The README** drops the "ALPHA - DO NOT USE" banner and says what is there: clustering,
      browsers and phones, mobile push, the plugin contract with modules, the configuration
      checks. Its limits name what is not proven (push against the real services, more than
      two nodes) or not built (conferencing, presence).
- [x] **Quick-start guides**, one per way in, under `docs/quick-start/`: Docker, one node by
      hand, a server on Linux (systemd, Redis, TLS, HTTPS), connecting phones, and calling
      from a browser (rtpengine, WSS, TURN, the subscriber's config endpoint).
      `docs/quick_start.md` is their index.
- [x] **Every doc checked against the code** and corrected: the configuration search order,
      `--check`'s public address line, `/metrics`, `--list-plugins`, the node certificate's
      names, both WebSocket certificate errors, the source layout and the push kind in
      architecture, the local UA in the call flow and glossary, the rtpengine pool's
      defaults and ordering, the exit code for a missing password, and the stale `wss`
      note in the interop fixture's README. Every relative link and anchor resolves.

## Milestone 5 - Scripting and trunks

The design is `docs/design/scripting-1-the-node.md` and `scripting-2-the-engine.md`
(2026-10-07); the plan is Milestone 5 in `ACTIVE.md`.

### The seam (2026-10-08)

- [x] **The `policy` plugin kind** (`src/policy/policy.h`, contract version 19). `Policy`
      answers four questions the node used to answer inline - `authorize`, `route`,
      `on_failure`, `register_` - with decision structs (`AuthDecision`, `RouteDecision`,
      `Target`, `FailureDecision`, `RegisterDecision`) that the node carries out. A
      `RequestView` gives the policy the message and what only the node knows: relay, from a
      peer, a sealed flow token, a remaining Route. A `Host` lends it the datastore through
      the strand, the node's own addresses and the configuration. `policy.url` picks the
      driver; `docs/plugins.md` documents the kind.
- [x] **`builtin://`** (`src/policy/builtin_policy.*`) holds what were `Proxy::_authorize`,
      the realm, subscriber and binding lookups of `_determine_targets`, the relay rule of
      `_authorize_relay`, and the registrar's realm, unserved-domain and expiry steps. It is
      the default, and a `Core` built without a policy uses it, so every proxy and registrar
      test runs against it unchanged and passes. The proxy keeps the mechanism: Digest
      challenge and verification in one realm or any (`_authenticate`,
      `_authenticate_any`), the Route and flow-token targets, the expansion of a subscriber
      into bindings, push and peer forwarding, and the 16.7 rules. The OPTIONS answer for
      the node itself stays native (RFC 3261 11.2), and the design says so.
- [x] **After a failed branch the policy is asked** (`Proxy::_after_failure`): go on, stop
      with the best so far, or try new targets first. 2xx and 6xx never reach it. A policy
      that cannot decide there is taken to say go on; anywhere else it is a 500.
- [x] **An off-node call follows the server's media policy.** A Request-URI in no realm here
      got a default-constructed `MediaPolicy` (anchor, mirror) whatever `behaviour` said; the
      route decision now carries the server's
      (`ProxyMediaTest.ACallLeavingTheNodeFollowsTheServersMediaPolicy`).
- [x] Tests: `tests/proxy_policy_test.cpp` drives the proxy and registrar through a scripted
      policy (refusal with its code, a policy that cannot decide, a reply in place of
      targets, targets the policy names, an empty route, stop and insert after a failure,
      no question after a 2xx, a REGISTER refused before any challenge and one challenged
      in the realm the policy names). 1307 unit tests, one skipped without a resolver. The
      sipp harnesses were not run: Docker Desktop is paused.
- [x] **A TLS peer is a node only under the cluster CA.** `TLSConnection` read the subject
      CN of any certificate that verified, and the proxy trusts a channel with one as a cluster
      peer, which skips authentication. Nothing reached it yet - the client listener asks for
      no certificate and outbound TLS went only to peers - but a trunk context verifying a
      carrier against the system store would have made the carrier a peer. The connection now
      takes a `cluster` flag, set by the inter-node listener and by `_secure_flow` under the
      cluster context, and names nobody without it
      (`ClusterTlsTest.ACertificateFromAnotherAuthorityNamesNoNode`, red before the fix).

### The engine (2026-10-08)

- [x] **The Lua engine** (`src/script/lua_engine.*`, replacing the stubs that were never
      built). Lua 5.4 from the system (Homebrew `lua@5.4`, Debian `liblua5.4-dev`), bound by
      hand. One state per load with a counting allocator; a coroutine per hook call; a store
      lookup issues the asynchronous datastore call and yields, and the answer resumes it by
      a post, never inline. Budgets for instructions (counted every 1000, waiting excluded),
      wall clock and memory. An error carries the script's file, line and a traceback. A
      reload that fails keeps the running scripts; a call in flight finishes on the state it
      started on. No `io`, `debug`, `package`, `load`, `loadfile` or `dofile`; of `os`, the
      clock only; `require` confined to `policy.lua.path` and the standard scripts, with
      names that cannot climb out of a directory. 15 tests in `tests/script/`.
- [x] **The library** (`src/script/lua_library.*`): the request (method, URIs, From, To,
      `in_dialog`, the node's flags, the source, headers), the response for `on_failure`,
      URIs, realms with `behaviour` and `behaviour_effective`, subscribers, bindings,
      `athenasip.store`, `.node`, `.config` (parsed values, then the file, then the
      schema's default), `.log` with the script's file and line, `.sip`. Objects handed to a
      hook refuse use once the call has ended. The answers (`athenasip.auth`, `.route`,
      `.register`) are made by `scripts/athenasip/prelude.lua`, which checks its arguments.
- [x] **`lua://`** (`src/policy/lua_policy.*`) with `policy.lua.path`, `.entry`,
      `.instruction_limit`, `.timeout_ms`, `.memory_limit_mb`. The standard scripts
      (`scripts/main.lua`, `scripts/athenasip/standard.lua`) are built into the binary by
      CMake, so a node always has the ones that match it, and installed to
      `share/athenasip/scripts` to read and copy. `standard.lua` is `builtin_policy.cpp`
      line for line. A script with no `on_failure` goes on, as builtin does.
- [x] **The proof, kept**: `ATHENA_TEST_POLICY=lua` runs any test against the standard
      scripts; the whole suite passes that way (1323). `PolicyEquivalenceTest` runs the
      proxy, registrar, dialog and core suites in a child under the scripts, and feeds both
      drivers the same requests against one store, comparing every field of every decision,
      store failures included. Two positive controls before believing it: `standard.lua`
      refusing every INVITE failed 112 of those tests, and a one-word change to a reason
      phrase failed the differential. The first control also found two tests that indexed
      an empty list when nothing was forwarded (`proxy_flow_routing_test.cpp`); they assert
      first now.
- [x] **`athenasip --check`** creates the policy as the node does, so a script that does
      not compile fails the check with its file and line
      (`CliCheckTest.AScriptThatDoesNotCompileFailsTheCheckWithItsLine`).
- [x] `docs/scripting.md` is the reference; Lua is in `docs/compiling.md`, the Linux
      quick start, `CONTRIBUTING.md`, `CLAUDE.md` and the Dockerfile.
- [x] **Reload from outside.** `SIGHUP`, so `systemctl reload` (the unit gains
      `ExecReload`), and `POST /api/v1/policy/reload` (`manage-cluster`, 422 with the
      script's line when the new scripts do not load) call `Core::policy_reload`, which keeps
      the rules in force on failure and publishes the node's status at once on success.
      `Policy` gains `reload()` and `fingerprint()`; `lua://`'s fingerprint is the SHA-256 of
      every script it loaded, and the node status carries `policy: {driver, fingerprint}` so
      a console can see nodes that disagree (`tests/api/policy_reload_test.cpp`).
