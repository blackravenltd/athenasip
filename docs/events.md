# AthenaSIP - Events

A node publishes what it is doing to its event system: `local://` keeps events in the
process, `mqtt://` publishes them to a broker, where they are also how nodes discover each
other. Configuration is under [`events`](configuration.md#events).

Events carry observability, presence and discovery. No SIP message travels over the bus
and nothing on the call path waits for it.

## Topic rules

- Levels are separated by `/`, as in MQTT. No topic begins with `/` and none contains dots.
- The topic names the thing; the payload says what happened to it.
- Every topic is built by `src/events/topics.h`. Do not assemble one from string literals.

## Topics

| Topic | Published when | Retained | Payload |
|---|---|---|---|
| `nodes/<node_id>/status` | Every `events.status_interval` seconds, on stop, and by the broker if the node dies | yes | [Node status](#node-status) |
| `nodes/<node_id>/channels/<transport>/<endpoint>` | A channel opens or closes | no | `{"status":"registered","at":"<zulu>"}` or `{"status":"closed","at":"<zulu>"}` |
| `nodes/<node_id>/transactions/<transaction_id>` | A transaction is created or destroyed | no | `registered` or `unregistered` |
| `subscribers/<uri>/status` | A subscriber registers | no | `{"contact":"<uri>","node":"<node_id>","registered":"<zulu>"}` |
| `calls/<call_id>/register` | A call is created | no | The call id |
| `calls/<call_id>/state` | A call changes state | no | The state |
| `calls/<call_id>/unregister` | A call ends | no | The call id |

`<transport>` is `udp`, `tcp`, `tls`, `ws` or `wss`. `<endpoint>` is `host:port`.

Only the node status is retained. A consumer that was not subscribed when any other event
was published has missed it.

## Node status

```json
{"status":"ok","node":"sip-0001","version":"0.8.0","datastore":"redis 0.0.1","at":"2026-09-27T09:58:59Z","uptime":188790,"status_interval":30,
 "transports":[{"transport":"tls","address":"10.35.1.20","port":5061,"uri":"sips:10.35.1.20:5061;transport=tls"}],
 "media":{"engine":"builtin 0.0.1","capabilities":["bridge"],"produces":["rtp"]},
 "cluster":{"address":"10.35.1.20","port":5062}}
```

| Field | |
|---|---|
| `status` | `ok`; `degraded` (running, but the datastore is unreachable); `stopped` (published by the node on shutdown); `down` (the MQTT will, published by the broker when the node vanished) |
| `node` | `sip.node_id` |
| `version` | The server version, as `athenasip --version` prints it |
| `datastore` | Driver name and version, or `none` |
| `at` | When the node composed the message. In a `down` that is startup, so date a `down` by receipt. |
| `uptime` | Seconds since the node started serving; `0` in a will |
| `status_interval` | `events.status_interval`: seconds until the next report, `0` if it never repeats |
| `transports` | One entry per enabled SIP listener, at `sip.public_address` when set; empty in a will |
| `media` | Engine name and version, its capabilities (`bridge`, `conference`, `record`, `transcode`) and the profiles it produces (`rtp`, `webrtc`, `srtp`); `null` with no engine; absent in a will |
| `cluster` | Where peers reach the inter-node listener; absent outside a cluster and in a will |

Every node subscribes to `nodes/+/status`, and `GET /api/v1/nodes` lists what it has
heard. A report not repeated within three status intervals is stale; a monitor should
apply the same rule using the `status_interval` in the message.

Consumers must ignore fields they do not know.

## A channel is not a registration

On the `channels` topics, `registered` means a transport flow entered the node's channel
registry: a TCP, TLS or WebSocket connection, or a UDP peer address. It says nothing about
authentication.

- A flow appears as soon as anything connects or sends a datagram.
- One flow can carry several registrations, and a binding outlives its flow.
- A UDP flow is forgotten after `sip.flow_idle_timeout` seconds idle and publishes
  `closed`; the peer's next datagram publishes `registered` again.

For who is registered, use `GET /api/v1/registrations` or `subscribers/+/status`.

## Subscribing

MQTT wildcards apply: `+` matches one level; `#` matches the rest and must come last.

| Filter | Matches |
|---|---|
| `nodes/+/status` | Node status, for discovery and monitoring |
| `nodes/sip-0001/#` | Everything one node publishes |
| `subscribers/+/status` | Every registration |
| `calls/+/unregister` | Every call ending |

Use the narrowest filter that works: `#` under a prefix delivers every payload shape below
it.

## MQTT

```yaml
events:
  url: "mqtt://user:password@127.0.0.1:1883"
  mqtt:
    prefix: "athenasip/"
```

| Topic | Detail |
|---|---|
| Prefix | Prepended on publish and subscribe and stripped on delivery, so one broker can carry several clusters. A missing trailing `/` is added. |
| Client identifier | Must be unique on the broker, or the broker disconnects the older connection. Defaults to `athenasip-<node_id>`. |
| Startup | The node waits up to `connect_timeout_ms` for the broker and does not start without it. After that the client reconnects on its own. |
| TLS | `mqtts://` is not implemented. |

`client_id`, `keep_alive`, `connect_timeout_ms` and `prefix` are also accepted as URL query
parameters; the `events.mqtt` section wins where both are given.
