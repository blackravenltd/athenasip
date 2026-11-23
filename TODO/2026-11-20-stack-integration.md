# AthenaSIP and the Cluster Stack: what to take, and when

Written 2026-11-20 from what the stack's own sessions said that day (the coordinating session,
Dannan, Dagda, Lir, Mimir, Lugh), not from their code, and rationalised on 2026-11-22. A plan only:
nothing of it is built yet. `ACTIVE.md` carries the items; this is the reasoning.

## The stack as it stands

Eight projects, all leaderless and multimaster, no special node: Dannan, Dagda, Lir, Lugh,
Louhi, Mimir, Kvasir, Macha, and AthenaSIP since 2026-11-20. Dagda's stack master
(`2026-11-03-stack.md`) draws AthenaSIP as a hosted system directly on Dagda.

| Project | What it is | Version | For AthenaSIP today |
|---|---|---|---|
| Dannan | The substrate: membership, agreement (CASPaxos registers), hybrid clock, channels, identity and CA, a mutual-TLS listener, layered config | 0.11.1 (as of 2026-11-23) | Usable. ABI kept within a minor; pin the exact release. Its listener and its config stand alone, without a node in-process |
| Dagda | The store: heads with an owner's merge, objects, claims, node-local spaces, leases | 0.4.0 (2026-11-23, on Dannan 0.11.1; arm64 packages `libdagda0.4.0`, `libdagda-dev`, `dagda-tools` in the apt repository, amd64 to follow) | Usable but 0.x, any minor may change a call. Missing what AthenaSIP needs: durable leases (D5), the register map (D6, in its gate), logs (D9), TTL, counters |
| Lir | The MQTT broker | none; rewritten from scratch | Not usable. No retained messages, wills or QoS 1 until Dagda's D5, D6, D9 |
| Mimir | Redis-protocol store | 0.1.0 untagged | Not usable. In memory only; multi-key writes refused until Dannan's atomic commit |
| Lugh | Directory and sign-on (OIDC) | 0.1.0 coming (as of 2026-11-20) | Usable on one node. Its client library links Dannan |
| Kvasir | MySQL-protocol database | in development | Nothing AthenaSIP needs |
| Louhi | Container orchestrator | 0.1.0 releasing | A way to deploy AthenaSIP later, not a dependency |
| Macha | Media and file server | 0.95.1 pending | Nothing; shares node-b's HAProxy |

Everything AthenaSIP would take for its datastore and its bus waits on the same two things:
Dagda's D5, D6 and D9, and Dannan's atomic commit across replica sets.

## Decisions

From 2026-11-20 unless dated otherwise, the maintainer, confirmed in session where they came
through the coordinating session.

- **(2026-11-23) In the stack, AthenaSIP uses Dagda and Dannan and nothing else for storage and
  messaging, linked in-process** (the maintainer, directly). A `dagda://` datastore and a
  `dannan://` event system are plugins like the others, but in-process as `memory://` and
  `local://` are: each AthenaSIP node is a member of a Dannan cluster, Dagda replicates the
  records among the nodes themselves, and the nodes find each other and carry their status over
  Dannan. No broker and no external store. **AthenaSIP must remain usable with Redis and MQTT, or
  memory**: `memory://`, `local://`, `redis://` and `mqtt://` all stay, and a single node still
  needs nothing. This supersedes, for the stack's own deployments, the route through Lir (the bus
  decision below) and the idea of running AthenaSIP on Lir and Mimir; Lir and Mimir remain
  brokers and stores in their own right that the `mqtt://` and `redis://` plugins can point at.
  The `athenasip` binary then links Dannan and Dagda: a new dependency, taken because the stack's
  rule is to link rather than duplicate.

- **AthenaSIP is one of the stack's projects**, under its group rules and its no-duplication
  audit: hosted CI green with badges, coverage and CodeQL; Debian packages at each release to
  the tomcully.com apt repository; heavy builds and tests on the build cluster; the console in
  this repository. All in place since `0.13.0`.
- **The datastore's route is a native `dagda://` driver**, written as Dagda's register map (D6),
  leases (D5) and log (D9) are released; `redis://` until then. Mimir is not the route; it may
  test the `redis://` driver.
- **All four existing backends stay**: `memory://`, `redis://`, `local://`, `mqtt://`, beside the
  stack's. A single node still needs nothing.
- **The bus**: `mqtt://` is the standard protocol to a broker; it points at Lir when Lir has
  retained messages, wills and QoS 1. No driver of AthenaSIP's own for Lir. *Superseded for the
  stack's own deployments on 2026-11-23 by the in-process `dannan://` event system; `mqtt://`
  stays, and may still point at Lir.*
- **Admin users**: password users stay beside Lugh sign-in, but hashing, sessions and the login
  limiter move to Dannan's credential helper and signed session tokens (its `0.12.0`); roles and
  subscriber Digest stay AthenaSIP's. Whether password users go altogether later (Lugh only, as
  Macha does) is still open. SIP Digest credentials are not put in Lugh (HA1 is a password
  equivalent; Lugh holds Argon2id only).
- **The admin HTTP server**, router, OpenAPI check and SPA serving move onto Dannan's HTTP core
  when it lands; Dannan wants AthenaSIP's admin API needs before building it.
- **Configuration**: YAML is the source of truth, and a flag or environment variable may override
  any key by one pattern, the key `a.b` as `--a-b` and `<PREFIX>_A_B`, through `dannan::config`
  (Dannan's `docs/config.md`: defaults, YAML, `secrets.yaml`, environment, command line). The
  log keys are `log.level` (all, debug, info, warn, error) and `log.format` (text, json).
- **No Docker Compose on the build cluster**: the harnesses run nodes and sipp directly on claimed
  machines; Docker only for a single container.
- **Boost**, decided 2026-11-23: Debian 13's Boost 1.83, with `boost::mqtt5` pinned over it, and
  `redis://` on a RESP client written by hand in place of `boost::redis` (the proof above). What
  follows is the history of the question.
- **Boost**, as it stood: AthenaSIP links Boost `1.89.0` statically for `boost::redis` (needs `1.84.0`) and
  `boost::mqtt5` (`1.88.0`); the stack builds on trixie's `1.83.0`. Agreed with Dannan to try the
  system Boost with those two header-only libraries as pinned sources over it, proved on a build
  machine first (against `1.83.0`'s Asio: `any_completion_handler`, cancellation, `consign`, and
  what mqtt5 takes from Beast's websocket). Dannan takes headers only, plus Boost.JSON with its
  HTTP core.

Dannan's releases: pin `0.11.1` (2026-11-23, from the coordinating session: Dagda `0.4.0`, also
tagged, needs it or later). `0.11.0` changed the ABI, so a pin moves to it in one step; `0.11.1` is
a patch on the same ABI. Read "Adopting 0.11.0" and the `0.11.1` entry of its `CHANGELOG.md` before
pinning. What they mean for AthenaSIP, read 2026-11-23:

- **Boost.JSON is carried once in libdannan** (0.11.0), and it is public: `dannan::json`'s types
  are Boost.JSON's own and its compiled part is exported from libdannan. AthenaSIP's own static
  Boost.JSON 1.89 beside it would be two definitions of the same symbols from different versions,
  an ODR violation (Dannan, 2026-11-23). So AthenaSIP takes Boost.JSON only from libdannan. Dannan
  builds on Boost 1.83 or later, and its apt packages on Debian's 1.83. Its two ways: (a) Debian's
  1.83 and the packaged libdannan, which Dannan recommends; (b) Dannan from source against 1.89,
  without the apt package, and 1.89 everywhere it meets Dannan's headers. AthenaSIP needs only
  `boost::redis` (1.84) and `boost::mqtt5` (1.88) newer than 1.83, both header-only, so (a) with
  those two pinned over it is the open Boost decision below: prove they compile against 1.83's
  Asio on a build machine, then the maintainer decides.
  **Proved 2026-11-23** on a build machine (Debian 13's Boost 1.83 development packages, the two
  libraries from 1.89 in an overlay searched first, the whole tree with tests): `boost::mqtt5`
  compiles against 1.83's Asio; `boost::redis` does not, because 1.89's `redis_stream.hpp`
  includes `boost/asio/cancel_after.hpp`, which Asio gained in Boost 1.86. Nothing else failed.
  So option (a) holds for `mqtt://` and not for `redis://` as it is written. Ways on, for the
  maintainer: a RESP client written by hand on Asio in place of `boost::redis` (the driver sends
  ten commands and `EVAL`, and the project prefers hand-written to a library); `boost::redis` from
  1.84 or 1.85, whose API differs from what the driver uses; or option (b), Boost 1.89 everywhere
  and Dannan from source.
- **The node settings live under `cluster.*`** (`dannan/config/node_settings.h`). AthenaSIP has
  its own `cluster:` section (enable, address, port, advertise, ca, cert, key); the two are
  reconciled when inter-node TLS moves onto Dannan.
- `Fault::cancelled` is new: every `switch` over `Fault` needs the case.
- A TSan run passes Dannan's suppressions (`DANNAN_TSAN_SUPPRESSIONS`, installed with
  libdannan-dev).
- `0.11.1` adds ordered channel delivery (`Delivery::ordered`), which a `dannan://` event system
  wants for one node's status reports.

`0.12.0` brings the HTTP core and the credential helper; no date for it.


## The plan, in the order it can be worked

### A. Now, needing nobody

- **Tell the stack what AthenaSIP needs**, which is what each session asked for:
  - Dagda: the requirements table per shape (sizes, counts, rates, latency, durability, TTL
    ranges, whether anything needs linearizable compare-and-set, CDR volume and how it is
    scanned, counter semantics). With it AthenaSIP enters Dagda's needs table, and TTL, counters
    and the time-scanned log each have a second user and get built in Dagda.
  - Mimir: the exact command list with options and the ten Lua scripts, for its conformance and
    differential gates. It shows which scripts its one-key rule refuses today.
  - Lir: the liveness pattern as a written scenario (retained status plus a will on one topic,
    node death, split and heal), and later the `mqtt://` driver as a second real client.
  - The coordinating session: AthenaSIP's row for the stack's placement table.
- **Audit the Redis scripts**: each must declare every key it touches in `KEYS` (Redis Cluster
  wants this too, so it is right regardless), and say which records truly need compare-and-set.
  Dagda's point stands on its own: a REGISTER needs no CAS if "per Call-ID, the higher CSeq
  wins" is the rule, which is RFC 3261 10.3's own.
- **The build cluster** (done for `0.13.0`'s gate): heavy builds and tests run there, under
  `/root/athenasip-*`, by the claim board's rules. Left: the harnesses on three or four real
  nodes there, without Docker Compose (`ACTIVE.md`, First after 0.13.0).
- **`mqtt://` takes a list of brokers** and reconnects to the next with backoff and jitter. Useful
  with any broker pair today, and what Lir asks of a client until its DNS name exists.

### B. Dannan 0.11.1, tagged: open once Boost is settled

Pin `GIT_TAG 0.11.1` or `find_package(dannan 0.11.1 CONFIG REQUIRED)`; read "Adopting 0.11.0" and
the `0.11.1` entry of its `CHANGELOG.md` first (summarised above), settle the Boost.JSON overlap,
and note in the pinning commit what of AthenaSIP's own it replaces.

**`cluster.*` is Dannan's** (the maintainer, 2026-11-22: it belongs to the node settings, and no
hosted system uses it for its own listener). Dannan's `add_node_settings` adds, under `cluster.`:
`key` (the cluster key file), `bootstrap`, `state`, `name`, `domain`, `relay`,
`listen.address`/`.port`/`.advertise`/`.inbound`, `heartbeat_ms`, `path_class`, `punch_ms`,
`reprobe_ms`, `custody.passphrase_env`/`.credential`/`.credentials_dir`, and
`identity.ca`/`.certificate`/`.revocations`/`.no_cluster_key`; flags and variables follow the
keys (`--cluster-listen-port`, `ATHENASIP_CLUSTER_LISTEN_PORT`). AthenaSIP's present keys map, as
Dannan proposed on 2026-11-23:

| AthenaSIP today | Becomes |
|---|---|
| `cluster.address`, `.port`, `.advertise` | `cluster.listen.address`, `.port`, `.advertise` |
| `cluster.ca` | `cluster.identity.ca` |
| `cluster.cert` | `cluster.identity.certificate` |
| `cluster.key` (a TLS key path) | nothing: the name is Dannan's cluster key file, and a node's private key is held by custody (`cluster.custody.*`), not named by a path |
| `cluster.enable` | nothing: whether a node joins is the binary's rule |

Open for part B: whether inter-node SIP (the 2026-09-17 decision, SIP proxied between nodes over
mutual TLS) runs over Dannan's listener and connector, so these keys are all it needs, or keeps a
listener of AthenaSIP's own, which then leaves `cluster.*` for a section of its own. The mapping
goes to Dannan to check against `node_config()` when part B is drawn. Existing node configurations
(node-a, node-b) change with it: a migration note in the release that ships it.

- Inter-node SIP over Dannan's listener and connector (`dannan::listen`, no node in-process),
  with certificates from `dannan-ca` in place of `--ca-init` and `--ca-node` and
  `src/cluster_ca.*`. Dannan has not checked that its CA's files fit the listener as they
  stand. SIP between nodes stays RFC 3261 over mutual TLS (Decisions, 2026-09-17).
- `dannan::config` (defaults, YAML, `secrets.yaml`, environment, flags, each value saying where
  it came from) in place of `src/config.*`, which also takes the secrets out of `config.yaml`.
  With the override pattern and the log keys (Decisions, Configuration), parsed by
  `dannan::status::parse_log_level` and `parse_log_format`.
- Trunk takeover on Dannan's agreement register works today, but only a split's majority side
  keeps its trunks registered. Not taken: Dagda's lease serves both sides and is the design.

### B2. When Dannan 0.13.0 ships its DNS client (about 2026-12-05)

- Dannan's DNS client (the maintainer's ruling, 2026-11-23: asynchronous with a deadline, A, AAAA,
  SRV and TXT cached by TTL, the system's or named servers, UDP with TCP on truncation) gets NAPTR
  at AthenaSIP's asking. By the no-duplication rule, AthenaSIP's own resolver (`src/dns/resolver.*`
  and `message.*`) then goes and the SIP locator (RFC 3263) runs on Dannan's, behind the same
  `query(name, type)` interface so its tests stay. Dannan sends the shape before any code; ours
  was offered as a reference (RFC 5452 checks, negative caching, the fuzz target).

### C. When Lugh tags 0.1.0

- Lugh as a second identity source for the admin API, one code path for both: the six roles
  as scopes `athenasip:<role>`, verified offline by `lugh_client`'s `RelyingParty`; the console
  signs in through the node (code flow with PKCE, the node as confidential client, its own
  session cookie); the provisioning user as a service app with `client_credentials`.

### D. When Dagda releases D5, D6, D9 and TTL: the in-process datastore

- A `dagda://` datastore driver that links the Dagda library and holds a Dannan instance in the
  `athenasip` process (Decisions, 2026-11-23), copied in shape from Lugh: records as register-map
  keys `realm/id` with `walk_from` paging; expiry as Dagda's TTL option; bindings merged by
  Call-ID and CSeq; the trunk lease as a D5 lease with its epoch on every REGISTER; CDRs on a D9
  log keyed by hybrid clock; counters merged by sum. What it needs is
  `TODO/2026-11-23-dagda-requirements.md`. The contract's version moves if its shape does.
- A `dannan://` event system on the same Dannan instance: node status and liveness from Dannan's
  membership rather than a retained topic and a will, and presence and trunk changes over its
  channels. The bus stays never storage and never on the call path.
- One Dannan instance per process, shared by both plugins and by inter-node TLS (part B), so the
  nodes of a cluster are exactly the members of one Dannan cluster.

### E. Lir, for `mqtt://`

Lir has the liveness scenario (2026-11-23, `TODO/2026-11-23-lir-liveness-scenario.md`) as its own
acceptance tests, disabled until Dagda's register map (D6, for retained messages) and leases (D5,
for wills) ship; until then its broker refuses both, in MQTT's own terms, and offers no interim. A
will carries the client's lease epoch, so one from an older connection never overwrites a newer
`ok`; retained values order by (hybrid clock stamp, node id), settled by the maintainer on
2026-11-20 (Lir's L1) and carried by Dagda's register map. D5 and D6 come together in Dagda 0.5.0.

- When Lir tags a release with retained messages, wills and QoS 1: run the `mqtt://` driver's
  tests and the cluster harness with `ATHENA_TEST_MQTT_URL` at a Lir cluster, then the
  node-death scenario. This proves `mqtt://` against Lir; it is no longer the stack's route for
  AthenaSIP (Decisions, 2026-11-23).

### F. When Mimir has atomic commit and persistence

- The `redis://` driver's tests against a three-node Mimir, which Mimir offers to stand up.

## What does not change

SIP stays native and on the call path alone; the bus is never storage and never on the call
path; a single node needs no external service; every existing backend (`memory://`, `local://`,
`redis://`, `mqtt://`) stays usable; the plugin contract stays the product.
