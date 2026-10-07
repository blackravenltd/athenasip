# AthenaSIP - End-to-end harness

sipp clients on real sockets against a node in Docker, asserting what the RFCs require.

```
test/e2e/run.sh                 # every scenario, one node, built-in relay
test/e2e/run.sh --rtpengine     # the same, with rtpengine on the media path
test/e2e/run.sh register        # only scenarios whose name contains "register"
test/e2e/cluster.sh             # two nodes, calls across them
test/e2e/cluster.sh across      # only cluster scenarios whose name contains "across"
```

`--rtpengine` must come before a name filter. It needs only Docker. The first run
builds the node image, including Boost from source; that layer is cached afterwards.

Logs land in `test/e2e/results/`: one per sipp end, plus the node's own (`node.log`,
`rtpengine.log`, `cluster-*.log`).

## One node

`docker-compose.test.yml` puts one node and two sipp containers on their own network,
with fixed addresses so a callee registers at an address the node can route back to.
`docker-compose.rtpengine.yml` is the overlay that adds rtpengine.

| | Address | |
|---|---|---|
| `athenasip` | 172.31.0.10 | the node, `memory://` and `local://` |
| `sipp-uas` | 172.31.0.20 | the callee |
| `sipp-uac` | 172.31.0.21 | the caller |
| `rtpengine` | 172.31.0.30 | with `--rtpengine` only |

The node starts with `--add-user`, which creates the administrator `e2e-admin`. `run.sh`
signs in as it and provisions the realm `example.com` and subscribers `alice`, `bob` and
`carol` over the admin API.

| Scenario | What it proves | Reference |
|---|---|---|
| `register` | A REGISTER is challenged, answered with Digest, and the binding comes back with its expiry and a Service-Route | RFC 3261 10, 22.4; RFC 3608 |
| `register-wrong-password` | A wrong password is challenged again and never accepted | RFC 3261 22.4 |
| `register-retransmit` | A retransmitted request is answered from the transaction, not handed to the registrar again | RFC 3261 17.2.2 |
| `relay-refused` | A stranger's INVITE to another domain is refused 403 and nothing leaves the node | RFC 3261 22.3 |
| `invite-bye` | A whole call: INVITE, 407 answered with credentials, 180, 200, ACK, BYE, with Record-Route bringing the BYE back through the node | RFC 3261 13, 16.6 |
| `cancel-ringing` | A CANCEL after 180 is answered 200 and the INVITE ends 487 | RFC 3261 9.1, 9.2 |
| `busy` | A 486 from the callee reaches the caller unchanged | RFC 3261 21.4.24 |
| `media` | Both descriptions point at the node's relay and RTP flows through it | RFC 8866 5.7 |
| `media-relayed` | The engine's counters show the `media` call was relayed, not declined | |
| `delayed-offer` | An INVITE with no body: the offer comes in the 200 and the answer in the ACK, and the node anchors both | RFC 3261 13.2.1; RFC 3264 5 |
| `hold-resume` | Two re-INVITEs in the dialog: hold offers `sendonly` and is answered `recvonly`, resume offers `sendrecv`, and the direction survives the relay | RFC 3264 8.4; RFC 6337 5.3; RFC 3261 12.2.1.1 |
| `invite-timeout` | An unanswered INVITE is retransmitted, gives up at timer B, and the caller is told 408 | RFC 3261 17.1.1.2 |

`media-relayed` exists because a call completes whether or not the engine anchored it: a
declined description travels on untouched and the two ends reach each other directly.
It reads `athenasip_media_packets_relayed_total` from `/metrics` for the built-in relay
and rtpengine's log for rtpengine.

`invite-timeout` takes 32 seconds (timer B is 64*T1, T1 is 500ms) and runs last.

## Two nodes

`cluster.sh` uses `docker-compose.cluster.yml`: two nodes sharing one Redis and one
Mosquitto, joined with certificates from `--ca-init` and `--ca-node`
(written to `test/e2e/generated-cluster/`).

| | Address |
|---|---|
| Redis | 172.31.0.2 |
| Mosquitto | 172.31.0.3 |
| Node A | 172.31.0.10 |
| Node B | 172.31.0.11 |

| Scenario | What it proves |
|---|---|
| `one-node-invite-bye` | The control: both ends on node A |
| `across-invite-bye`, `across-reversed-invite-bye` | A call to a subscriber the other node holds, in each direction |
| `across-media`, `across-media-anchored-once` | Media crosses, through the first node's relay and not the second's |
| `across-cancel-ringing`, `across-busy`, `across-delayed-offer`, `across-hold-resume`, `across-invite-timeout` | The single-node scenarios, across the two nodes |
| `one-node-tcp-flow`, `across-tcp-flow`, `one-node-ws-flow`, `across-ws-flow` | A callee reachable only down the TCP or WebSocket connection it registered on (`flow_callee.py`) |
| `across-call-records` | Both nodes list the same call records, and a call across them names both |
| `failover` | Last, because it kills node A: the callee registered through node A registers again through node B, and a new call reaches it within a minute |

## Adding a scenario

A scenario that needs a callee is a pair: a `uas_*.xml` that answers in a particular
way, and a caller scenario that asserts on what comes back. Add a `run_one` or
`run_pair` line to `run.sh`; the callee is registered and deregistered for you.

Each end reads its realm, user, password and the user it dials from a CSV in this
directory. A new subscriber needs a CSV and a line in `run.sh`'s provisioning.

Assert on what the RFC requires, not on what the node currently does. An `ereg` with
`check_it="true"` fails the scenario when the pattern is missing.

Give every sipp run its own source port (`allocate_port`). sipp derives its branch from
the call number, so two runs from one port look like a retransmission of one transaction
(RFC 3261 17.2.3).
