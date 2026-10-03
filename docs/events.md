# AthenaSIP - Event Topics

AthenaSIP publishes what it is doing to an event system. The built-in `local://` driver
keeps events inside the process; the canonical `mqtt://` driver publishes them to a
broker, where they are also how nodes discover each other.

Events are for observability, presence and discovery. They are never on the call setup
path: nodes route calls to each other over SIP, not over the event bus.

## Shape

Topics are hierarchical and use `/` as the only separator, matching MQTT. They follow
three rules:

1. **No topic begins with `/`.** A leading slash is a distinct, empty first level in
   MQTT, so `/nodes/x` and `nodes/x` are different topics and a `nodes/#` filter does
   not match the first.
2. **The topic says what the thing is, the payload says what happened.** A channel
   opening and closing publish to the same topic with different payloads.
3. **No dots.** `calls/<id>/unregister`, never `call.unregister`.

Every topic is built by `src/events/topics.h`, so the scheme is defined once. Nothing
should assemble a topic from string literals at the call site.

4. **The bus is observability, never signalling.** No SIP request or response travels
   over it, and nothing on the call setup path waits for it. An `account/<uri>/invite`
   topic once carried INVITEs between nodes; it is gone, and requests reach other nodes
   by being proxied to them.

## Topics

| Topic | Published when | Retained | Payload |
| --- | --- | --- | --- |
| `nodes/<node_id>/status` | Every `events.status_interval` seconds, on stop, and by the broker if the node dies | yes | see below |
| `nodes/<node_id>/channels/<transport>/<endpoint>` | A channel opens or closes | no | `{"status":"registered","at":"<zulu>"}` or `{"status":"closed","at":"<zulu>"}` |
| `nodes/<node_id>/transactions/<transaction_id>` | A transaction is registered or unregistered | no | `registered` or `unregistered` |
| `subscribers/<uri>/status` | A subscriber registers | no | `{"contact":"<uri>","node":"<node_id>","registered":"<zulu>"}` |
| `calls/<call_id>/register` | A call is created | no | The call id |
| `calls/<call_id>/unregister` | A call ends | no | The call id |
| `calls/<call_id>/state` | A dialog changes state | no | The state |

`<transport>` is one of `udp`, `tcp`, `tls`, `ws`, `wss`. `<endpoint>` is `host:port`.

Only the node status is retained, which is what makes it the one topic a monitor can ask
rather than wait for. Everything else is an event: it says something happened, and a
consumer that was not subscribed at the time has missed it.

### The node status payload

```json
{"status":"ok","node":"corvus-fi-1","version":"0.7.0","datastore":"redis 0.0.1","at":"2026-09-27T09:58:59Z","uptime":188790,"status_interval":30,
 "transports":[{"transport":"tls","address":"10.35.1.20","port":5061,"uri":"sips:10.35.1.20:5061;transport=tls"}]}
```

| | |
|---|---|
| `status` | `ok`, `degraded`, `stopped` or `down` |
| `node` | `sip.node_id`, which is required configuration and is not derived from the hostname |
| `version` | the server version |
| `datastore` | the driver and its version, or `none` |
| `at` | when the node composed the message, not when it arrived |
| `uptime` | seconds since this process started serving, `0` in a will |
| `status_interval` | `events.status_interval`: seconds until this node says it again, `0` if it never repeats; present in a will too |
| `transports` | where the node listens, one entry per enabled SIP transport, at `sip.public_address` when set; empty in a will |
| `media` | `{"engine":..,"capabilities":[..],"produces":[..]}`: the media engine, what it can do (`bridge`, `conference`, `record`, `transcode`) and the profiles it can produce (`rtp`, `webrtc`, `srtp`); `null` with no engine; absent in a will |
| `cluster` | `{"address":..,"port":..}`: where a peer node reaches this one's inter-node listener (`cluster.advertise`); absent on a node not in a cluster, and in a will |

`degraded` is a node that is running with a datastore it cannot reach: it cannot read a
registration, so calling that `ok` would be the most misleading thing this node says.
`stopped` is published on the way down by a node that got to say goodbye. `down` is the
will, published by the broker on this node's behalf when it did not - so `at` in a `down`
is the time the *will was composed*, at startup, and a consumer must date it by receipt.

Every node subscribes to `nodes/+/status`, so each knows the cluster as the others
describe themselves, and `GET /api/v1/nodes` lists it. A report not repeated within three
status intervals is listed as stale. A monitor elsewhere should do the same with the
`status_interval` the node carries, rather than a threshold of its own that agrees with it
only by coincidence.

A consumer should ignore fields it does not know, and the live registries
(Milestone 5) may add counts. Nothing already here is planned to change meaning or go
away.

### A channel is not a registration

The `channels` topics say "registered", and it does not mean what REGISTER means. A
channel is a transport flow this node is holding - a TCP, TLS, WS or WSS connection, or,
for UDP, the peer address a datagram arrived from - and "registered" means it has been
entered in the node's channel registry. Nothing about it says anybody authenticated.

So counting them is not counting endpoints, in either direction:

- A flow appears as soon as something connects or sends a packet, whether or not it ever
  sends a REGISTER, gets past a Digest challenge, or belongs to a subscriber this node has
  heard of. A port scanner makes them.
- One flow can carry several registrations, and a registration outlives its flow: a
  binding lives in the datastore with its own expiry, so a UDP phone has a registration
  and usually no live flow at all, and a TCP client that reconnects has two flows and one
  registration.
- A UDP flow is created on the first datagram from an address. It is forgotten after
  `sip.flow_idle_timeout` seconds without traffic (five minutes by default), and publishes
  `closed` when it goes, so the two topics do balance. Forgetting one costs a peer nothing:
  its next datagram makes a new flow, and a request for it - a call to its binding, or a
  BYE in a dialog it is on - opens a new flow to the address it was last heard from, which
  publishes "registered" again.

What answers "who is registered here" is `GET /api/v1/registrations`, which reads the
bindings, or the `subscribers/+/status` events as they happen.

## Subscribing

Standard MQTT wildcards apply: `+` matches exactly one level, `#` matches the rest and
must be the last level.

| Filter | Matches |
| --- | --- |
| `nodes/#` | Everything every node publishes |
| `nodes/+/status` | Node up and down events, for discovery |
| `nodes/sip-0001/#` | Everything one node publishes |
| `calls/+/unregister` | Every call ending, for CDR |
| `subscribers/+/status` | Every registration, for presence |

Subscribe to the narrowest filter that does the job. A trailing `#` under a prefix also
matches that prefix's other topics, so a consumer expecting one payload shape will be
handed others.

## Prefix

The MQTT driver prefixes every topic with the `prefix` from its URL, so one broker can
carry several clusters:

```
events:
  url: "mqtt://127.0.0.1:1883/?client_id=sip-01&keep_alive=30&prefix=athenasip/"
```

The prefix is applied on publish and subscribe, and stripped on delivery, so the topics
above are what the application sees either way. It is a topic level, so the separator
is added when it is left off: `athenasip` and `athenasip/` mean the same thing.

The settings can also go in an `events.mqtt` section, which is the form
`config/config.example.yaml` shows and which wins where both are given.

## Client identifiers

MQTT requires a client identifier to be unique on the broker, and a broker that sees a
second connection using one it already has disconnects the first. Two nodes sharing one
would trade the connection back and forth for as long as both were running, and the bus
would carry nothing.

So there is no shared default. Unset, the identifier is this node's `sip.node_id`,
which is unique across the cluster by definition; `events.mqtt.client_id` overrides it
where something else is needed.

## Connecting

The client queues what it is given and reconnects on its own, so starting it says
nothing about whether the broker is there. `connect()` therefore answers on a round
trip to the broker rather than on having started, bounded by
`events.mqtt.connect_timeout_ms` (5000 by default). A node whose broker address is
wrong says so at startup rather than running with a bus that carries nothing.
