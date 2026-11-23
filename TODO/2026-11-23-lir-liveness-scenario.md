# The liveness pattern AthenaSIP relies on, as a scenario for Lir

2026-11-23, for the stack's part A (`TODO/2026-11-20-stack-integration.md`). Lir asked for the
pattern written as a scenario it can test: a retained status plus a will on the same topic, a node
dying, a split and a heal. Taken from the `mqtt://` driver and `Core` at `0.13.2`. What AthenaSIP
needs of the broker is marked **Needs**.

## The pattern

- Topic: `athenasip/nodes/<node-id>/status` (`mqtt.prefix` defaults to `athenasip/`). One topic per
  node; the node's ID is unique in the cluster.
- At connect (MQTT 5, `clean_start`, keep-alive 30 s by default) each node sets a **will** on its
  topic: QoS 0, **retained**, payload a JSON status with `"status":"down"`, its version and
  `status_interval`, and no inter-node listener.
- Once connected it publishes its status on the same topic, QoS 0, **retained**, with
  `"status":"ok"` (or `"degraded"` when its datastore is down), its listeners, its inter-node
  listener and its policy, every `events.status_interval` (30 s by default).
- On a clean stop it publishes `"status":"stopped"`, retained, then disconnects normally, so the
  will is not sent.
- Every node subscribes to `athenasip/nodes/+/status`, QoS 0, retain handling "send retained
  messages at subscribe". From what it hears it keeps a directory of nodes; a node is forwarded to
  only while its last status is `ok` and not older than three status intervals.
- Nothing on the bus is storage: a node's state is in the datastore. The bus says who is up.

## The scenario

Three nodes A, B and C, each connected to a different node of a Lir cluster where Lir has more
than one node, as a client would be after a load balancer.

1. **Start.** A, B and C connect, each with its will, and publish `ok` retained.
   **Needs:** each subscriber receives every other node's retained `ok` at subscribe time,
   whichever Lir node it is connected to, and each later publish within a second or two.
2. **A late subscriber.** D connects after the others have published.
   **Needs:** D receives the three retained `ok` messages on subscribe, from whichever Lir node it
   reached, without anyone publishing again.
3. **A clean stop.** B publishes `stopped` retained and disconnects normally.
   **Needs:** the retained message is `stopped` everywhere, and B's will is not published (MQTT 5
   3.1.2.5: a normal disconnect, reason 0x00, discards it).
4. **A node dies.** C's process is killed, or its network cut, without a DISCONNECT.
   **Needs:** after the keep-alive lapses (1.5 x 30 s), C's will, `down`, is published, retained,
   to every subscriber on every Lir node, and replaces C's retained `ok`. This is AthenaSIP's
   node-down signal: how long it takes is what to measure.
5. **The Lir node a client is on dies.** The Lir node that A is connected to fails.
   **Needs:** A's session there ends as a network failure would, so A's will is published (the
   other nodes see A `down`) unless A reconnects to another Lir node first. When A reconnects (it
   retries with backoff) and publishes `ok`, the retained message becomes `ok` again. What
   AthenaSIP cannot use: A's will published while A is in fact connected and publishing through
   another Lir node, with the will arriving after A's new `ok` and overwriting it.
6. **Split.** The Lir cluster divides into two sides, A and B on one, C on the other.
   **Needs:** each side keeps delivering within itself. Any retained message either side shows is
   one some node really published, never stale from before a later publish on the same side, and
   a will is published only for a client that really lost its session.
7. **Heal.** The sides join again.
   **Needs:** for each node's topic, the retained message every subscriber sees, and every later
   subscriber gets, is the last one published or will-published for that topic, by Lir's order,
   on whichever side. A node that published `ok` after an old `down` must end `ok`; one whose
   `down` came after its last `ok` must end `down`.

## What AthenaSIP does not need

- QoS 1 or 2 on the status topic: it is republished every interval, and a lost one only delays the
  directory by one interval. (Lir's QoS 1 matters to AthenaSIP for nothing on this topic.)
- Persistent sessions: every connect is clean.
- Ordering between different nodes' topics.
- Shared subscriptions or wildcards beyond the single-level `+`.

## How AthenaSIP would test it

`ATHENA_TEST_MQTT_URL` pointed at a Lir cluster runs the driver's unit tests against it; the
cluster harness (`test/e2e/cluster.sh`) runs two nodes; steps 4 and 5 need a node and a Lir node
killed by hand, and the time from kill to `down` measured.
