# AthenaSIP - Active Work

Work happens on `develop`; `main` carries the last release, `0.13.0` (2026-11-20), tagged,
released on GitHub and in the apt repository for amd64 and arm64. Every release from `0.5.0`
on is tagged. Line numbers refer to the current tree; update them as files move.

**Resume here (2026-11-23, evening).** `0.13.2` is tagged and on node-b and node-c from the arm64
package (GitHub release and apt staging wait for the maintainer). Stack part A is done but for
`live.json`. Decided today (Decisions, 2026-11-23): in the stack, AthenaSIP uses Dagda and Dannan
in-process for storage and messaging, and every existing backend stays. So node-c does not join
node-b over cross-site Redis and MQTT: it waits, stopped, for the in-process drivers (Next, The
third node). Nine commits on `develop` wait for the maintainer's word to push.

**Before that (2026-11-22, evening).** `0.13.0` is out (`COMPLETED.md`, Releases). On `develop`
since: the leaks at exit are gone and LeakSanitizer is back on in `tools/ci.sh asan`; two of them
leaked in a running node too (every store lookup from a script, and the registrations and trunks
listings). `websocket.tls` takes the `tls` section's certificate, the rtpengine control socket is
bound, and the workflow actions are bumped with Dependabot pointed at `develop` (`COMPLETED.md`,
After 0.13.0). Next is the harnesses on the build cluster (Next, First after 0.13.0). Every build
and test run is on the build cluster, never on the maintainer's Mac (the maintainer, 2026-11-22).
What is waiting on the maintainer is the first list below; read it before anything else.

The stack: AthenaSIP is one of the Cluster Stack's projects (Decisions, 2026-11-20), and
`TODO/2026-11-20-stack-integration.md` is the plan for what it takes from Dannan, Dagda, Lir,
Mimir and Lugh. Dannan `0.11.1` is the release to pin (2026-11-23: Dagda `0.4.0` needs `0.11.1` or
later, and the in-process datastore and bus will use Dagda); the datastore and the bus wait on Dagda's
leases (D5), register map with `walk_from` (D6) and log (D9).

Working rules from the stack (2026-11-20, the maintainer through the coordinating session):
heavy builds and test runs go to the shared build cluster by the claim board, claimed only for
a job ready to run and released when it ends, work other projects wait on first; versions are
written in full, `x.y.z`, Boost's too; on a dependency's new version, read its release notes and
look again at what of ours it replaces; refer to other sessions by role in anything tracked.

Milestones 1 to 6 are complete, presence and the review's perimeter work too (`COMPLETED.md`
has every item with what shipped). This file was rationalised against the tree on 2026-10-26,
and against the record and the docs on 2026-11-22, without re-checking the code. What is open is
in two lists: **Next**, which nothing blocks, in the order to work it, and **Waiting on
something**, each item with what it waits on.

**Two live nodes**, neither production, both still on `0.12.0` (deployed 2026-10-13, the binary
built on node-a and copied to node-b with the systemd unit; the previous binary kept as
`/usr/local/bin/athenasip.previous` on each; `verify.py` 15 of 15 after). Neither has `0.13.0`.
- `node-a` (10.101.1.20): `lua://` with `athenasip.trunks` from
  `/etc/athenasip/scripts/main.lua` (previous configuration `config.yaml.before-lua`), and a
  trunk to node-b, registering as node-b's subscriber 1003 through 10.102.1.50.
  Deploying there is standing-authorised; touch the files changed since the last deploy
  before building (a stale object crash-looped it on 2026-10-09).
- `node-b` on 10.102.1.20 (the maintainer reinstalled the host; AthenaSIP set up again on
  2026-11-01 at his request, `develop` at `1b1c3c5` built on the host): the realm
  `athenasip.com`, secure transports only (TLS 5061 and WSS 8089; nothing unencrypted), Redis and
  Mosquitto on loopback, rtpengine and coturn (3478, TLS 5349), the admin API and console on
  loopback behind the shared HAProxy at `https://athenasip.com` (Let's Encrypt, DNS-01 by hand).
  Each deploy there needs the maintainer's yes. The old node's trunk to the carrier, its
  subscribers and node-a's trunk to it are gone with the reinstall; `test/interop/live.json`
  still describes them (Next).
- A probe on node-a (`athenasip-probe.timer`) runs `test/interop/verify.py
  test/interop/live.json` every 15 minutes and publishes `athenasip/probes/node-a/calls`.
  A manual run at the same time as the probe fails some checks: both use subscribers 1901
  and 1902.
- TURN on both (2026-10-11): native coturn on 3478 UDP/TCP, relay 24000-24100, credentials
  minted by each node under `http.api.turn_shared_secret` and handed out by
  `/api/v1/subscriber/{realm}/config`. node-a's is reachable only where 10.101.1.20 is; node-b's
  from anywhere once its router forwards the ports (Waiting on the maintainer, 3).

**What passed for `0.13.0`** (2026-11-20, on an arm64 build machine, Debian 13): `tools/ci.sh`
`check`, `release`, `asan` (with UBSan, leak checking off), `tsan` (1644 tests each, with Redis and
a broker), `fuzz` (nine targets, 60 s each, nothing found), the Debian package with lintian; and
every job of `ci.yml` on GitHub, amd64 and arm64. **Not run since `0.12.0`:** the sipp, cluster
and trunk harnesses, the interop fixture and the suite with AthenaPhone. They need Docker Compose
and are being moved onto the build cluster (Next). The release notes say so.

**Once things are stable, video calling is a primary feature** (the maintainer, 2026-10-03): nothing
built may assume a call is audio only.

Items are in priority order: work top to bottom, and say why when something is taken out
of turn. Move items to `COMPLETED.md` as they land, with a note on what shipped.

## Waiting on the maintainer

Nothing below can move until these are settled. They are here rather than scattered through
the lists because a session that has lost its context needs to see them first.

1. **`athenasip-admin` on GitHub** still has its history from before the 2026-11-20 scrub, in
   public. Its code is in `console/`. The maintainer deletes the repository (decided 2026-11-22);
   until then it stays here.
2. **The router forwards for node-c**, when it is to serve: at node-c's site, to node-c, TLS 5061
   and WSS 8089, TURN 3478 udp and tcp and 5349, RTP 22000-22200/udp and relay 24000-24100/udp,
   and whatever port Dannan's listener takes for the cluster. No Redis or MQTT forwards: the
   cross-site plan is set aside (Decisions, 2026-11-23).
3. **node-b** (2026-11-01): the router forwards to 10.102.1.20 (TCP 5061, 8089, 5349; UDP and
   TCP 3478; UDP 22000-22200 and 24000-24100) and TCP 80 to 10.102.1.50, after which the node is
   tested from outside; PROXY protocol v2 (Next, Found setting up node-b); and the
   certificate's renewal, by hand with a TXT record or automated with Route 53 credentials. It
   expires 2027-01-30. `0.13.1` is to be deployed there and to node-c (the maintainer, 2026-11-22).
4. **GitHub housekeeping**: the `CODECOV_TOKEN` secret (until it exists the upload cannot fail the
   run; then `fail_ci_if_error: true` in `ci.yml`).
5. **What is left of the stack's decisions** (`TODO/2026-11-20-stack-integration.md`): whether
   admin users stay local beside Lugh or move to Lugh alone, and the Boost version (agreed
   with Dannan to try the system Boost with the Redis and MQTT clients as pinned sources).
6. **Review finding 30**: subscriber ids are FNV-1a-64 (`Util::stable_id`); a new id is a
   migration of every stored subscriber and binding key, so it waits for a decision on how.
7. **Releases before `0.13.0`**: GitHub releases for `0.10.0` to `0.12.0` (notes written); the
   Docker image and the Homebrew tap (`blackravenltd/homebrew-athenasip`) for any release; whether
   images go to Docker Hub at all. athenasip.org's "Where it stands" says presence is not built
   and AthenaPhone is iOS and Android (review finding 48); the site is outside the repo.
8. **The phone, when the maintainer lends it**: the browser call to AthenaPhone with
   `ATHENA_INTEROP_PAGE_URL=https://10.102.1.50:8443` (plain http gives the page no
   microphone), and the hold, DTMF and transfer checks under Waiting on something.
   `phone.py` taps only while AthenaPhone is in front; its wireless adb port is found by mDNS.

## How to prove it

`docs/testing.md` is the whole of it: the layers, what each answers that the one
before it cannot, and the exact commands. The short version, cheapest first:

```
tools/ci.sh check|release|asan|tsan|fuzz   the release gate, on a build machine (CI runs
                                 the same steps on GitHub for develop and main)
./build-tests/athenasip_tests    the unit suite, with Redis and MQTT set or they skip
test/e2e/run.sh [--rtpengine]    the sipp scenarios, the second with a real engine
test/e2e/cluster.sh              the same call across two nodes of a cluster
test/e2e/run.sh --lua            any harness on the standard Lua scripts
test/e2e/trunk.sh                trunks, with sipp as the carrier
test/suite/run.sh                everything that runs unattended, the siblings' specs too
test/interop/verify.py live.json both live nodes, every transport and the trunk
test/interop/phone.py            calls to and from AthenaPhone on the test phone, over adb
test/interop/up.sh --rtpengine   a node to point a real client at
test/interop/browser.sh          two browsers calling, direct then relayed through coturn
e2e/phone-call.spec.ts           (console/) a headless browser calls a real phone
                                 through a named node, with video; somebody answers it
test/interop/UAT.md              the call a person has to make
```

All of them pass before anything is tagged; `0.13.0` was the exception, gated without the
harnesses (What passed, above).

**The sanitizers by hand are for a release or an intractable bug** (the maintainer, 2026-09-30
and 2026-10-03), never routinely on a change. Hosted CI runs `asan` and `tsan` on every push to
`develop` and `main` since 2026-11-20, which is the stack's rule, not a reason to run them locally.

What replaces them is the thing they were standing in for: reason about lifetimes and
threading directly, and exercise the change against a running node. Nearly every bug found
in the work since 0.7.0 came from running something rather than from a checker - a
connection the node addressed by a name that would never resolve, a UDP flow whose close
was silently skipped, a TURN username no browser could use.

## Principles

1. **Multi-master, clusterable, HA SIP server.** Any node serves any subscriber.
   Registrations survive a node dying. A dialog's state is in the datastore (2026-10-11),
   but its signalling does not move to another node yet (Parked).
   State lives in the datastore (the maintainer, 2026-10-11): the nodes share the datastore and the
   event system, and that is what makes them a cluster. A node holds in memory only caches
   of records, timers and its transport flows; the bus signals changes and is not storage.
2. **Standards first.** RFC 3261 in full (sections 16 and 17, timers A-K), plus 3263,
   3327, 3581, 4028, 5626, 7118, 8866, and for WebRTC/conferencing 4575, 4579, 8843.
   Compliance is proven by a sipp harness, not asserted.
3. **Easy to install, configure and use** without a telecoms background. Sane defaults,
   a ten-line config, one docker-compose, a CLI that generates the cluster CA and node
   certificates, and an admin UI that works out of the box. Every feature ships with
   the tooling and documentation that makes it easy, or it is not done.
4. **IoC / plugin architecture.** `Datastore`, `EventSystem`, `MediaEngine`, `PushService`
   and `Policy` are plugin kinds behind one registry keyed by (kind, URL scheme). Two implementations
   per kind: one built-in for zero-config single node, one canonical for production.
5. **Canonical backends:** Redis (datastore), MQTT (events), rtpengine (media), Lua
   (policy, beside the built-in default).
6. **Web video calling and conferencing are first class.** AthenaPhone is the client
   for now; a web client follows. Everything in the signalling and media model must
   already allow for it.

## Decisions

Dated, and not reopened without asking.

- (2026-11-23) **`redis://` gets a RESP client written by hand on Asio, in place of `boost::redis`**
  (the maintainer, on the Boost proof: `boost::redis` needs Asio from Boost 1.86, `boost::mqtt5`
  builds on Debian's 1.83). It drops a dependency and lets AthenaSIP build on Debian 13's Boost
  1.83 and the packaged libdannan, with only `boost::mqtt5` pinned over it, as the rest of the
  stack does (Dannan's option (a)). Dannan leans the same way; Mimir's RESP code is a reference
  for the parsing.

- (2026-11-23) **In the stack, AthenaSIP uses Dagda and Dannan and nothing else for storage and
  messaging, linked in-process** (the maintainer, directly). A `dagda://` datastore and a
  `dannan://` event system are plugins, in-process as `memory://` and `local://` are: each node
  is a member of a Dannan cluster, Dagda replicates the records among the nodes, and the nodes
  find each other over Dannan, with no broker and no external store. **AthenaSIP must remain
  usable with Redis and MQTT, or memory**: every existing backend stays, and a single node still
  needs nothing. This supersedes the 2026-11-20 route for the bus through Lir, for the stack's own
  deployments, and the idea of running AthenaSIP on Lir and Mimir on its nodes. Linking Dannan and
  Dagda is a new dependency, taken because the stack links rather than duplicates.
  `TODO/2026-11-20-stack-integration.md`, parts D and E.

- (2026-11-20) **AthenaSIP is part of the cluster stack** (the maintainer, confirmed in session), and
  takes its group rules: hosted CI kept green with badges, coverage and CodeQL (`tools/ci.sh`,
  `.github/`), which replaces the 2026-10-26 decision against hosted CI; Debian packages at
  each release by the stack's convention; heavy builds and tests on the shared build cluster;
  the console in this repository (`console/`). The datastore's route into the stack is a native
  `dagda://` driver, written as Dagda's register map, leases and log are released, with
  `redis://` until then; `memory://`, `redis://`, `local://` and `mqtt://` all stay.
  `TODO/2026-11-20-stack-integration.md` has the plan.
- (2026-10-26) **Conferencing's four questions** (`docs/design/conferencing.md`), the maintainer:
  1. Focuses are trunks. `/api/v1/focuses` is a view on the trunks that carry a `focus`
     attribute, not a record of its own.
  2. `builtin://` does not route conferences: the point of `lua://` is that this behaviour
     is configurable. Conferencing is the standard scripts' (`athenasip.standard`), and the
     node reads a trunk's `focus` attribute only for its API views, never to route.
  3. Mute state from the start: the node watches each room at its focus for RFC 4575
     `conference-info` (the design's step 6) as part of the feature, not an option.
  4. The prefix is the operator's; the quick start shows `conf-` and says a numeric prefix
     suits phones that cannot dial letters.

- (2026-10-12) **Every flavour of DTMF is handled** (the maintainer): RFC 4733 telephone-events,
  SIP INFO (`application/dtmf-relay` and `application/dtmf`) and in-band tones, passed
  through when both ends agree and converted when they differ. AthenaPhone does the same
  on its side; its session has been told.

- (2026-09-17) Nodes proxy SIP to each other (Path / Record-Route / Via). MQTT is for
  events, presence, discovery and observability, never on the call setup path.
- (2026-09-17) Inter-node authentication is mutual TLS from a cluster CA on a dedicated
  listener. Cluster-CA client cert = peer node (Route trust); anything else = endpoint
  or trunk (Digest).
- (2026-09-17) Cluster discovery: retained MQTT `nodes/<id>/status` messages carrying
  the node's SIP and inter-node TLS addresses; Redis holds registration ownership with
  TTL.
- (2026-09-17) MySQL, PostgreSQL and SQLite drivers are deleted. `memory` + `redis` for
  datastores, `local` + `mqtt` for events and `builtin` + `rtpengine` for media are what
  ship in-tree and are tested in v1. The architecture privileges none of them: every
  driver, in-tree or not, registers through the same plugin contract, and DynamoDB,
  NATS, Kafka or anything else is a plugin someone can write, not a roadmap item the
  core carries.
- (2026-09-18) The plugin contract is a stability promise. It is versioned, it is async
  from its first version because it cannot be made async later without breaking every
  plugin built against it, and a plugin receives its own YAML root plus read access to
  the system config. Plugins are compiled in for now and become shared libraries with
  autodiscovery later, through the same contract.
- (2026-09-17) rtpengine (ng protocol) is the flagship media engine. `RTPProxyClient`
  (rtpproxy text protocol) may remain as a driver but nothing is built on it.
- (2026-09-17) `MediaEngine` carries capabilities: `bridge`, `conference`, `record`,
  `transcode`. It is not a two-party SDP rewriter.
- (2026-09-17) `Call` is multi-party from the start (participants, optional conference
  focus).
- (2026-09-17) The transaction layer is the four RFC 3261 section 17 state machines.
  `is_reliable()` only disables timers A/E/G and shortens D/K.
- (2026-09-17) WSS is required (browsers need a secure origin); WS stays for local
  development.
- (2026-09-17) Conferencing is RFC 4579 focus routing. FreeSWITCH `conference` is the
  first focus; an SFU (Janus / mediasoup / LiveKit, or rtpengine publish/subscribe)
  comes later behind the same URI scheme.
- (2026-09-17) Lua scripting stays parked and out of the build path until there is a
  real use. When it returns it is a routing-policy plugin, not a core feature.
  **Superseded 2026-10-07 (the maintainer):** routing and authorisation are decisions a policy makes
  and the node carries out, and a script is how a deployment expresses them, because a
  configuration option for every case makes a configuration nobody can read and still
  cannot express the next case. `policy` is a plugin kind with five hooks (`authorize`,
  `route`, `on_failure`, `register`, `init`; presence added a sixth, `watch`, on
  2026-10-11, and `on_call_ended`, a notification, came on 2026-10-26). Two drivers ship and
  both stay: `builtin://`
  is today's behaviour in C++ and the default, so a plain node runs no script;
  `lua://` runs Lua 5.4 scripts, and the standard scripts reproduce `builtin://` exactly,
  which a fixture in the tree keeps checked. Trunks are records in the datastore plus
  scripts, never configuration. The SIP mechanics (transactions, Via, Record-Route, flow
  tokens, Digest verification, nonces, media anchoring, the RFC 3261 16.7 rules) stay
  native and cannot be reached from a script. Design: `docs/design/scripting-1-the-node.md`
  and `scripting-2-the-engine.md`.
- (2026-09-17) Core runs on a single strand. Servers post into it.
- (2026-09-20) Nodes can sit behind a load balancer, and the shape it forces is this. A
  binding shares; a flow does not: the Redis row is readable by any node, but the socket
  a TCP, TLS, WS or WSS client registered on lives on one node, so a node that reads a
  binding it does not own forwards to the node that does, which is what
  `Location.node_id` and `Location.flow_id` are for. Connection-oriented transports
  balance cleanly because the balancer pins a connection to one node; UDP does not,
  because an L4 balancer cannot see Call-ID, so front UDP with a SIP-aware dispatcher or
  do not balance it. TLS is passed through and never terminated at the balancer, because
  the cluster-CA peer certificate is what distinguishes a peer from an endpoint.
  Record-Route names the node, not the balancer, so in-dialog requests come straight
  back to the node that anchored the media, which is coherent with principle 1 and needs
  no dialog replication; every node therefore needs a client-reachable address of its
  own as well as the shared one. RFC 3263 SRV is the failover mechanism for SIP
  endpoints and needs no balancer; browsers cannot use it, so they get a balancer or a
  provisioned list from `GET /api/v1/subscriber/{realm}/config`.
- (2026-09-21) Client failover without infrastructure. A realm should survive a node
  dying without the operator running DNS they control, which is what principle 3 asks
  for and what RFC 3263 alone does not give. The answer is the standard mechanisms
  first and an optional extension for what they do not cover, in this order: RFC 3608
  Service-Route and `GET /api/v1/nodes` (both done), the node list fed from discovery,
  RFC 5626 outbound with two flows to two nodes, RFC 3263, and last the
  `AthenaSIP-Alternate-Server` header. The M4 items carry the detail.
- (2026-09-21, reversed 2026-10-03) The type for a subscriber was renamed `Account` in the
  code, because `Subscriber` would have collided with SUBSCRIBE (RFC 6665) the moment
  presence arrived. It is `Subscriber` again: see the 2026-10-03 decisions.
- (2026-09-25) The vocabulary, which every document, endpoint and role name follows: a
  **user** is a thing that can use the API, and the admin interface is only a client of
  the API; a **subscriber** is a thing registered on a realm to make and receive calls.
  Neither is created from the other in either direction and neither credential works as
  the other. The resource became `/realms/{realm}/subscribers` on 2026-10-03, and the
  type `Subscriber`.
- (2026-10-05, replacing the 2026-10-03 and 2026-10-04 decisions on browsers) A subscriber
  reaches its own API routes, all under `/api/v1/subscriber/{realm}/`, with HTTP Digest
  (RFC 7616) and its SIP credentials: the HTTP realm is the SIP realm, so the stored HA1 is
  the key. A softphone gets its ICE servers and a minted TURN credential there, so coturn
  stays in shared-secret mode (it cannot also take long-term users: tested 2026-10-05).
  `/client/config` moved there with no alias, and the console follows.
- (2026-10-04) nghttp2 is a dependency, for APNs, which speaks only HTTP/2.
- (2026-10-04) A node asks the `stun:` servers already in `http.api.ice_servers` for its own
  address, and no others: no public STUN server by default.
- (2026-10-04) A REGISTER for a domain no node serves is forwarded for this node's
  subscribers by default (`sip.forward_register`), over any transport.
- (2026-10-03) the maintainer's answers to the questions that were waiting on him:
  - The API says **subscriber** for a subscriber, everywhere and in one go:
    `/realms/{realm}/subscribers`, and `subscriber` / `subscriber_id` where a response
    said `account` / `account_id`. A subscriber belongs to a realm; a person who uses the
    admin interface is a user. No alias for the old path. The console followed in the
    same deploy.
  - **Deleting a realm deletes everything in it**: its subscribers and their
    registrations. It is in the `Datastore::realm_delete` contract, so every driver does
    it, not only the two in-tree.
  - **Every route is rate limited.** Public routes aggressively, authenticated routes
    appropriately.
  - **The admin interface does not need HTTPS yet**, unless it is really easy, in which
    case it is done to get it out of the way.
  - **No role is defined before it has routes to permit.** `manage-cluster` waits for M4
    and M5.
  - **`status_interval`** is in the node status payload, for monitors such as an external monitor to
    derive staleness from.
  - **Sanitizers are not routine** (see How to prove it).
  - **Terms are consistent everywhere, and `docs/glossary.md` is where they are defined.**
    The event topic is `subscribers/<uri>/status`, and logs, API messages, docs and
    scripts say subscriber. Nothing consumed the bus topic when it changed.
  - **Internal names match external names.** A **user** may sign in and observe and manage
    the cluster; a **subscriber** belongs to a realm and can REGISTER and make and receive
    calls. **Account is not anything**: the C++ type is `Subscriber`, the datastore
    operations are `subscriber_*` (contract version 14) and the Redis keys are
    `athena:subscriber:*`, which reverses the 2026-09-21 rule that the type is `Account`.
    The Redis schema is renamed and a store is recreated to match, not migrated. What
    reads the event bus is a consumer, so that subscriber means one thing.
- (2026-10-03) A cluster is a full mesh: every node reaches every other node's inter-node
  listener directly (the maintainer). A node forwards to the node that holds a flow in one hop, with
  no Route between them, and discovery carries no notion of which node can reach which.
  Nodes that cannot reach each other are not a cluster with a hole in it; they are two
  servers. A client only ever talks to the node it is connected to: the inter-node
  addresses are in the route set, and only a node dials them.
- (2026-09-25) There is no superuser role, and no role implies another.
- (2026-09-29) `Datastore::session_delete` succeeds whether or not that hash was held, and
  the realm and subscriber deletes still report when there was nothing there. A session is
  named by a secret the caller presented, so an answer distinguishing "that was live" from
  "that was never live" is a way to ask this node whether a token is real, one guess at a
  time, on a route anybody can reach. A realm name is not a secret. `API_VERSION` did not
  move, because the shape did not change - which is exactly why `docs/plugins.md` now says
  the version tracks shape and shape is not the whole contract.
- (2026-10-01) No configured API tokens of any scope, and no setup or bootstrap token.
  The first administrator, and recovery, is `athenasip --add-user` on the host and nothing
  else; on `memory://` that command creates the user in the node's own datastore and
  carries on serving. A file that sets `http.api.tokens` is refused at start. Automation
  signs in as a role-limited user, as `node-a`'s provisioning does.
- (2026-10-01) Behaviour that differs between SIP servers is a setting, not a choice made
  for the operator. Each lives in one plainly named per-realm section, with a server-wide
  default in the config. The shipped default is exactly what the standards say, and every
  setting can be set to reproduce Asterisk, Kamailio/OpenSIPS or FreeSWITCH. Established
  servers are the reference for least astonishment, for migrating operators and for their
  clients. AthenaSIP is built for every RFC-compliant client, never tuned to AthenaPhone
  or JsSIP, and where it adds value it does so visibly and never by silent learning. The
  first use is what a callee is offered (Milestone 3): transport default, a per-subscriber
  profile, one re-offer on 488 reported to the operator, then OPTIONS-advertised SDP.
- (2026-10-01) The proxy is not an open relay. A request out of dialog whose From is in a
  domain this node serves is authenticated as that subscriber, whoever it is calling: RFC
  3261 22.3, a 407 with Proxy-Authenticate, answered with Proxy-Authorization for that
  same subscriber, or a reliable connection that carried an authenticated REGISTER for it. A
  From elsewhere may call into this node's domains and nowhere else (403). Requests in a
  dialog this node is on, ACK and CANCEL are not challenged. Found because the proxy had
  no 407 at all and forwarded a stranger's INVITE anywhere. What a client needs to do
  about it is its own session's work.
- (2026-10-02) Contact rewriting is a behaviour setting, off by default (the maintainer). The NAT item
  had left it as a judgement call; it is `behaviour.rewrite_contact`, Asterisk's
  `rewrite_contact`, and WebSocket clients are never rewritten.
- (2026-10-02) The cluster CA is made by the node with flags, `--ca-init` and `--ca-node ID
  --san NAME`, matching the command line there is rather than the subcommands this plan
  first named.
- (2026-09-29) A route declared with an empty role set means **any authenticated caller**,
  not a public route; a route open to anyone says so with `Router::add_open`. The old
  `public_scope` was an empty string, so a route that forgot to name its scope was open to
  the world. Forgetting now gives "must be logged in and may do nothing", which is the
  direction a mistake should fail in.
- (2026-09-30) A test asserts facts about this node's own configuration, never a client's
  labels for them. The case that settled it: Chrome reports a relayed local candidate as
  `candidateType: "prflx"` once connectivity checks run, so the browser relay test asserts
  the local port is inside coturn's configured range instead. Agreed with the console
  session and written into `docs/testing.md`.
- (2026-09-23) The browser end of the Milestone 3 harness is the admin client's own
  softphone, served by this node's static middleware, rather than a minimal page kept
  here. One page, maintained where the client lives, and tested where it is written;
  the cost is that the harness mounts a build from `../athenasip-admin` rather than
  serving something checked in, which is preferred to a copied bundle going stale. The
  Playwright spec lives beside the page in that repository and this side brings the
  node, rtpengine and the subscribers up for it.
- (2026-10-11) **All state lives in the datastore** (the maintainer). The nodes share the datastore
  and the event system, and that is what makes them a cluster: it is part of the point of
  the design. So every fact a node keeps is a record through the `Datastore` plugin, in
  `memory://`, `redis://` or whatever driver is configured; a node's memory holds caches of
  records, timers, and its own transport flows; and the bus signals that something changed
  and is never storage, since it may drop a message (MQTT here is at most once) and a
  reader must be able to read the fact back. One fact, one record: a feature that needs a
  fact another record already holds extends that record rather than keeping a copy. The
  retained `nodes/<id>/status` of the discovery decision stays: it is liveness, carried by
  a will, and nothing reads it as a record of anything else. The items under "State into
  the datastore" in Milestone 7 bring the tree to this. Dialog state is included: the maintainer said yes
  the same day to putting it in the call record, which supersedes principle 1's v1 terms
  for dialogs and leaves only the takeover of a live dialog's signalling parked.
- (2026-10-11) Presence (the maintainer, answering three questions):
  - **The node is the notifier for the `presence` and `dialog` event packages** of the
    subscribers in its realms: it answers SUBSCRIBE (RFC 6665), accepts PUBLISH (RFC 3903),
    keeps published state in the datastore, and sends the NOTIFYs. Line state for BLF
    (RFC 4235) comes from the dialogs the node already tracks, so desk phones need no client
    support. A SUBSCRIBE for any other package is forwarded to the devices, as before.
  - **Watchers: the same realm by default.** Any subscriber may watch any other in its own
    realm; anyone else is refused 403. A policy hook lets a script decide otherwise, as
    routing does. No XCAP and no per-user rule documents.
  - **No message-waiting indication for now**: nothing stores voicemail. A
    `message-summary` SUBSCRIBE is forwarded like any other package, so a voicemail server
    behind a trunk can still answer it.
- (2026-10-06) The node sends a BYE to each end when it ends a call by its own policy
  (`sip.media_timeout`, `sip.max_call_duration`) or for an administrator
  (`DELETE /api/v1/calls/{call}`, `manage-cluster`), as Kamailio's dialog module does. It
  never does on an RFC 4028 lapse, after which a proxy "MUST NOT send a BYE" (8.3). The BYE
  is the one the far end would send, entering the proxy like any in-dialog request.


## Architecture

The layering follows RFC 3261's own: transport, transaction, transaction user. `Core`
is the composition root and owns the strand; it does not process SIP itself.

```
udp/tcp/tls/ws/wss servers -> Connection -> Channel     transport   s18, RFC 3581
                                              |
                                      Transaction layer   s17: matcher + 4 machines
                                              |
                    +-------------+-----------+-----------+
                Registrar       Proxy       Dialogs       UA        transaction users
               s10, 3327,      s16, 3263   s12, 4028     (BYE to
               5626 flows      loose route              both ends)
                    |             |
                Datastore    MediaEngine       Events: observability only, never the call path
                (async)

Core = composition root + the strand
```

Everything below the transaction users is a plugin: datastores, event systems, media
engines, push services and the policy. They hang off the TU layer
and register through one contract, so adding a kind or an implementation touches nothing
above it. Policy is the one kind the transaction users call upwards into: the proxy and
registrar ask it who a caller is and where a request goes, and act on the answer.

The tree matches the diagram, with the trunk registrar (`src/trunk_registrar.*`) beside
the registrar as a UAC of its own. The local UA (`src/local_ua.*`) is the fourth transaction
user: it sends a BYE to both ends of a call this node ends, through the proxy. The notifier
(`src/notifier.*`) is the fifth, for `presence` and `dialog`; the diagram in
`docs/architecture.md` shows all five. That document describes the shape for a reader;
this section is the target, and the two agree.

### Known deviations from the standards

Each is deliberate and each is either an item below or a setting, and this list is so that
none of them is mistaken for compliance. **Re-checked against the tree on 2026-10-03:**

- RFC 3263: no blacklisting of a server that failed (4.3 permits it), so the next call
  tries it again first.
- RFC 5626: no `Flow-Timer`, by choice - the one value this node could give is longer than
  most NATs keep a UDP mapping. No 430 or 403 for an in-dialog request whose flow token
  names a flow that has gone, also by choice: the token is in every dialog's Record-Route,
  outbound or not, and today that request still reaches a desk phone through its Contact.
  No flow token in a Path this node writes as an edge for another registrar; M4.
- Forking is serial. 16.7 allows it, and parallel forking is Parked.
- A media-anchoring node rewrites the body it forwards, which 16.6 forbids a proxy, and
  re-offers the other profile once on a 488. It is the relay; `behaviour.media_anchor:
  false` turns all of it off, and where it cannot anchor the message travels on untouched.
- With `behaviour.rewrite_contact` on (off by default) the Contact an endpoint wrote is
  replaced with the address its message came from, as Asterisk and Kamailio do.
- Outbound TLS is to cluster peers, with the cluster's certificates, and to trunks,
  verified against the trunk's CA or the system's store. To any other host, such as one
  an arbitrary Request-URI names, it is refused rather than faked.
- Record-Route is written twice on every dialog-forming request rather than only where
  the interfaces differ. RFC 5658 requires the pair in that case and permits it in
  every case; the flow token in each value is what makes a call to a browser routable
  in both directions, so both are always written.

---

## Next

Nothing here waits on a device, a carrier, the maintainer or another repository. In order.

### First after 0.13.0

- [ ] **The sipp, cluster and trunk harnesses on the build cluster** (`test/e2e/`), running
      AthenaSIP nodes and sipp directly on claimed machines, not under Docker Compose (the
      maintainer, 2026-11-20: "They should use the cluster as the other projects do"). Docker is
      there for a single container (rtpengine, a broker). A node dropping is normal there. Then
      everything changed since `0.12.0` gets its sipp run: the CANCEL scenarios' 487 CSeq, the
      trunk harness against `trunks/changed` and the OPTIONS answer.
- [ ] **Sanitizer runs build what they link with the same sanitizer** (the maintainer, 2026-11-20):
      the three static Boost libraries (thread, json, charconv) are built plain today, which is why
      UBSan's `vptr` check on Boost.JSON is suppressed (`docs/testing.md`). `tools/ci.sh deps` builds
      an instrumented copy for `asan` and one for `tsan`, and the suppressions go. Dannan and Dagda,
      when linked, are built from source with `DANNAN_SANITIZE=address` or `thread`, set before
      `FetchContent_MakeAvailable(dannan)`.

### The third node (2026-11-22)

node-c is a second machine, at a second site, that already runs another of the maintainer's
services. It joins node-b's cluster (the maintainer, 2026-11-22). Nothing is compiled on it, or on
node-b: both install the arm64 package built on the build cluster. The other service's session is
told before either machine is touched.

- [x] `0.13.2` on node-b and node-c from the package (2026-11-23); node-b's build from source kept
      to roll back to. node-c has AthenaSIP, rtpengine and coturn installed, stopped and disabled.
- [x] node-b: the cluster CA made on it, a certificate for each node installed, its inter-node
      listener up; Redis capped at 256 MB. Redis and the broker over TLS for node-c were set up
      and then taken out again the same day, when the maintainer chose the in-process route
      (Decisions, 2026-11-23): both are on loopback only, as before.
- [x] node-c prepared (2026-11-23): its own name `inverbeg.athenasip.com`, kept current by the
      other service's DNS updater; a Let's Encrypt certificate by DNS-01 (hooks in
      `/etc/letsencrypt/athenasip/`); rtpengine and coturn configured as node-b's. AthenaSIP,
      rtpengine and coturn all stopped and disabled; its configuration holds placeholders for the
      datastore and bus.
- [ ] node-c joins node-b on the in-process `dagda://` and `dannan://` drivers once they exist
      (integration plan, part D). Until then it waits (the maintainer, 2026-11-23).
- [ ] The realm's subscriber config listing both nodes.
- [ ] A call each way between a phone on node-b and one on node-c, and a REGISTER through either
      reaching the other: `verify.py` with a `live.json` for the two.
- [ ] The router forwards (Waiting on the maintainer, 2) are what this waits on from outside.

### The stack, now (2026-11-20)

From `TODO/2026-11-20-stack-integration.md`, part A: what needs no decision and nobody else.

- [x] Send Dagda AthenaSIP's requirements per shape (2026-11-23,
      `TODO/2026-11-23-dagda-requirements.md`): twelve shapes, one read on the call path, one true
      compare-and-set (the trunk lease), counters by sum, call records as a time-keyed log.
- [x] Send Mimir the exact Redis command list with options and the five Lua scripts (2026-11-23, from
      the audit), for its conformance and differential gates.
- [x] Write the liveness pattern for Lir as a scenario (2026-11-23,
      `TODO/2026-11-23-lir-liveness-scenario.md`): retained status and will on one topic, seven
      steps from start to heal, each with what AthenaSIP needs of the broker; sent to Lir.
- [x] Audit the Redis scripts (2026-11-23, `TODO/2026-11-23-redis-scripts-audit.md`): three of five
      declare every key; `presence_put` and `presence_delete` build index keys inside the script.
      Only a trunk's lease is a true compare-and-set; counters, the call index and presence need
      atomicity, not CAS; a binding needs neither (higher CSeq wins, RFC 3261 10.3).
- [ ] `presence_put` and `presence_delete` declare every key: read the old record first, pass all
      keys in `KEYS` and the old value in `ARGV`, write only if it still holds, retry with a bound
      and a backoff (the audit's recommendation). Multi-key stays, for Mimir and Dagda to answer.
- [ ] **A RESP client by hand for `redis://`** (decided 2026-11-23): framing, pipelining, `AUTH`
      and `SELECT`, TLS with the verification `rediss://` has now, reconnect with backoff; only
      what the driver sends (`TODO/2026-11-23-redis-scripts-audit.md`). Tested against Redis and
      Mimir. Then the build moves to Debian's Boost 1.83 with `boost::mqtt5` pinned over it, and
      Boost.JSON comes from libdannan once part B links it.
- [x] `mqtt://` takes a list of brokers and moves to the next with backoff and jitter (2026-11-23):
      `mqtt://h1:1883,h2:1883`, boost.mqtt5's reconnect (1 to 16 s, half a second of jitter);
      `types::URL` keeps the text as given, since RFC 3986 holds one host
      (`MqttEventSystemTest.AListOfBrokersMovesPastOneThatDoesNotAnswer`, failing first).
- [ ] `RtpengineMediaEngineTest.ConnectSucceedsWhileAnyEngineAnswers` aborted once in the full
      suite on a build machine (2026-11-23, "terminate called without an active exception", which
      is usually a thread destroyed joinable); 20 runs of it alone passed. Log kept on the build
      machine in `/root/athenasip-gate/findings/`. Read it, and run it in the suite until it fails
      again, before naming a cause.
- [ ] `test/interop/live.json` still describes the old node-b (the carrier trunk, node-a's trunk
      to it): bring it to node-b as it is, or `verify.py` fails there.

Then part B, on Dannan `0.11.1`: inter-node TLS and the CA from Dannan, and
`dannan::config` with the stack's naming (the integration plan has both). Later, each gated in
that document: Lugh sign-in; the in-process `dagda://` datastore and `dannan://` event system on
one Dannan instance per process (Decisions, 2026-11-23); `mqtt://` proved against Lir and the
`redis://` tests against Mimir.

### Found setting up node-b (2026-11-01)

- [ ] **PROXY protocol v2 from a trusted proxy**, on the HTTP and WebSocket listeners
      (asked of the maintainer, not yet answered: Waiting on the maintainer, 3). Behind HAProxy
      every connection comes from HAProxy, so the per-source API limits are shared by everyone,
      and SIP over `wss://athenasip.com/ws` on 443 would put every browser behind one address
      for the Digest guessing bans.

### Left from the external review (2026-10-25)

`reviews/2026-10-25/athenasip-review.md`. Every finding but these is done (`COMPLETED.md`, The
perimeter, and fuzzing and hosted CI with `0.13.0`): 30 waits on the maintainer (5); 42, comments
that restate code, as files are touched; 45, adversarial tests, a corpus kept from the fuzz runs
and replayed as unit tests (each fix brought its own test already).

### DTMF, every flavour (Milestone 3, decided 2026-10-12)

Built 2026-10-26 (`COMPLETED.md`) and unit-proven only. Left:

- [ ] A sipp scenario sending RFC 4733 events through the node, with and without
      rtpengine, and one sending INFO to a leg that takes events (with the harnesses, above).
- [ ] Each conversion against a real rtpengine and a phone: an INFO phone to the carrier's
      voicemail, and the carrier's events to an INFO phone. The `block DTMF` flag on the offer
      towards an INFO leg and `DTMF-log-dest` per call are from rtpengine's ng documentation and
      `daemon/dtmf.c`, not yet seen working.

### The console (`console/`, its own plan in `console/TODO/ACTIVE.md`)

Unblocked since the console moved into this repository (2026-11-20):

- [ ] Ring the automated browser call: `browser.sh` answers within milliseconds and so
      never rang long enough to hit the DTLS-role bug the live call found; a delay of a
      few seconds before the callee answers would have caught it. A knob on the spec.
- [ ] Flag a node whose `policy.fingerprint` differs from the others'.
- [ ] Pages for trunks and for attributes.
- [ ] The presence screens, from the API (`docs/design/presence.md`, The API and the console).
- [ ] A client generated from the OpenAPI document in place of the hand-written one; every
      operation in `docs/api/openapi.yaml` carries an `operationId` for it.
- [ ] The console's styling from Lugh's admin UI and sign-in through Lugh (the stack's dashboard
      rule), when Lugh tags `0.1.0` (integration plan, part C).

### Smaller, unblocked

- [ ] A sipp scenario for push (RFC 8599) with a fake provider, so the harness proves
      what `tests/push/` proves only in units: a sleeping client woken, then the INVITE
      delivered down the flow it opens.
- [ ] An arm64 Docker image beside the amd64 one, and the SDK headers in the packages.
- [ ] A manual page for `athenasip` (lintian's `no-manual-page`).

### Milestone 7, the rest - conferencing and the web client

The order is the milestone's own:

- [ ] RFC 4579 conference focus routing (`docs/design/conferencing.md`, a focus as a trunk with
      an `attributes.focus`, eight steps). Steps 1 to 4 are in (`COMPLETED.md`). Still to do
      before step 5: `/api/v1/focuses` as a view on the trunks with a `focus` attribute
      (decision 1), and the quick start's `conf-` prefix with the numeric-prefix note
      (decision 4), which waits for a focus to show. Step 5 onward needs a FreeSWITCH as the
      focus and sipp with three participants (with the harnesses, above).
- [ ] RFC 4575 conference event package proxied to participants; roster in the admin
      and client APIs (the design's step 6, and its decision 3).
- [ ] `MediaEngine::conference` capability implemented for rtpengine publish/subscribe
      (basic SFU), then an SFU driver (Janus / mediasoup / LiveKit) behind the same URI
      scheme. The builtin relay stays `bridge` only.
- [ ] RFC 6080 client configuration: looked at on 2026-10-26, a day's work (the notifier's
      package model has no room for `Event` parameters or `message/external-body`), for clients
      that do not exist yet; left until the web client wants it.
- [ ] RFC 8599 section 6, `pn-purr` for long-lived dialogs, which nothing needs yet.
- [ ] Web client repository: video calling and conferencing, JsSIP over WSS, served by
      AthenaSIP, configured from `/api/v1/subscriber/{realm}/config` with the subscriber's
      own credentials.

---

## Waiting on something

Each with what it waits on. None is forgotten; none can be worked here today.

**The phone (Waiting on the maintainer, 8):**

- [ ] The phone's media across the internet: AthenaPhone on mobile data to node-b.
- [ ] Hold and resume, and blind transfer (REFER proxying), verified against AthenaPhone.
- [ ] DTMF conversion and in-band proved on the test phone against the carrier.
- [ ] Presence with a real client. AthenaPhone agreed on 2026-10-11 to be it once its PJSIP
      engine has presence: one subscription per buddy for `presence` and `dialog`, PUBLISH
      of PIDF, BLF pickup with Replaces from the dialog ids the node sends.
- [ ] Interop matrix documented: AthenaPhone, JsSIP in Chrome, Firefox and Safari,
      Linphone, a hardware desk phone, Asterisk as a trunk.

**The harnesses on the build cluster (Next):**

- [ ] Two RFC 5626 flows on two nodes: the console phone registers reg-id 1 and 2 with one
      +sip.instance, each flow preferring a different node. Unit tests prove both bindings side
      by side and one flow at a time per instance on one node
      (`RegistrarTest.ASecondRegIdIsASecondFlow`, `ProxyOutboundTest`); an interop case with
      two nodes proves a call reaches the flow held by the other node, and that a 430 or a
      closed flow there moves to the flow here.
- [ ] `across-cancel-ringing` failed once in five runs at `5986eaa`, not reproduced since
      (2026-10-06: 15 runs on its own, three of the full harness). Keep the next failure's logs
      (`cluster-across-cancel-ringing*.log` and both nodes') and read them before naming a cause.

**The maintainer's word (not now, 2026-10-12: do not ask):**

- [ ] Push proved against the real Apple, Firebase and Web Push services.

**The Homebrew tap (Waiting on the maintainer, 7):**

- [ ] The Homebrew formula tried against a published tag.

---

## Parked

Kept in the tree or history, not on any list:

- `RTPProxyClient` (`src/rtp/rtp_proxy_client.*`): rtpproxy text protocol. Out of the
  build path since 2026-09-21, the way Lua was. May become an `rtpproxy://` driver with
  `bridge` only; not a priority.
- A node taking over a live dialog's signalling when the node in its Record-Route dies.
  The dialog's state is in the call record and a surviving node that is sent a request in
  it carries the dialog on (2026-10-26), so this is routing and media, not replication: the
  request has to reach a surviving node, whose address is not in the dialog's Record-Route,
  and that node has to take the media over.
- Parallel forking. The fork is serial: one branch at a time, best response wins. 16.7's
  response context is written to hold more than one branch and the CANCEL path already
  walks it, so the change is in `_forward_next` rather than in the shape. Forking
  creates one dialog per answering branch (12.1) and the model holds them, but with one
  branch outstanding at a time the second is untested; parallel forking is what would
  exercise it.
- `Util::is_ipv4` (`src/util.cpp:148`) still uses a regex and does see network data, but
  the pattern is anchored with no nested quantifiers, so it is linear and not the hazard
  the message-path regexes were. Left deliberately.
