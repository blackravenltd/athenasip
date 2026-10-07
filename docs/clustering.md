# AthenaSIP - Running a cluster

Two or more nodes that serve the same subscribers, so that one can stop without taking
the service with it. Any node serves any subscriber. Terms are in the
[Glossary](glossary.md); the design is in [Architecture](architecture.md#clustering).

## What is shared and what is not

| Shared, through | What |
|---|---|
| The datastore (Redis) | Realms, subscribers, bindings, administrators and their sessions, Digest nonces, call records |
| The event bus (an MQTT broker) | Each node's status: whether it is up, its addresses, its inter-node listener. This is how nodes find each other. |

| Not shared | Why it matters |
|---|---|
| Connections | A phone's TCP, TLS or WebSocket connection is held by the node it registered through. A call for that phone arriving at another node is forwarded to the holding node over the inter-node listener. |
| Calls in progress | A call lives on the nodes it passed through. If one of them dies, the call goes with it. |

So a cluster needs `redis://` and `mqtt://`. With `memory://` or `local://` each node is on
its own.

## 1. Redis and a broker

Run one Redis and one MQTT broker (such as [Mosquitto](https://mosquitto.org/)) that every
node can reach. Each node needs to reach:

| Port | What |
|---|---|
| Redis, `6379/tcp` | The datastore |
| The broker, `1883/tcp` | The event bus |
| Every other node, `5062/tcp` | The inter-node listener (`cluster.port`) |
| Every other node's SIP UDP port | Only if nodes find their own address (below) |

## 2. Certificates

Nodes talk SIP to each other over mutual TLS, with a certificate authority the cluster
makes for itself. On one machine:

```sh
athenasip --ca-init
athenasip --ca-node node-a --san 10.0.0.11 --san node-a.example.com
athenasip --ca-node node-b --san 10.0.0.12 --san node-b.example.com
```

Copy `ca.crt` and each node's own `.crt` and `.key` to that node. `ca.key` stays where it
is. [Certificates](certificates.md) explains each step.

## 3. Configuration

Node A:

```yaml
sip:
  node_id: node-a              # unique in the cluster
  public_address: 10.0.0.11    # where clients and peers reach this node

udp:
  port: 5060
tcp:
  port: 5060

cluster:
  enable: true
  port: 5062
  advertise: 10.0.0.11         # must be one of the node's --san names
  ca: /etc/athenasip/cluster/ca.crt
  cert: /etc/athenasip/cluster/node-a.crt
  key: /etc/athenasip/cluster/node-a.key

datastore:
  url: "redis://10.0.0.5:6379/0"

events:
  url: "mqtt://10.0.0.5:1883"

http:
  port: 8080
  api:
    enable: true
```

Node B is the same with `node-b` in place of `node-a` and its own addresses. Everything
else, including `behaviour` and the media engine, should match, so a subscriber is treated
the same whichever node it reaches. With an rtpengine pool, list the same engines in the
same order on every node ([Media](media.md#more-than-one-engine)).

Make the first administrator once, on either node. With `redis://` it is written to the
shared store and the command exits:

```sh
athenasip --add-user you --role manage-admin-users --role manage-realms \
  --role manage-realm-subscribers --role view-cluster-status
```

Realms and subscribers made through one node's API are on both.

## 4. Check it

Start both nodes, then on each:

```sh
athenasip --check
```

```
ok    configuration        /etc/athenasip/config.yaml
ok    datastore            redis 0.0.1 at redis://10.0.0.5:6379/0
ok    events               mqtt 0.0.1 at mqtt://10.0.0.5:1883
ok    media                builtin 0.0.1 at builtin://
ok    cluster certificate  /etc/athenasip/cluster/node-a.crt
ok    peer node-b          10.0.0.12:5062, mutual TLS
ok    reached by peers     node-b reach this node's inter-node listener
ok    public address       10.0.0.11 (sip.public_address)
```

`peer node-b` is a real mutual-TLS handshake with node B. On the first node started, it
reads `no other node has said it is up on the event bus` instead, which is not a failure.
`GET /api/v1/nodes` lists the nodes each node knows, with their status.

[Troubleshooting](troubleshooting.md#cluster-nodes-do-not-see-each-other) covers each
`FAIL`.

## When a node dies

The broker publishes a `down` status for a node whose connection drops, and a node that
stops publishing for three `events.status_interval` periods counts as gone. Other nodes
stop forwarding to it.

Bindings survive in Redis, but the connections the dead node held do not. Its clients
have to register again through a node that is up. There are three ways to get them
there; use whichever your clients support:

- **DNS (RFC 3263).** Publish SRV records for the SIP domain naming every node. A client
  whose node stops answering tries the next one:

  ```
  _sip._udp.example.com. 300 IN SRV 10 50 5060 node-a.example.com.
  _sip._udp.example.com. 300 IN SRV 10 50 5060 node-b.example.com.
  ```

- **Outbound (RFC 5626).** A client that supports it keeps a registration through two
  nodes at once (two `reg-id`s). A call tries the newest first and moves to the other if
  that one has gone, so nothing waits for a re-registration.
- **`AthenaSIP-Alternate-Server`.** A client that sends `Supported: athenasip-failover`
  over TLS or WSS gets the other live nodes listed in the REGISTER's 200, and can move to
  one of them. See [Architecture](architecture.md#clustering).

A call that was up through the dead node loses its signalling: a BYE or re-INVITE has
nowhere to go. Its media does not depend on the node when rtpengine anchors it, so the call
carries on until a phone hangs up. With `builtin://` the media dies with the node. An
rtpengine pool keeps new calls going when an engine dies
([Media](media.md#more-than-one-engine)); calls already on that engine end with it.

Calls the dead node held stay in the datastore as they were. Once a node has gone, the live
node with the lowest `node_id` closes their records: at once for a call whose media was not
anchored, and for an anchored one when its engine says the media has stopped for
`sip.media_timeout` or no longer holds the call, releasing its ports. A node that restarts
closes its own calls from before the restart the same way. Nodes judge another gone only
after hearing the cluster for three `events.status_interval` periods.

## The node's own address

Clients and peers have to be told an address they can reach. The simplest and most
reliable is to set `sip.public_address` on every node, as above.

Left unset, a node in a cluster finds it for itself:

- It asks the `stun:` servers in `http.api.ice_servers` what address its UDP packets come
  from, every five minutes.
- Other nodes verify the answer by sending an OPTIONS to that address. A node advertises a
  discovered address only once another node has reached it; until then it logs
  `No other node has reached this node at <address>, so it is not advertised; set sip.public_address`.
- Every ten minutes each node also tries every other node's inter-node listener. A node
  that peers have tried and none could reach marks itself unreachable and is not
  forwarded to. It logs
  `No other node can reach this node's inter-node listener; it is marked unreachable and is not forwarded to`.

`athenasip --check` reports what STUN and the peers found.
[The node's own address](architecture.md#the-nodes-own-address) has the details.

## Try it on one machine

`test/e2e/cluster.sh` brings up two nodes, Redis, Mosquitto and the sipp test client in
Docker, makes the certificates with `--ca-init` and `--ca-node`, and calls across the
nodes:

```sh
test/e2e/cluster.sh           # every scenario
test/e2e/cluster.sh across    # only those whose name contains "across"
```

It needs Docker and builds the image first, which takes a while. Node A is `172.31.0.10`
and node B `172.31.0.11` on a network of their own; their configuration is in
`test/e2e/config-cluster/`. The last scenario kills node A, registers Bob through node B
and calls him. It prints the result of each scenario and tears everything down; the
nodes' logs are left in `test/e2e/results/cluster-node-a.log` and `cluster-node-b.log`.
