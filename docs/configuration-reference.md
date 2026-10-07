# AthenaSIP - Configuration reference

Every setting, its default and its limits. [configuration.md](configuration.md) explains
them; this page is generated from the settings the server reads by
`athenasip --print-schema=markdown`, so edit `src/config_schema.cpp` rather than this.
`athenasip --print-schema` prints the same as a JSON Schema
([configuration.schema.json](configuration.schema.json)), which an editor can check a
file against as it is written.

## `sip`

The node, and how it treats SIP.

| Setting | Takes | Default | |
|---|---|---|---|
| `sip.node_id` | text | required | This node's name. Unique in the cluster. |
| `sip.public_address` | text | none | The address this node tells others to reach it on. Needed in a container, behind NAT or a balancer; empty falls back to the bind address, or to one peers have seen this node at. |
| `sip.localnet` | list | `[]` | Prefixes on this node's side of the router, such as 192.168.0.0/16. A far end inside them is given the local address and port; everyone else the public ones. |
| `sip.allow_unencrypted` | true or false | `true` | False refuses to start with a udp, tcp or plain websocket listener. |
| `sip.forward_register` | one of `subscribers`, `never` | `subscribers` | A REGISTER for a domain this node does not serve (RFC 3261 10.3): subscribers forwards one from this node's own subscribers; never answers 403. |
| `sip.log_messages` | true or false | `false` | Log every message in full, not only its first line. Credentials are redacted. |
| `sip.session_expires` | 0, or 90 or more | `1800` | RFC 4028: the session interval, in seconds, put on a call that asked for none. 0 inserts none. |
| `sip.session_min_se` | integer, 90 or more | `90` | RFC 4028: the shortest session interval a call may negotiate, in seconds. |
| `sip.require_session_timer` | true or false | `false` | Add Require: timer when the caller did not. NOT RECOMMENDED by RFC 4028: every call to an endpoint without the extension fails. |
| `sip.max_call_duration` | integer | `0` | The longest any call is held, in seconds; each end is then sent a BYE. 0 is no limit. It cuts live calls too. |
| `sip.media_timeout` | integer | `300` | Seconds an anchored call may carry no media before each end is sent a BYE. 0 turns it off. RTCP counts, so a call on hold is not silent. |
| `sip.connect_timeout_ms` | integer | `4000` | How long opening a connection to a next hop may take, in milliseconds. |
| `sip.flow_idle_timeout` | integer | `300` | Seconds a UDP flow may sit idle before it is forgotten. 0 never forgets. Keep it above 32 (64 * T1). |

### `sip.timers`

The RFC 3261 section 17 timers. a to k are multiples of T1 or T4, not milliseconds. Leave them alone.

| Setting | Takes | Default | |
|---|---|---|---|
| `sip.timers.t1_rtt_ms` | integer, 1 to 65535 | `500` | T1, the round-trip estimate, in milliseconds. |
| `sip.timers.t2_max_retransmit_interval_ms` | integer, 1 to 65535 | `4000` | T2, the longest retransmit interval, in milliseconds. |
| `sip.timers.t4_network_propagation_ms` | integer, 1 to 65535 | `5000` | T4, the longest a message stays in the network, in milliseconds. |
| `sip.timers.a_invite_initial` | integer, 1 to 65535 | `1` | Timer A, the first INVITE retransmit, in T1. |
| `sip.timers.b_invite_timeout` | integer, 1 to 65535 | `64` | Timer B, the INVITE transaction timeout, in T1. |
| `sip.timers.d_invite_duration` | integer, 0 to 65535 | `64` | Timer D, the wait for response retransmits, in T1. |
| `sip.timers.e_non_invite_initial` | integer, 1 to 65535 | `1` | Timer E, the first non-INVITE retransmit, in T1. |
| `sip.timers.f_non_invite_timeout` | integer, 1 to 65535 | `64` | Timer F, the non-INVITE transaction timeout, in T1. |
| `sip.timers.g_server_invite_initial` | integer, 1 to 65535 | `1` | Timer G, the first INVITE response retransmit, in T1. |
| `sip.timers.h_server_invite_timeout` | integer, 1 to 65535 | `64` | Timer H, the wait for an ACK, in T1. |
| `sip.timers.i_server_invite_duration` | integer, 0 to 65535 | `1` | Timer I, the wait for ACK retransmits, in T4. |
| `sip.timers.j_server_non_invite_duration` | integer, 0 to 65535 | `64` | Timer J, the wait for non-INVITE retransmits, in T1. |
| `sip.timers.k_non_invite_duration` | integer, 0 to 65535 | `1` | Timer K, the wait for response retransmits, in T4. |
| `sip.timers.c_invite_proxy_ms` | integer, 180001 or more | `240000` | Timer C, how long a proxied INVITE may ring, in milliseconds. Over three minutes (RFC 3261 16.6 step 11). |

## `udp`

SIP over UDP. No section, no listener.

| Setting | Takes | Default | |
|---|---|---|---|
| `udp.enable` | true or false | `true` | A listener whose section is present is enabled unless this is false. |
| `udp.address` | text | `0.0.0.0` | The address to bind. |
| `udp.port` | integer, 0 to 65535 | required | The port to bind. Required unless enable is false. |
| `udp.public_port` | integer, 0 to 65535 | `0` | The port a router forwards to this listener, for what the node tells others. 0 is the bound port. |

## `tcp`

SIP over TCP. No section, no listener.

| Setting | Takes | Default | |
|---|---|---|---|
| `tcp.enable` | true or false | `true` | A listener whose section is present is enabled unless this is false. |
| `tcp.address` | text | `0.0.0.0` | The address to bind. |
| `tcp.port` | integer, 0 to 65535 | required | The port to bind. Required unless enable is false. |
| `tcp.public_port` | integer, 0 to 65535 | `0` | The port a router forwards to this listener, for what the node tells others. 0 is the bound port. |

## `tls`

SIP over TLS. No section, no listener.

| Setting | Takes | Default | |
|---|---|---|---|
| `tls.enable` | true or false | `true` | A listener whose section is present is enabled unless this is false. |
| `tls.address` | text | `0.0.0.0` | The address to bind. |
| `tls.port` | integer, 0 to 65535 | required | The port to bind. Required unless enable is false. |
| `tls.public_port` | integer, 0 to 65535 | `0` | The port a router forwards to this listener, for what the node tells others. 0 is the bound port. |
| `tls.cert_pem_filename` | text | none | The certificate, PEM. |
| `tls.key_pem_filename` | text | none | The certificate's key, PEM. |

## `websocket`

SIP over WebSocket (RFC 7118), for browsers. No section, no listener.

| Setting | Takes | Default | |
|---|---|---|---|
| `websocket.enable` | true or false | `true` | A listener whose section is present is enabled unless this is false. |
| `websocket.address` | text | `0.0.0.0` | The address to bind. |
| `websocket.port` | integer, 0 to 65535 | required | The port to bind. Required unless enable is false. |
| `websocket.public_port` | integer, 0 to 65535 | `0` | The port a router forwards to this listener, for what the node tells others. 0 is the bound port. |
| `websocket.tls` | true or false | `false` | Serve wss on port. Needs this section's own certificate and key. A page served over https can only use wss. |
| `websocket.secure_port` | integer, 0 to 65535 | `0` | A second, wss listener beside a plain one. 0 is none. Unused when tls is true. |
| `websocket.cert_pem_filename` | text | none | The wss certificate, PEM. Empty uses the tls section's. |
| `websocket.key_pem_filename` | text | none | The wss certificate's key, PEM. Empty uses the tls section's. |

## `cluster`

The inter-node listener: mutual TLS under the cluster CA (docs/certificates.md).

| Setting | Takes | Default | |
|---|---|---|---|
| `cluster.enable` | true or false | `false` | Join a cluster. Needs a shared datastore, an event bus, and ca, cert and key. |
| `cluster.address` | text | `0.0.0.0` | The address to bind. |
| `cluster.port` | integer, 0 to 65535 | `5062` | The port to bind. |
| `cluster.advertise` | text | none | What other nodes dial. The node's certificate must name it. Empty is the bind address, or sip.public_address on a wildcard bind. |
| `cluster.ca` | text | none | The cluster CA's certificate, from athenasip --ca-init. |
| `cluster.cert` | text | none | This node's certificate, from athenasip --ca-node. |
| `cluster.key` | text | none | This node's key. |

## `datastore`

Where users, registrations and calls are kept. Each driver may have a section of its own here, named after it.

| Setting | Takes | Default | |
|---|---|---|---|
| `datastore.url` | text | required | memory:// keeps nothing over a restart and serves one node; a cluster shares redis://. |

## `events`

The event bus: observability, presence and discovery. Each driver may have a section of its own here.

| Setting | Takes | Default | |
|---|---|---|---|
| `events.url` | text | required | local:// stays in this process; nodes find each other over mqtt://. |
| `events.status_interval` | integer | `30` | Seconds between this node's status reports on the bus. 0 reports once. |

## `media`

The media engine that relays calls. Each driver may have a section of its own here.

| Setting | Takes | Default | |
|---|---|---|---|
| `media.url` | text | `builtin://` | builtin:// relays RTP; WebRTC (ICE, DTLS, SRTP) needs rtpengine://host:port. |

## `plugins`

Plugin modules, loaded at start.

| Setting | Takes | Default | |
|---|---|---|---|
| `plugins.path` | list, or one value | `[]` | Directories of plugin modules (.so, .dylib, .dll). |

## `push`

RFC 8599 push notifications. Off with no urls. Each driver may have a section of its own here.

| Setting | Takes | Default | |
|---|---|---|---|
| `push.urls` | list | `[]` | One push service per entry: apns://, fcm://, webpush://. |
| `push.timeout` | integer, 1 to 30 | `10` | Seconds a request waits for the client to re-register after a push (RFC 8599 5.6.2). |
| `push.refresh` | integer, 121 or more | `180` | Seconds before a push binding expires that a push asks the client to refresh it (RFC 8599 5.5). A push binding must then ask for at least this plus 60. |

## `behaviour`

The defaults for what differs between SIP servers. A realm overrides any of them over the API.

| Setting | Takes | Default | |
|---|---|---|---|
| `behaviour.media_anchor` | true or false | `true` | Relay media through the media engine. False leaves it end to end, which fails behind NAT. |
| `behaviour.media_profile` | one of `mirror`, `transport`, `webrtc`, `rtp`, `srtp` | `mirror` | What each leg's media is offered as: mirror gives each leg what it offered; the others are explained in docs/behaviour.md. |
| `behaviour.qualify_interval` | 0, or 5 to 86400 | `0` | Seconds between OPTIONS to a registered client. 0 is never. |
| `behaviour.rewrite_contact` | true or false | `false` | Rewrite a registering client's Contact to the address it was seen at. |

## `log`

What the node logs, and how.

| Setting | Takes | Default | |
|---|---|---|---|
| `log.level` | one of `debug`, `info`, `warn`, `error` | `debug` | The least severe message logged. |
| `log.format` | one of `text`, `json` | `text` | One line of text per message, or one JSON object. |

## `calls`

The record of calls.

| Setting | Takes | Default | |
|---|---|---|---|
| `calls.history_retention` | integer | `2592000` | Seconds an ended call's record is kept. 0 keeps records for ever. |

## `http`

The admin API, the console and the file server. No section, no listener.

| Setting | Takes | Default | |
|---|---|---|---|
| `http.address` | text | `0.0.0.0` | The address to bind. |
| `http.port` | integer, 0 to 65535 | required | The port to bind. |

### `http.tls`

An HTTPS listener beside the plain one. Browsers give the microphone only to a secure page.

| Setting | Takes | Default | |
|---|---|---|---|
| `http.tls.enable` | true or false | `false` | Serve HTTPS. |
| `http.tls.address` | text | none | The address to bind. Empty is http.address. |
| `http.tls.port` | integer, 0 to 65535 | `8443` | The port to bind. |
| `http.tls.cert_pem_filename` | text | none | The certificate, PEM. Empty uses the tls section's. |
| `http.tls.key_pem_filename` | text | none | The certificate's key, PEM. Empty uses the tls section's. |

### `http.api`

The admin and client API (docs/api/openapi.yaml).

| Setting | Takes | Default | |
|---|---|---|---|
| `http.api.enable` | true or false | `true` | Serve the API when its section is present. |
| `http.api.session_lifetime` | integer, 1 or more | `43200` | An admin session's absolute lifetime, in seconds. |
| `http.api.session_idle` | integer | `3600` | How long an admin session may sit unused, in seconds. 0 turns the check off. |
| `http.api.ice_servers` | list | `[]` | STUN and TURN URLs handed to web clients, each a URL or a map with url. They do not affect this node's own media. |
| `http.api.turn_shared_secret` | text | none | The secret shared with the TURN server (coturn's static-auth-secret), from which expiring credentials are minted. Empty serves turn: URLs without credentials. |
| `http.api.turn_credential_ttl` | integer, 1 or more | `3600` | How long a minted TURN credential lasts, in seconds. |

#### `http.api.rate_limits`

A burst allowance, then a rate per minute. 0 in either turns that limit off.

#### `http.api.rate_limits.open`

Open routes, unknown endpoints and credentials that do not resolve, by source address.

| Setting | Takes | Default | |
|---|---|---|---|
| `http.api.rate_limits.open.burst` | integer | `30` | Requests allowed at once. |
| `http.api.rate_limits.open.per_minute` | integer | `30` | Requests allowed per minute after that. |

#### `http.api.rate_limits.login_source`

Sign-ins, by source address.

| Setting | Takes | Default | |
|---|---|---|---|
| `http.api.rate_limits.login_source.burst` | integer | `10` | Requests allowed at once. |
| `http.api.rate_limits.login_source.per_minute` | integer | `5` | Requests allowed per minute after that. |

#### `http.api.rate_limits.login_user`

Sign-ins, by username.

| Setting | Takes | Default | |
|---|---|---|---|
| `http.api.rate_limits.login_user.burst` | integer | `5` | Requests allowed at once. |
| `http.api.rate_limits.login_user.per_minute` | integer | `1` | Requests allowed per minute after that. |

#### `http.api.rate_limits.session`

A signed-in caller, by session.

| Setting | Takes | Default | |
|---|---|---|---|
| `http.api.rate_limits.session.burst` | integer | `60` | Requests allowed at once. |
| `http.api.rate_limits.session.per_minute` | integer | `300` | Requests allowed per minute after that. |

### `http.files`

A static file server, for the console.

| Setting | Takes | Default | |
|---|---|---|---|
| `http.files.enable` | true or false | `true` | Serve files when the section is present. |
| `http.files.path` | text | none | The directory served. |
| `http.files.spa` | true or false | `true` | Answer a path that matches no file with index.html, so a single-page app's routes survive a reload. Never for /api/ or a path with a file extension. |
