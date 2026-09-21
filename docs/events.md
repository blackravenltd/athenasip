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

| Topic | Published when | Payload |
| --- | --- | --- |
| `nodes/<node_id>/status` | The node starts and stops | `{"started":"<zulu>"}` or `{"stopped":"<zulu>"}` |
| `nodes/<node_id>/channels/<transport>/<endpoint>` | A channel is registered or closed | `{"status":"registered","at":"<zulu>"}` or `{"status":"closed","at":"<zulu>"}` |
| `nodes/<node_id>/transactions/<transaction_id>` | A transaction is registered or unregistered | `registered` or `unregistered` |
| `account/<uri>/status` | An account registers | `{"contact":"<uri>","node":"<node_id>","registered":"<zulu>"}` |
| `calls/<call_id>/register` | A call is created | The call id |
| `calls/<call_id>/unregister` | A call ends | The call id |

`<transport>` is one of `udp`, `tcp`, `tls`, `ws`, `wss`. `<endpoint>` is `host:port`.

## Subscribing

Standard MQTT wildcards apply: `+` matches exactly one level, `#` matches the rest and
must be the last level.

| Filter | Matches |
| --- | --- |
| `nodes/#` | Everything every node publishes |
| `nodes/+/status` | Node up and down events, for discovery |
| `nodes/sip-0001/#` | Everything one node publishes |
| `calls/+/unregister` | Every call ending, for CDR |
| `account/+/status` | Every registration, for presence |

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
