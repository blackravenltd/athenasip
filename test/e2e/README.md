# AthenaSIP - End-to-end harness

Principle 2 of the plan says compliance is proven by a sipp harness rather than
asserted. The unit tests say the code does what the RFC says; this says a real client on
a real socket agrees.

## Running it

```
test/e2e/run.sh              # every scenario
test/e2e/run.sh register     # only the scenarios whose name contains "register"
```

It needs Docker and nothing else. The first run builds the node image, which builds
Boost from source because Debian ships 1.83 and this needs 1.87 or newer; that layer is
cached afterwards and the rest takes seconds.

Logs from each scenario land in `test/e2e/results/`, one per end.

## What it brings up

`docker-compose.test.yml` puts one node and two sipp containers on a network of their
own, with fixed addresses because a callee has to register at an address the node can
route back to:

| | address | what it is |
|---|---|---|
| `athenasip` | 172.31.0.10 | one node, `memory://` and `local://` |
| `sipp-uas` | 172.31.0.20 | the callee |
| `sipp-uac` | 172.31.0.21 | the caller |

The node uses the in-process datastore and event system on purpose. Nothing here is
testing Redis or a broker, and a harness that needs them is a harness that fails for
reasons that have nothing to do with SIP.

`run.sh` provisions the realm and three accounts over the admin API before it makes a
call, which is the same path an operator uses and a second check that the API works.

## The scenarios

| Scenario | What it proves | Where it comes from |
|---|---|---|
| `register` | A REGISTER is challenged, answered with Digest, and the binding comes back with its expiry and a Service-Route | RFC 3261 s10, 22.4; RFC 3608 |
| `register-wrong-password` | A wrong password is challenged again and never accepted | RFC 3261 22.4 |
| `register-retransmit` | The same request twice is answered from the transaction, not handed to the registrar again | RFC 3261 17.2.2 |
| `relay-refused` | A stranger's INVITE to a number on another network is refused 403 and nothing leaves the node | RFC 3261 22.3; the 2026-10-01 decision |
| `invite-bye` | A whole call: INVITE, a 407 answered with credentials, 180, 200, ACK, BYE, and a Record-Route that brings the BYE back through the node | RFC 3261 s13, 16.6 |
| `cancel-ringing` | A CANCEL after 180 is answered 200 and the INVITE it cancelled ends 487 | RFC 3261 9.1, 9.2 |
| `busy` | A 486 from the callee reaches the caller unchanged | RFC 3261 21.4.24 |
| `media` | Both descriptions point at the node's relay rather than at each other, and RTP flows through it | RFC 8866 5.7 |
| `delayed-offer` | An INVITE with no body: the offer comes in the 200 and the answer in the ACK, and the node anchors both, so neither end learns the other's address | RFC 3261 13.2.1; RFC 3264 s5 |
| `hold-resume` | Two re-INVITEs inside the dialog, by its route set: hold offers `sendonly` and is answered `recvonly`, resume offers `sendrecv`, and the direction survives the relay each way | RFC 3264 8.4; RFC 6337 5.3; RFC 3261 12.2.1.1 |
| `invite-timeout` | An INVITE nothing answers is retransmitted and gives up at timer B, and the caller is told 408 | RFC 3261 17.1.1.2 |

`invite-timeout` takes 32 seconds by design: timer B is 64*T1 and T1 is 500ms. It runs
last for that reason.

## Adding one

A scenario is a pair when it needs a callee: a `uas_*.xml` that registers and then
answers in a particular way, and a caller that asserts on what comes back. Both take the
realm, user and password from a CSV in this directory, so an account is added by adding
a row and a line in `run.sh`'s provisioning.

Assert on what the RFC requires, not on what the node currently does. An `ereg` with
`check_it="true"` fails the scenario when the pattern is missing, which is how a
scenario says "this header has to be here" rather than merely logging that it was not.
