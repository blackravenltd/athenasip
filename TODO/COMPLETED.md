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

Work since the last tag is at the end, under Milestone 3.

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

Everything down to "The mixed-transport call" shipped in 0.6.0 and 0.7.0. What follows
it is on `develop` and not yet tagged.

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
