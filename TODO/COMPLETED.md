# AthenaSIP - Completed Work

Reconstructed from git history (135 commits, 2025-02-06 to 2026-07-12) and the
current source. Items are what exists and works in the tree as of `8651ced` plus the
uncommitted working tree on 2026-09-17.

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
      transactions refactor; see ACTIVE.md section 3.)

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
      LocalEventSystem. Helpers for pumping the global io_context. (Currently broken by
      the snake_case rename; see ACTIVE.md section 4.)

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
- [x] Docs updated: `architecure.md` driver table, `configuration.md` datastore table
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

