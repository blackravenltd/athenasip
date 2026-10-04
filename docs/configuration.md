# AthenaSIP - Configuration

One YAML file. With no `--config`, the first of these that exists is read:

1. `$ATHENASIP_CONFIG`
2. `/etc/athenasip/config.yaml`
3. `~/.athenasip/config.yaml`

[`config/config.example.yaml`](../config/config.example.yaml) shows the settings in place.
An unknown `behaviour` value, a missing required key or an unparseable value stops the
node at startup.

## Minimal configuration

```yaml
sip:
  node_id: sip-0001

udp:
  port: 5060
```

`sip.node_id` is the only required key. A listener section that is present is enabled and
needs a `port`; a node with no listener section listens for nothing. The plugin URLs
default to `memory://`, `local://` and `builtin://`, which need no external service. The
admin API and console need an [`http`](#http) section.

## Inspecting a node

| Command | What it does |
|---|---|
| `athenasip --print-config` | Prints the effective configuration (file, search path and defaults resolved) as YAML, names the file it came from, and exits. Plugin sections are copied through as written. |
| `athenasip --check` | Tries the datastore, event bus, media engine, each certificate a listener needs and, in a cluster, a mutual-TLS handshake with each node that reports itself up. Prints `ok` or `FAIL` per item; exits 0 if all passed, 1 otherwise. Starts and changes nothing, so it is safe beside a running node. Passwords in URLs are masked. |

```
ok    configuration        /etc/athenasip/config.yaml
ok    datastore            redis 0.0.1 at redis://:***@127.0.0.1:6379/3
ok    events               mqtt 0.0.1 at mqtt://10.35.1.10:1883
FAIL  peer node-b          10.0.0.2:5062 - Connection refused
```

## `sip`

| Key | Default | Meaning |
|---|---|---|
| `node_id` | required | This node's name, unique across the cluster. Used on the event bus and in `GET /api/v1/nodes`. |
| `public_address` | unset | The address written into `Via`, `Record-Route` and `Service-Route` and published in the node list. Set it when the node binds `0.0.0.0` in a container, behind a load balancer or behind NAT; leave it unset on a single-homed host. |
| `localnet` | `[]` | Prefixes on the node's own side of the router, e.g. `["192.168.0.0/16", "10.0.0.0/8"]`. A peer inside one is given the node's local address and port instead of `public_address` (Asterisk's `localnet`). A bare address is a prefix of one. |
| `allow_unencrypted` | `true` | Permit SIP over plain UDP, TCP and WS. When `false`, the node refuses to start with a `udp`, `tcp` or plain `websocket` listener enabled (including a plain `websocket.port` beside `secure_port`), and dials only TLS. |
| `forward_register` | `subscribers` | A REGISTER for a domain this node does not serve (RFC 3261 10.3 step 1). `subscribers` forwards it, by DNS (RFC 3263), for a sender who is one of this node's subscribers: one already registered here over a reliable connection, or one answering a 407 for this node's realm. The node puts itself on the Path, so the far registrar's calls come back down the same connection. `never` answers 403. Either way nobody else's REGISTER is relayed. |
| `log_messages` | `false` | Log every SIP message in full (headers and body) rather than its first line. Digest headers are logged as `<redacted>`. |
| `connect_timeout_ms` | `4000` | How long to spend opening a connection to a next hop the node has no flow to (RFC 3261 16.6 step 7). |
| `flow_idle_timeout` | `300` | Seconds a UDP flow may sit idle before the node forgets it. `0` never forgets. The peer's next datagram makes a new flow. |
| `media_timeout` | `300` | Seconds an anchored call may carry no RTP or RTCP before the node releases it. `0` is off. |
| `session_expires` | `1800` | Session interval (RFC 4028 8.1) added to an INVITE that carried none. `0` adds none. Below 90 is refused and the default kept; below 1800 is accepted with a warning. |
| `session_min_se` | `90` | Shortest session interval the node accepts. Below 90 is refused and the default kept. |
| `max_call_duration` | `0` | Longest any call is held, in seconds. `0` is no cap. |
| `require_session_timer` | `false` | Add `Require: timer` when the caller did not advertise support. An endpoint without RFC 4028 then answers 420 and the call fails. |
| `timers` | see below | RFC 3261 transaction timers. |

### Addresses behind NAT

```yaml
sip:
  public_address: 203.0.113.5
  localnet: ["192.168.0.0/16", "10.0.0.0/8"]
udp:
  port: 5060
  public_port: 5080   # the port the router forwards, when it differs
```

Peers outside `localnet` are given `public_address` and the listener's `public_port`;
peers inside are given the local address and bound port. The builtin media relay follows
the same rule for the address it writes into SDP. rtpengine chooses its own addresses.

### Ending dead calls

An endpoint that loses power sends no BYE. Four settings bound how long the node holds
such a call:

| Setting | Reaches | Misses |
|---|---|---|
| `media_timeout` | Calls this node anchors | Media that goes end to end |
| `session_expires` | Calls whose callee implements RFC 4028 | Two endpoints that do not |
| `max_call_duration` | Every call | Cannot tell a live call from a dead one |
| `require_session_timer` | Every call that is set up | Fails calls to endpoints without RFC 4028 |

The first two cover most deployments. When any of them fires the node releases its state
and sends nothing: a proxy must not send BYE (RFC 4028 8.3).

A caller that asks for an interval below `session_min_se` and advertises
`Supported: timer` is answered 422 with `Min-SE`. One that does not advertise it has its
interval raised to `session_min_se` on the way through. The node never names a refresher.

### `timers`

Change these only with a specific reason. A to K are multiples of T1 or T4 (RFC 3261 17).

| Key | Default | Timer |
|---|---|---|
| `t1_rtt_ms` | `500` | T1, round-trip estimate (ms) |
| `t2_max_retransmit_interval_ms` | `4000` | T2, retransmission ceiling (ms) |
| `t4_network_propagation_ms` | `5000` | T4, longest a message stays in the network (ms) |
| `a_invite_initial` | `1` x T1 | A, INVITE client retransmit interval |
| `b_invite_timeout` | `64` x T1 | B, INVITE client timeout |
| `d_invite_duration` | `64` x T1 | D, INVITE client wait for response retransmissions |
| `e_non_invite_initial` | `1` x T1 | E, non-INVITE client retransmit interval |
| `f_non_invite_timeout` | `64` x T1 | F, non-INVITE client timeout |
| `k_non_invite_duration` | `1` x T4 | K, non-INVITE client wait for response retransmissions |
| `g_server_invite_initial` | `1` x T1 | G, INVITE server final-response retransmit interval |
| `h_server_invite_timeout` | `64` x T1 | H, INVITE server wait for ACK |
| `i_server_invite_duration` | `1` x T4 | I, INVITE server wait for ACK retransmissions |
| `j_server_non_invite_duration` | `64` x T1 | J, non-INVITE server wait for request retransmissions |
| `c_invite_proxy_ms` | `240000` | C, how long a proxied INVITE may stay provisional (RFC 3261 16.6 step 11), in ms. Must be larger than 180000; a smaller value is refused and the default kept. |

### Routing to a host name

Not configurable. A request URI that names a host is resolved per RFC 3263: NAPTR, then
SRV, then A/AAAA, trying each target in turn. The nameservers come from
`/etc/resolv.conf`; each query is tried for five seconds, twice round, and answers are
cached for their TTL.

## Listeners

Each of `udp`, `tcp`, `tls` and `websocket` is optional.

| Key | Default | Meaning |
|---|---|---|
| `enable` | `true` if the section is present | Whether the listener starts |
| `address` | `0.0.0.0` | Bind address |
| `port` | required when enabled | Bind port. Conventionally 5060 for `udp` and `tcp`, 5061 for `tls`. |
| `public_port` | the bound port | The port a router forwards to this listener |

`tcp`, `udp` and plain `websocket` are unencrypted.

### `tls`

| Key | Meaning |
|---|---|
| `cert_pem_filename` | PEM server certificate |
| `key_pem_filename` | PEM private key |

Also the fallback certificate for `websocket` and `http.tls`.

### `websocket`

SIP over WebSocket (RFC 7118).

| Key | Default | Meaning |
|---|---|---|
| `tls` | `false` | Make this listener `wss`. Needs `cert_pem_filename` and `key_pem_filename` in this section. |
| `secure_port` | `0` (none) | A second, `wss` listener beside the plain one. Uses this section's certificate, or the `tls` section's. Must differ from `port`. Ignored when `tls` is `true`. |
| `cert_pem_filename`, `key_pem_filename` | unset | Certificate and key for the secure listener |

A page served over HTTPS may only open `wss`, so a browser client needs one of the two.

## `cluster`

The inter-node listener: SIP over mutual TLS between nodes.
[Certificates](certificates.md) covers making the files.

| Key | Default | Meaning |
|---|---|---|
| `enable` | `false` | Whether this node is part of a cluster |
| `address` | `0.0.0.0` | Bind address |
| `port` | `5062` | Bind port |
| `advertise` | derived | The name or address peers dial; the node's certificate must name it. Unset, it is the bind address, or `sip.public_address` when bound to `0.0.0.0`. |
| `ca`, `cert`, `key` | required when enabled | Cluster CA certificate, this node's certificate and its key |

## Plugins: `datastore`, `events`, `media`

Each takes a `url` whose scheme selects the driver, and an optional section named after
the driver for what a URL cannot express. [Plugins](plugins.md) describes the contract.

### `datastore`

| URL | Driver |
|---|---|
| `memory://` (default) | In-process. Nothing survives a restart. Single node only. |
| `redis://[user:password@]host[:port][/db]` | [Redis](https://redis.io/). Shared by a cluster and survives restarts. Port defaults to 6379. `rediss://` and `redis+ssl://` are the same driver over TLS. |

### `events`

| Key | Default | Meaning |
|---|---|---|
| `url` | `local://` | `local://` keeps events in-process. `mqtt://[user:password@]host[:port]` publishes to a broker (port 1883 by default), which is also how nodes discover each other. |
| `status_interval` | `30` | Seconds between retained node-status messages. `0` publishes once at startup. |

`events.mqtt`:

| Key | Default | Meaning |
|---|---|---|
| `client_id` | `athenasip-<node_id>` | MQTT client identifier; must be unique on the broker |
| `keep_alive` | `30` | MQTT keep-alive, seconds |
| `connect_timeout_ms` | `5000` | How long startup waits for the broker before failing |
| `username`, `password` | unset | Broker credentials, if not in the URL |
| `prefix` | `athenasip/` | Topic level prepended to everything published and subscribed, so clusters can share a broker |

[Events](events.md) lists the topics.

### `media`

| URL | Driver |
|---|---|
| `builtin://` (default) | Relays plain RTP from the node's own process. Declines offers that need ICE, DTLS or SRTP; the SDP then passes through untouched and media goes end to end. |
| `rtpengine://host[:port]` | [rtpengine](https://github.com/sipwise/rtpengine) over its ng control port (2223 by default). Required for WebRTC, SRTP, recording and transcoding. |

`media.builtin`:

| Key | Default | Meaning |
|---|---|---|
| `bind_address` | `0.0.0.0` | Where the relay binds |
| `public_address` | `0.0.0.0` | The address written into SDP. Must be reachable by endpoints: on a NAT'd host, the public address. May be a host name, resolved at start and every minute; while it does not resolve, the relay declines. |
| `port_min`, `port_max` | `22000`, `23000` | RTP port range. Behind NAT, forward the whole range. |

`media.rtpengine`:

| Key | Default | Meaning |
|---|---|---|
| `timeout_ms` | `500` | Wait for one ng reply before asking again |
| `attempts` | `3` | Total tries per request |
| `media_address` | unset | Address rtpengine should advertise. Leave unset to let rtpengine's interface configuration decide. May be a host name. A leg inside `sip.localnet` is given the node's local address instead. |

Whether media is anchored at all is [`behaviour.media_anchor`](#behaviour).

## `push`

Push notifications ([RFC 8599](https://www.rfc-editor.org/rfc/rfc8599)) wake a client that
is asleep, as a phone's operating system puts a SIP app to sleep. A client asks for push
by putting `pn-provider`, `pn-prid` and, for some services, `pn-param` on the Contact of
its REGISTER. Off unless `urls` names a service.

| Key | Default | Meaning |
|---|---|---|
| `urls` | `[]` | The push services this node uses, one driver each: `fcm://`, `webpush://`. Each takes a section named after it, below. |
| `timeout` | `10` | Seconds a call waits for the client to wake and register again before the next target is tried, or the caller gets 480. 1 to 30. |
| `refresh` | `180` | Seconds before a push binding expires that the client is pushed to refresh it. Over 120. A client asking for push must register for at least `refresh` + 60 seconds, or it gets 423. |

What the node does:

- A REGISTER asking for a service this node runs is answered 200 with
  `Feature-Caps: *;+sip.pns="<service>"`, and the binding is marked for push. A service it
  does not run, or a Contact missing what the service needs, is 555.
- A call or other new request for a push binding sends a push and holds the request until
  the client registers again, then sends it down the new connection. Any node of a cluster
  can do this, and the client may register again through any node.
- The `pn-*` parameters are never shown in the admin API or on the event bus.

`push.fcm`, for Android through [Firebase Cloud Messaging](https://firebase.google.com/docs/cloud-messaging).
The client's `pn-param` is the Firebase project ID and `pn-prid` its registration token:

| Key | Default | Meaning |
|---|---|---|
| `service_account` | required | Path to the service-account JSON file the Firebase console gives you |
| `ttl` | `60` | Seconds FCM keeps trying to deliver the push |
| `api_base` | `https://fcm.googleapis.com` | Only to go through a proxy |
| `ca_file` | system roots | Extra CA certificates to trust |

`push.webpush`, for browsers ([RFC 8030](https://www.rfc-editor.org/rfc/rfc8030)). The
client's `pn-prid` is its push subscription URL and it has no `pn-param`. The node signs
each push with its VAPID key ([RFC 8292](https://www.rfc-editor.org/rfc/rfc8292)) and gives
the public half to clients as `+sip.vapid`:

| Key | Default | Meaning |
|---|---|---|
| `vapid_private_key` | required | Path to a P-256 private key in PEM: `openssl ecparam -name prime256v1 -genkey -noout -out vapid.pem` |
| `subject` | required | A `mailto:` or `https:` URI the push service can contact you at |
| `ttl` | `60` | Seconds the push service keeps trying |
| `ca_file` | system roots | Extra CA certificates to trust |

iOS (APNs) is not supported yet.

## `behaviour`

```yaml
behaviour:
  media_anchor: true
  media_profile: mirror
  qualify_interval: 0
  rewrite_contact: false
```

| Key | Default | Meaning |
|---|---|---|
| `media_anchor` | `true` | Route media through the media engine |
| `media_profile` | `mirror` | What a leg is offered: `mirror`, `transport`, `rtp`, `webrtc` or `srtp` |
| `qualify_interval` | `0` | Seconds between OPTIONS to each registered client; `0` is never, otherwise 5 to 86400 |
| `rewrite_contact` | `false` | Rewrite forwarded Contacts to the address a message came from |

These are the server defaults. Realms and subscribers override them over the admin API.
[Behaviour](behaviour.md) explains each setting and the overrides.

## `log`

| Key | Default | Values |
|---|---|---|
| `level` | `debug` | `debug`, `info`, `warn`, `error` |
| `format` | `text` | `text`, or `json` for one object per line |

```json
{"at":"2026-10-03T19:44:20Z","level":"info","scope":"channel tls://10.35.1.164:33083","message":"Connected"}
```

`scope` is the component that wrote the line, nested scopes joined with `/`; it is absent
when there is none.

## `calls`

| Key | Default | Meaning |
|---|---|---|
| `history_retention` | `2592000` (30 days) | Seconds a call record is kept in the datastore. `0` keeps them for ever. |

A record is written when a call that reached ringing ends. `GET /api/v1/call-records`
lists them, newest first.

## `http`

The admin API and the console. Without this section neither is served.

```yaml
http:
  address: 0.0.0.0
  port: 8080
  tls:
    enable: true
    port: 8443
  api:
    enable: true
  files:
    path: "../admin/"
```

| Key | Default | Meaning |
|---|---|---|
| `address` | `0.0.0.0` | Bind address |
| `port` | required | Plain HTTP port |
| `tls.enable` | `false` | Add an HTTPS listener serving the same API and console |
| `tls.address` | `http.address` | HTTPS bind address |
| `tls.port` | `8443` | HTTPS port |
| `tls.cert_pem_filename`, `tls.key_pem_filename` | the `tls` section's | HTTPS certificate and key. With neither available the node does not start. |

The console's softphone needs HTTPS (a browser grants no microphone otherwise) and
therefore a `wss` listener.

### `http.api`

Enabled when the section is present. Users sign in with `POST /api/v1/auth/login`; see
[Authentication](authentication.md). `http.api.tokens` is refused at startup.

| Key | Default | Meaning |
|---|---|---|
| `enable` | `true` | Serve `/api/v1` |
| `session_lifetime` | `43200` | Seconds a login lasts. Cannot be `0`. |
| `session_idle` | `3600` | Seconds a login survives unused. `0` is off. |
| `rate_limits` | see below | Request limits |
| `ice_servers` | `[]` | STUN and TURN URLs handed to web clients by `GET /api/v1/client/config`, e.g. `- url: "stun:stun.example.com:3478"` |
| `turn_shared_secret` | unset | coturn `static-auth-secret`. The node mints time-limited TURN credentials under it. Without it a `turn:` URL is served with no credential, and the node warns at startup. |
| `turn_credential_ttl` | `3600` | Seconds a minted TURN credential is valid. Cannot be `0`. |

`rate_limits` entries take `burst` (requests allowed at once) and `per_minute` (refill
rate). `0` in either turns that limit off. A refusal is `429` with `Retry-After`.

| Limit | Applies to | Keyed by | Default |
|---|---|---|---|
| `open` | No credential, an unknown endpoint, an unresolved token | source address | `{ burst: 30, per_minute: 30 }` |
| `login_source` | The login, on top of `open` | source address | `{ burst: 10, per_minute: 5 }` |
| `login_user` | The login, on top of `open` | username | `{ burst: 5, per_minute: 1 }` |
| `session` | A signed-in caller | session | `{ burst: 60, per_minute: 300 }` |

Limits are per node and in memory. Behind a reverse proxy every caller has the proxy's
address: turn `open` and `login_source` off and limit at the proxy.

### `http.files`

Enabled when the section is present.

| Key | Default | Meaning |
|---|---|---|
| `enable` | `true` | Serve static files from the admin listener. `/api/` is routed first. |
| `path` | unset | Document root: the built console |
| `spa` | `true` | Answer a `GET` or `HEAD` for a path with nothing behind it with `index.html`, so client-side routes survive a reload. Never applied under `/api/` or to a path with a file extension. |

## Not in the file

Realms, subscribers and users are provisioned over the admin API
([`api/openapi.yaml`](api/openapi.yaml)) and stored in the datastore, so a cluster shares
them.
