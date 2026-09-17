# AthenaSIP - Completed Work

Reconstructed from git history (135 commits, 2025-02-06 to 2026-07-12) and the
current source. Items are what exists and works in the tree as of `5ca2c9e` plus the
uncommitted working tree on 2026-09-17.

## Foundations

- [x] CMake 3.23+ / C++20 build with a static `athena_core` library and `athenasip`
      executable; optional `athenasip_tests` (GoogleTest) behind `ATHENA_BUILD_TESTING`.
      Homebrew prefix discovery on macOS. Builds clean on Darwin 24.6 (`0514016`, `de23c27`).
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
      `websocket_connection.cpp`) (`6279539`, `fd6a087`, `bd2848e`).
- [x] `Channel`: byte-stream to SIP message framing (header/body split on CRLFCRLF,
      Content-Length body accumulation), outbound serialisation with Via/Record-Route/
      Content-Length fix-up, registration with `Core` (`ac9b909`).
- [x] `is_reliable()` on all connections (`ab2a675`).

## SIP parsing and message model

- [x] `SIPHeader`: request/response first line, ordered header list + lookup map,
      folded-line continuation, add/add_start/remove_value/clear.
- [x] Typed header registry (`Header::create` factory, `register_factory`): `ViaHeader`,
      `CSeqHeader`, `AuthorizationHeader`, `SIPIdentityHeader` (From/To/Contact with tags),
      `SupportedHeader`, `UIntHeader`, `StringHeader` fallback (`f1c3ab0`, `b6a590a`).
- [x] `SIPUri`, `SIPIdentity`, `Realm`, `Subscriber`, `Authorization` (Digest parsing)
      types under `src/types/`.
- [x] `SIPMessage::generate_response` (copies From/To/Call-ID/Via/CSeq, adds To tag,
      `Allow`), `get_transaction_id` (top Via branch + CSeq method).
- [x] `SDP` parser/serialiser with session and media sections, `Media::get_unique_id`
      (mid + media + codec hash) (`513662d`, `b0d4729`).

## Core signalling

- [x] `Core` (formerly `Registrar` + `SIPCore`, merged in `3f82a62`): owns config,
      datastore, event system, servers, channels, transactions, calls, RTP relay, admin API.
- [x] Transaction layer: transaction lookup/creation per message, INVITE vs non-INVITE
      typing, single B/F timeout timer, `transaction_end_all` on shutdown
      (`9e06214`, `a73f25d`, `093c2ba`, `f4bf907`).
- [x] 400 Bad Request for messages missing Via/CSeq.
- [x] REGISTER with Digest authentication: 401 challenge with HMAC-SHA256 signed nonce
      (realm id + random + timestamp, secret from `realm.nonce_secret`), nonce stored in
      datastore and in an `ExpirySet` cache, subscriber lookup by AoR, HA1 verification,
      200 OK with Contact echo (`f6096f8`, `f661ebc`, `b053fe4`, `826a0ab`, `f1ded95`).
- [x] Per-subscriber event subscription on successful REGISTER; `subscriber/<uri>/status`
      event published with contact, node id, timestamp (`1e3575d`).
- [x] INVITE receive path: 100 Trying, `Call` registered in memory and datastore,
      `subscriber/<uri>/invite` event with JSON `{call_id, from, to, sdp}` (working tree).
- [x] 501 Not Implemented for unknown methods; ACK accepted silently (`1adbc2f`).
- [x] Node lifecycle events: `nodes/<id>/status` started/stopped,
      `nodes/<id>/channels/<endpoint>` registered/closed, transaction registered/
      unregistered (`d1845af`).
- [x] Signal handling: SIGINT orderly shutdown (transactions, channels, servers, relay,
      admin, events, datastore); SIGHUP logged.

## Datastores (`src/datastores/`, URL-selected driver registry)

- [x] `Datastore` interface + scheme registry (`register_driver` / `create_driver`)
      (`df7e6ef`, `6e988c0`).
- [x] MySQL / MariaDB via X DevAPI (`mysql`, `mysqlx`): realm, subscriber, location
      register/unregister, nonce create/check, call create.
- [x] PostgreSQL via libpqxx (`postgres`, `postgresql`): realm, subscriber, location,
      nonce.
- [x] SQLite (`sqlite`, `sqlite3`): realm, subscriber, location, nonce; creates the DB
      file if missing (`bcf45a0`).
- [x] Redis via boost.redis (`redis`, `rediss`, `redis+ssl`): JSON-encoded realm/
      subscriber, location, nonce with TTL; own io thread, sync-over-async helpers with
      timeouts (`62ebc47`, `607ed65`).
- [x] MySQL schema dump in `sql/create.sql` (`call`, `location`, `nonce`, `participant`,
      `realm`, `subscriber`, `new_id()` routine) and `bin/save_db_create.sh`.

## Event systems (`src/events/`, URL-selected driver registry)

- [x] `EventSystem` interface with sync and callback overloads, `Subscription`,
      MQTT-style `TopicFilter` (`+`, `#`, `$` rules) (`df7e6ef`).
- [x] `LocalEventSystem` (`athena`, `local`, `memory`): in-memory pub/sub dispatched on
      the global io_context, exception-safe callbacks; unit tested.
- [x] `MQTTEventSystem` (`mqtt`, `mqtt5`): boost.mqtt5 client on its own thread/strand,
      URL parsing (host, port, client_id, keep_alive, username, password, prefix),
      QoS 0 publish, broker-side subscription dedupe, SUBACK reason handling,
      receive loop with session-expiry resubscribe, prefix-stripped local dispatch
      (`22af397`, `360c138`).

## Media

- [x] `RTPRelay`: port pool allocator over a bind address and range.
- [x] `RTPRelaySet`: single UDP socket per stream that learns remote endpoints from the
      first packet and relays between all others (`4bf1a9d`, `372f612`).
- [x] `RTPProxyClient`: rtpproxy text protocol client (V, U, L, R, D commands) on a
      strand, async send/receive with response parsing (`src/rtp/rtp_proxy_client.*`).
- [x] `MediaStream` / `Call::streams` model. (Wiring into INVITE was removed in the
      transactions refactor; see ACTIVE.md section 3.)

## Admin / HTTP

- [x] Beast HTTP server with a middleware chain (`AdminAPI`, `HttpSession`),
      status helpers (200/400/404/500 JSON), `StaticMiddleware` serving the built
      athenasip-admin React bundle from `admin/` (`57f4667`, `6537753`, `14abca9`).

## Scripting

- [x] `ScriptEngine` interface and `LuaScriptEngine`: Lua state lifecycle, `print`,
      `include()` with allowed-path check, `log.{debug,info,warn,error}` table,
      `execute_lua_fn`. Example scripts in `config/`. (Not started from `main`.)

## Tests (`tests/`, GoogleTest)

- [x] DelayedTask, ExpiryMap, ExpirySet, Util, Version, URL, header registry,
      LocalEventSystem. Helpers for pumping the global io_context. (Currently broken by
      the snake_case rename; see ACTIVE.md section 4.)

## Refactors landed

- [x] FieldValue -> Header, siptypes -> types (`f1c3ab0`).
- [x] Session -> Channel; TLSSession folded into Session (`ac9b909`, `3946a2c`).
- [x] Databases -> Datastores with explicit driver registration (`6e988c0`, `de23c27`).
- [x] Registrar + SIPCore -> Core (`3f82a62`).
- [x] camelCase -> snake_case across `src/` (`5ca2c9e` + working tree).

## Milestone 1 - Foundations

### Working tree and hygiene (2026-09-17)

- [x] `tests/` follow the snake_case rename (`get_registry`, `get_global_io_context`),
      and the `EventSystemTest` cases connect the event system before publishing, which
      `LocalEventSystem` now requires. ctest is 44/44 green (`5f53f57`).
- [x] Working tree split into three commits: the snake_case rename (`5f53f57`), the
      weak_ptr back-reference refactor and outbound INVITE sketch (`a1168b0`), and call
      record persistence (`479db93`).
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

