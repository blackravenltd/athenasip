# AthenaSIP and the Cluster Stack: what to take, and when

Written 2026-11-20 from what the stack's own sessions said that day (the coordinator, Dannan,
Dagda, Lir, Mimir, Lugh), not from their code. A plan only: nothing here is built, and the
decisions at the top are the maintainer's. `ACTIVE.md` carries the items; this is the reasoning.

## The stack as it stands

Eight projects, all leaderless and multimaster, no special node: Dannan, Dagda, Lir, Lugh,
Louhi, Mimir, Kvasir, Macha. AthenaSIP is not in the group (Dannan's session, citing the maintainer), though
Dagda's stack master (`2026-11-03-stack.md`) draws it as a hosted system directly on Dagda.

| Project | What it is | Version | For AthenaSIP today |
|---|---|---|---|
| Dannan | The substrate: membership, agreement (CASPaxos registers), hybrid clock, channels, identity and CA, a mutual-TLS listener, layered config | 0.9.2, 0.10.0 close | Usable. ABI kept within a minor; pin a minor. Its listener and its config stand alone, without a node in-process |
| Dagda | The store: heads with an owner's merge, objects, claims, node-local spaces, leases | 0.3.2 | Usable but 0.x, any minor may change a call. Missing what AthenaSIP needs: durable leases (D5), the register map (D6, in its gate), logs (D9), TTL, counters |
| Lir | The MQTT broker | none; rewritten from scratch | Not usable. No retained messages, wills or QoS 1 until Dagda's D5, D6, D9 |
| Mimir | Redis-protocol store | 0.1.0 untagged | Not usable. In memory only; multi-key writes refused until Dannan's atomic commit |
| Lugh | Directory and sign-on (OIDC) | untagged, 0.1.0 coming | Usable on one node. Its client library links Dannan |
| Kvasir | MySQL-protocol database | in development | Nothing AthenaSIP needs |
| Louhi | Container orchestrator | 0.1.0 releasing | A way to deploy AthenaSIP later, not a dependency |
| Macha | Media and file server | 0.95.1 pending | Nothing; shares node-b's HAProxy |

Everything AthenaSIP would take for its datastore and its bus waits on the same two things:
Dagda's D5, D6 and D9, and Dannan's atomic commit across replica sets.

## Decisions for the maintainer

**Answered the same day, and confirmed by the maintainer in session** (the first five through the
coordinator session, then here):

- 1: AthenaSIP joins the Cluster Stack; the group rules and the no-duplication audit apply.
- 5: hosted CI replaces "No hosted CI" (GitHub CI green, badges, Codecov, a release badge,
  `tools/ci.sh` and `ci.yml` shaped as Dannan's, CodeQL); debs by the stack's convention, and
  AthenaSIP's own debs may go to the tomcully.com apt repository at a release; the dashboard rule
  applies, and `../athenasip-admin` is brought into this repository.
- The order stays the coordinator's: Mimir for the datastore first, Mosquitto until Lir is
  ready, then Dannan's identity and config. That leaves 2 open, since Dagda says Mimir as the
  datastore needs the maintainer's ruling.
- 3, 4, 6 and 7 are not answered. Joining the stack implies 4 (Dannan is linked).

A second set followed, of which the datastore route and the backends are confirmed here:

- 2 is closed: a native `dagda://` driver, written as Dagda's D6, D5 and D9 land; `redis://` until
  then; Mimir is not the route. AthenaSIP's requirements table goes to Dagda.
- All four existing backends stay (`memory://`, `redis://`, `local://`, `mqtt://`) beside the
  stack's.
- No docker compose on the build cluster: the harnesses run AthenaSIP nodes and sipp directly on claimed
  cores, as the other projects do, with a node dropping treated as normal. Docker itself is
  there for a single container (rtpengine, a broker).
- 6, from the group rules: password users stay beside Lugh sign-in, but hashing, sessions and
  the login limiter move to Dannan's credential helper and signed session tokens; roles and
  subscriber Digest stay AthenaSIP's.
- The HTTP server, router, OpenAPI check and SPA serving move onto Dannan's HTTP core as it lands.
- Boost: with `redis://` and `mqtt://` kept, the 1.89 conflict is to be settled with Dannan.
  Options: those two backends as build options so a stack build uses the system Boost; the two
  header-only libraries from 1.89 over the system Boost; or Dannan and Dagda from source on 1.89.

Settled with Dannan the same day, subject to the above: try the system Boost (trixie's 1.83) for
everything, with boost::redis and boost::mqtt5 alone as pinned header-only sources over it, and
prove on a build machine that both build against 1.83's Asio (any_completion_handler, cancellation,
consign, and what mqtt5 takes from Beast's websocket) before committing. Dannan takes headers
only (Asio, Asio SSL, Beast), and Boost.JSON with its HTTP core. Dannan's minors: 0.10.0 close
(clock rules, stream carrier); 0.11.0 agreement's stamped requests and certificate expiry;
0.12.0 the HTTP core and the credential helper with signed session tokens, for which Dannan
wants AthenaSIP's admin API needs before it is built. No dates past 0.10.0.

Dannan was told by the maintainer on 2026-11-20 that AthenaSIP is not in the group, and is asking him.

As asked, before those answers arrived:


1. **Is AthenaSIP a hosted system of the stack, or a product beside it?** The answers disagree.
   As a member it falls under the group rules (below) and the no-duplication audit, and its
   stated product principle (plugins: `memory`/`redis`, `local`/`mqtt`, nothing privileged)
   needs restating. Recommendation: stay a product beside the stack, with the stack as one more
   set of drivers behind the existing registries, so a single node still needs nothing and a
   Redis and Mosquitto cluster still works.
2. **The datastore's route to Dagda.** Dagda recommends a native `dagda://` driver and says that
   `redis://` pointed at Mimir breaks "hosted systems never address one another" and needs
   The maintainer's ruling even as a stopgap. The coordinator recommends Mimir first, as it needs no code.
   Recommendation: native `dagda://` as the target, written when D5 and D6 are released; Mimir
   as a compatibility test of the `redis://` driver only, which costs nothing and helps Mimir.
3. **The bus.** The same principle applies to `mqtt://` at Lir. Recommendation: it is the
   standard protocol to a broker, as Mosquitto is today; switch when Lir has retained, wills and
   QoS 1, with no driver of AthenaSIP's own.
4. **Linking Dannan into the node at all.** Lugh sign-in, Dannan's listener, Dannan's config and
   a `dagda://` driver each put Dannan in the binary, built from source against Boost 1.89 (the
   debs are built on 1.83 and cannot be mixed; boost::redis and boost::mqtt5 need 1.84 and
   1.88). It is a new dependency under the minimal-dependencies rule.
5. **The group rules against AthenaSIP's decisions.** Hosted CI green with all badges on public
   repos, against "No hosted CI" (2026-10-26). About 100% coverage with Codecov, against none
   measured. Debs in every release, against "No Debian packages for now" (2026-10-12). Squash
   onto `develop`, against merge commits. All builds and tests on the build cluster. The dashboard
   rule (a React console styled from Lugh's admin UI, sign-in by password or Lugh).
6. **Admin users long-term**: local users with Lugh as an option (Lugh has no objection; Mimir
   and Kvasir do the same), or Lugh only, as Macha is going.
7. **SIP Digest credentials in Lugh**: not planned. Lugh holds Argon2id only and HA1 is a
   password equivalent; it would be a new factor kind in Lugh. Recommendation: no.

## The plan, in the order it can be worked

### A. Now, needing nobody (and no decision)

- **Tell the stack what AthenaSIP needs**, which is what each session asked for:
  - Dagda: the requirements table per shape (sizes, counts, rates, latency, durability, TTL
    ranges, whether anything needs linearizable compare-and-set, CDR volume and how it is
    scanned, counter semantics). With it AthenaSIP enters Dagda's needs table, and TTL, counters
    and the time-scanned log each have a second user and get built in Dagda.
  - Mimir: the exact command list with options and the ten Lua scripts, for its conformance and
    differential gates. It shows which scripts its one-key rule refuses today.
  - Lir: the liveness pattern as a written scenario (retained status plus a will on one topic,
    node death, split and heal), and later the `mqtt://` driver as a second real client.
  - The coordinator: AthenaSIP's row for the placement table, once the maintainer has ruled on 1.
- **Audit the Redis scripts**: each must declare every key it touches in `KEYS` (Redis Cluster
  wants this too, so it is right regardless), and say which records truly need compare-and-set.
  Dagda's point stands on its own: a REGISTER needs no CAS if "per Call-ID, the higher CSeq
  wins" is the rule, which is RFC 3261 10.3's own.
- **The build cluster.** Heavy builds and test runs move to the build cluster (arm64, Debian trixie),
  under `/root/athenasip-*`, by the claim board's rules. It gives what this plan lacked: clang
  with libFuzzer (review item 6), an arm64 build, and three or four real nodes for a cluster
  test. Docker on the build cluster for the sipp harnesses is to be asked of the coordinator.
- **`mqtt://` takes a list of brokers** and reconnects to the next with backoff and jitter. Useful
  with any broker pair today, and what Lir asks of a client until its DNS name exists.

### B. When Dannan 0.10.0 is tagged (needs decision 4)

- Inter-node SIP over Dannan's listener and connector (`dannan::listen`, no node in-process),
  with certificates from `dannan-ca` in place of `--ca-init` and `--ca-node` and
  `src/cluster_ca.*`. Dannan has not checked that its CA's files fit the listener as they
  stand. SIP between nodes stays RFC 3261 over mutual TLS (Decisions, 2026-09-17).
- `dannan::config` (defaults, YAML, `secrets.yaml`, environment, flags, each value saying where
  it came from) in place of `src/config.*`, which also takes the secrets out of `config.yaml`.
- Trunk takeover on Dannan's agreement register works today, but only a split's majority side
  keeps its trunks registered. Not taken: Dagda's lease serves both sides and is the design.

### C. When Lugh tags 0.1.0 (needs decisions 4 and 6)

- Lugh as a second identity source for the admin API, one code path for both: the six roles
  as scopes `athenasip:<role>`, verified offline by `lugh_client`'s `RelyingParty`; the console
  signs in through the node (code flow with PKCE, the node as confidential client, its own
  session cookie); the provisioning user as a service app with `client_credentials`.

### D. When Dagda releases D5, D6, D9 and TTL (needs decisions 1, 2 and 4)

- A `dagda://` datastore driver, copied in shape from Lugh: records as register-map keys
  `realm/id` with `walk_from` paging; expiry as Dagda's TTL option; bindings merged by Call-ID
  and CSeq; the trunk lease as a D5 lease with its epoch on every REGISTER; CDRs on a D9 log
  keyed by hybrid clock; counters merged by sum. The contract's version moves if its shape does.

### E. When Lir tags a release with retained, wills and QoS 1

- Run the unit suite and the cluster harness with `ATHENA_TEST_MQTT_URL` at a Lir cluster, then
  the node-death scenario; measure how long a will takes, since that is the node-down signal.

### F. When Mimir has atomic commit and persistence (needs decision 2)

- The `redis://` driver's tests against a three-node Mimir, which Mimir offers to stand up.

## What does not change

SIP stays native and on the call path alone; the bus is never storage and never on the call
path; a single node needs no external service; the plugin contract stays the product.
