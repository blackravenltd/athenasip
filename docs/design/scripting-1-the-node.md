# AthenaSIP - Scripting, part 1: what the node provides

This is the first of two design documents. It says what the node must gain for routing
and authorisation to be scripted, how the decisions leave the C++ and what the scripts
are handed in their place. [Part 2](scripting-2-the-engine.md) is the Lua engine, the
standard scripts that reproduce today's behaviour, and the scripts that go beyond it.

Written 2026-10-07 against `0.9.0`. Line numbers are from that tag. It supersedes the
decision of 2026-09-17 that Lua would return only as a routing-policy plugin: Tom decided
on 2026-10-07 that scripting is how routing and authorisation are expressed, because the
alternative, a configuration option for every case a deployment needs, makes for a
configuration nobody can read and still cannot express the next case.

## The principle

**Scripts decide; the node acts.** A script is asked a question at each point where the
node has a choice, answers with a decision, and the node carries it out. A script never
touches a transaction, a Via, a Record-Route, a flow token or a nonce, and cannot send a
message. What RFC 3261 requires of a proxy stays in C++ and cannot be broken from Lua.

The line between the two is already visible in `src/proxy.cpp` and `src/registrar.cpp`,
where each step is either a question (who is this, where does it go, what next when a
branch fails) or an act (forward it, challenge it, cancel the branch). The sections below
go through the code and sort every step onto one side or the other.

## Today's decisions, step by step

### An out-of-dialog request through the proxy

`Proxy::on_request` (`src/proxy.cpp:157-207`) and what it calls, in order. "Decision"
marks what a script takes over; "mechanism" stays as it is.

| Step | Where | Side | Notes |
|---|---|---|---|
| No Request-URI: 400 | `:161` | mechanism | |
| Loop token and check (482) | `_loop_token :384`, `_is_loop :403` | mechanism | RFC 3261 16.3 |
| Answer OPTIONS addressed to the node | `:175-191`, `_answer_options :1852` | decision | Whether to answer is policy; the 200 is built natively |
| Session timer: insert, require, 422 | `_apply_session_timer :1512` | mechanism | The values are `sip.session_expires`, `sip.session_min_se` and `sip.require_session_timer`, and stay configuration. Note that this runs before authorisation, so an unknown caller can get a 422; that is unchanged |
| Route preprocessing, strict-router undo, flow token | `_preprocess_routes :423` | mechanism | RFC 3261 16.4 |
| Authorisation | `_authorize :211` | decision | The whole of it; see below |
| Target determination | `_determine_targets :457` | decision | Except Route and flow-token targets, which are mechanism |
| Forwarding, serial forking | `_forward_next :646` on | mechanism | Which branch is tried next is a decision |

`_authorize` (`:211-271`) and `_authenticate` (`:274-355`) make these choices, in this
order:

1. ACK and CANCEL pass.
2. A channel with `peer_node()` set passes. That is the subject CN of a client
   certificate that verified against the cluster CA (`servers/tls_connection.h:83-91`).
3. A request with a To tag inside a known dialog passes.
4. A request carrying a flow token this node sealed passes.
5. The From host, lowercased, names a served realm: Digest in that realm. A reliable
   channel already authenticated as that From AOR by a REGISTER skips the challenge
   (`:282`). The credential's username must equal the From user, or 403 (`:343`). An
   unknown subscriber is 407, the same as a wrong password.
6. No realm for the From host but a Route header remains: 403.
7. The Request-URI host names a served realm: pass, so anybody may call into a realm.
8. Otherwise 403.

Of these, 1, 3 and 4 are properties of the message the node establishes and a script
reads; 2 is a trust rule; 5 to 8 are policy. Verifying a Digest, minting and checking a
nonce, and building a 407 are mechanism, and stay native.

`_determine_targets` (`:457-601`) then takes the first that applies:

1. A Route remains: forward to it. Mechanism.
2. The flow token names a channel, or a UDP flow id: forward down it. Mechanism.
3. The Request-URI host is not a served realm: one target, the Request-URI unchanged,
   sent wherever RFC 3263 says. **Decision.** This is where a trunk would be chosen, and
   today nothing is.
4. A served realm: the subscriber is the whole Request-URI; none is 404; no bindings is
   480; otherwise the bindings become targets. **Decision** at each refusal, and in the
   order the bindings are tried (`_add_targets :958`: most recently registered first).

Expanding a binding into a target is mechanism and stays so: a push binding becomes a
push-and-wait (`_push_and_wait :1016`), a binding held by another node becomes a forward
to that node over the cluster listener (`_held_elsewhere :1137`, `_peer_target :1145`), an
outbound instance's flows are tried in turn (`_target_for :2039`). A script says "ring
this subscriber"; the node knows how.

Forwarding (`_forward_next :646` to `_send_best :1217`) is mechanism throughout, with
these exceptions, which RFC 3261 16.7 leaves to the proxy and a script may own:

- after a branch's final 3xx, 4xx or 5xx, whether to try the next target, stop, or try
  something new (`:932`);
- the 488 re-offer with the other media profile (`_reoffer :1180`), which stays native
  but can be turned off per target.

Everything else there is fixed by the RFCs: 2xx and 6xx end the search, 503 tries the
next DNS hop (RFC 3263 4.3), 408 and 430 try the instance's next flow (RFC 5626), the best
503 goes upstream as 500 (16.7 step 6) and a 430 as 480, timer C cancels a branch that
only rang, and the CANCEL path is untouched.

Media (`_anchor_media :1256`) is mechanism with policy inputs: whether to anchor and
which profile to produce come from `realm.behaviour.over(config->behaviour)` (`:550`),
the subscriber's `media_profile` and what each leg has said. A script sets the policy for
a call; the precedence between the policy, the subscriber and what a leg said stays native
and documented in [Behaviour](../behaviour.md#media_profile).

`_rewrite_contact` (`:606`) is a per-call policy flag with a native implementation.

### A REGISTER

`Registrar::on_request` (`src/registrar.cpp`), in order:

| Step | Where | Side |
|---|---|---|
| AOR from To; none is 400 | `:130-142` | mechanism |
| Realm by the To host | `:145-154` | decision |
| Unserved: 404 if the Request-URI is this node or a served realm, else forward if `sip.forward_register` is `subscribers`, else 403 | `_on_unserved :160-188` | decision |
| Challenge, nonce check, subscriber lookup, Digest verify | `:190-265` | mechanism |
| Granted expiry: `min(requested, realm.registration_timeout)`; 423 under `realm.registration_minimum` | `:482-518` | decision |
| Qualify interval: `realm.behaviour.qualify_over(config)` | `:307` | decision |
| Push: provider served, `accepts`, 555, 423 under the push minimum | `:333-360` | mechanism |
| Outbound: instance, reg-id, `;ob` on the Path, 439 | `:69-101, :362-372` | mechanism |
| Bindings written, qualifier and refresher told, the proxy woken | `:380-480` | mechanism |
| 200 with Contacts, Service-Route, Feature-Caps, alternate servers | `_send_ok :560-626` | mechanism |

A forwarded REGISTER goes through `Proxy::forward_register` (`:1870`) and
`_authorize_relay` (`:1888`): a reliable channel any subscriber authenticated on passes;
otherwise Digest in any served realm, with a 407 carrying one challenge per realm
(`_send_relay_challenge :1960`). That is a decision with the same shape as the proxy's.

### What stays configuration

Not every knob moves. A setting that parameterises a mechanism stays in `config.yaml`:
timers A to K and C, session timer values, `push.timeout` and `push.refresh`,
`flow_idle_timeout`, `media_timeout`, `allow_unencrypted`, the transports, the engines.
What moves is what answers "who", "where" and "what now". The `behaviour` section and the
realm's `behaviour` stay as the data the standard scripts read; a deployment that needs
more than they express writes a script instead of waiting for a fifth setting.

## The policy hooks

Five functions, called by the node at the points above. The exact Lua signatures and
every field are in part 2; this is what each is asked and may answer.

| Hook | Asked when | Answers |
|---|---|---|
| `authorize(request)` | An out-of-dialog request reaches the proxy, and a REGISTER is to be relayed | How the caller is to be proved: Digest in a realm, trusted as a trunk or peer, accepted as already known, or refused |
| `route(request)` | An authorised initial request has no Route and no flow token | A list of targets with a media policy, a reply to send instead, or a refusal |
| `on_failure(request, response, state)` | A branch ends in 3xx, 4xx or 5xx and the search is not over | Try the next target, stop with this response, or try these new targets first |
| `register(request)` | A REGISTER arrives, before it is challenged | The realm, the expiry limits, the qualify interval; or forward it; or refuse |
| `init()` | The scripts load | Nothing; a place to read configuration and build tables |

The `authorize` answer is a requirement, not a verdict. A script says "Digest in
`example.com`"; the node challenges if there are no credentials, verifies if there are,
and only then runs `route`. The script sees the second INVITE exactly as the first, and
need not know whether it was challenged. On success the node writes `request.identity`
(kind, realm, AOR, trunk) for `route` to read.

In-dialog requests, CANCEL, ACK and anything with a Route or a flow token are not put to
`route`: their path was fixed when the dialog formed, and RFC 3261 12.2 and 16.4 say
where they go. A later version may add an observing hook for them; nothing needs it now.

Hooks are asynchronous on the node's side: each takes the Core executor and a handler,
like every plugin operation. That is what lets a script make a datastore lookup in the
middle of a decision without blocking the strand (part 2, coroutines).

## The policy kind

The hooks are a plugin kind, `policy`, beside `datastore`, `events`, `media` and `push`:

```cpp
class Policy : public plugins::Plugin {
  virtual void authorize(plugins::Executor on, std::shared_ptr<RequestView> request,
                         plugins::Handler<AuthDecision> handler) = 0;
  virtual void route(plugins::Executor on, std::shared_ptr<RequestView> request,
                     plugins::Handler<RouteDecision> handler) = 0;
  virtual void on_failure(plugins::Executor on, std::shared_ptr<RequestView> request,
                          std::shared_ptr<SIPMessage> response, const ForkState& state,
                          plugins::Handler<FailureDecision> handler) = 0;
  virtual void register_(plugins::Executor on, std::shared_ptr<RequestView> request,
                         plugins::Handler<RegisterDecision> handler) = 0;
};
```

```yaml
policy:
  url: "builtin://"                   # today's behaviour, no scripts
  # url: "lua://"
  lua:
    path: [/etc/athenasip/scripts]    # searched in order; the standard library is always last
    entry: main.lua
```

Two drivers ship. `builtin://` is today's policy in C++ and the default: a node that
serves its realms and nothing more runs no script and needs none, which is what "runs on
its own" means. `lua://` is for everything else, trunks included. The two agree exactly on
every request the builtin one can answer, and a fixture in the tree keeps it so (part 2,
Proof). Making the hooks a kind costs a registry entry and buys the rest: the decision
types are a documented boundary rather than private proxy state, and somebody who wants
policy in another language, or from an external service, has the same contract a
datastore author has. The contract version bumps when the kind lands.

The decision types are plain structs the node already understands:

```cpp
struct AuthDecision {   // one of
  Digest {std::string realm; bool from_must_match = true; bool trust_flow = true;}  // realm "*": any served realm
  Trusted {std::string kind; std::string name;}               // "trunk", "peer", "address"
  Accept {}                                                   // already proved
  Reject {std::uint16_t code; std::string reason;}
};
struct RouteDecision {  // one of
  Forward {std::vector<TargetSpec> targets; std::optional<MediaPolicy> media; std::optional<bool> rewrite_contact;}
  Reply {std::uint16_t code; std::string reason; Headers extra;}
};
struct TargetSpec {     // one of
  Subscriber {std::shared_ptr<types::Subscriber>; Options}    // expanded to bindings natively
  Uri {std::shared_ptr<SIPUri> uri; std::optional<SIPUri> next_hop; std::optional<std::string> trunk; Options}
  struct Options {std::optional<std::chrono::seconds> ring_timeout; bool reoffer = true;};
};
struct FailureDecision { enum {Next, Stop} or std::vector<TargetSpec> first; };
struct RegisterDecision {  // one of
  Accept {std::shared_ptr<types::Realm>; std::uint32_t max_expires, min_expires, qualify_interval;}
  Forward {}
  Reject {std::uint16_t code; std::string reason;}
};
```

`RequestView` is the read-mostly view of the message a script is handed: method, URIs,
headers, body, the channel's transport, source address, `peer_node`, what it is
authenticated as, `in_known_dialog`, whether a flow token was present, and after
`authorize` the identity. Header edits go through it with a guard list (below).

## What the proxy and registrar become

`Proxy::on_request` keeps its first five steps and then, instead of `_authorize`, asks the
policy. On `Digest` it runs today's `_authenticate` against the named realm, with the
username check if asked; on `Trusted` it records the identity; on `Accept` it continues;
on `Reject` it answers. Then, where `_determine_targets` consulted the datastore, it asks
`route`, and expands each `TargetSpec` with the code that exists: `_add_targets` for a
subscriber, `_flow_to` and the locator for a URI. `_on_response` asks `on_failure` where
line 932 today always continues. `_authorize` and the lookups in `_determine_targets`
are deleted; nothing else in the file changes shape.

`Registrar::on_request` asks `register` where `:145` looks up the realm, and takes the
expiry limits and qualify interval from the answer instead of from `realm` and `config`.
`_on_unserved` goes. `forward_register` asks `authorize` with `request.relay = true`.

Both keep every mechanism they have. The unit tests for the two keep passing, run against
the standard scripts.

## New functionality the node needs

Scripts make trunks expressible; they do not make them work. These are the mechanisms a
trunk needs and the node does not have, each a primitive a script can name but not
implement.

### 1. Trunk records

A trunk is a record in the datastore, provisioned over the API and the console, read by
scripts. Storing it rather than configuring it keeps it shared across the cluster and
editable without a restart.

```
Trunk {
  std::string name;                         // unique; what a script names
  std::shared_ptr<SIPUri> uri;              // sip:sip.carrier.example;transport=tls: where calls and REGISTER go
  std::optional<Credentials> auth;          // username and password for the carrier's challenges
  Registration register_;                   // enabled, expires, contact_user
  std::vector<std::string> inbound_addresses;  // CIDRs a call from this trunk may come from
  std::optional<std::string> tls_ca;        // a CA file; absent means the system store
  json attributes;                          // free-form, for scripts: prefixes, caller id, anything
}
```

Datastore: `trunk_get`, `trunk_create`, `trunk_update`, `trunk_delete`, `trunk_list`, as
defaulted virtuals so existing drivers compile; `memory://` and `redis://` implement
them. API: `/api/v1/trunks` and `/api/v1/trunks/{trunk}`, a new `manage-trunks` role,
`trunks_api.cpp` registered beside the others in `main.cpp:471-505`. The password is
write-only over the API, as a subscriber's is. A read of a trunk includes its
registration state per node, from the events below.

The password has to be stored as given: the Digest response for a carrier's challenge
needs HA1 over the carrier's realm, which is only known from the challenge. The
datastore holds it as it holds everything else; the API never returns it.

A carrier that registers to us is not a trunk. It is a subscriber, and works today.

### 2. Free-form attributes on realms and subscribers

`attributes` (JSON, opaque to the node) on `Realm` and `Subscriber` as well as on
`Trunk`, settable over the API, readable by scripts. This is how a script gets
per-subscriber data without a schema change: a forwarding number, an outbound caller id,
a class of service. The console shows and edits it as JSON.

### 3. A client-side UA for registration

Registering to a carrier is a UAC the node does not have. `LocalUA` builds a BYE as a raw
string and pushes it through the proxy (`src/local_ua.cpp:22-98`); it keeps no state and
answers no challenge. The Qualifier (`src/qualifier.cpp`) is the nearer model: a timer
per probe, `Core::client_transaction_start` with a channel, a persistent Call-ID and an
incrementing CSeq.

`TrunkRegistrar` (new): for each trunk with registration enabled, one REGISTER cycle:
connect or reuse the channel to `trunk.uri`, send REGISTER with the node's advertised
address as Contact and `contact_user` as its user part, answer 401 or 407 with
`digest::respond` (below), refresh at `expires` less a margin or at what the 200 granted,
retry with backoff on failure. State in memory per node, the way the Qualifier's is.

In a cluster one node registers per trunk, or the carrier sees the Contact change on every
refresh. A lease in the datastore (`trunk_lease(name, node_id, ttl)`, `SET NX` in Redis,
trivial in memory) names the registering node; the others watch and take over when the
lease lapses, the way `PushRefresher::_due` re-reads the store before acting
(`src/push_refresher.cpp:65-114`). Calls from the carrier then arrive at the node that
holds the registration, and reach any subscriber through the cluster forward that exists.

Each node publishes `trunks/<name>/status` on the bus (registered, failed, next refresh);
the API's trunk read and the console show it.

### 4. Answering a carrier's challenge on a call

An INVITE to a carrier is answered 401 or 407, and today the proxy relays that upstream
(`proxy.cpp:850-955` treats it as any final failure). The caller's phone cannot answer it:
the credentials are the trunk's. The node must.

Mechanism: when a branch to a target with a `trunk` ends in 401 or 407 and the branch has
not yet been retried, build the credentials with `digest::respond`, send the request again
on a new client transaction with CSeq incremented (RFC 3261 8.1.3.5 and 22.2), and treat
the second answer as the branch's. The retried request has a new branch parameter; the ACK
to the 407 is the client transaction's own.

The caller never saw the increment, so for the rest of the dialog the two sides disagree
on CSeq. The node keeps an offset per dialog per side (`Dialog::cseq_offset`, one new
field in `src/types/dialog.h`): every request it forwards towards the trunk has the offset
added, every response from the trunk has it subtracted before going upstream, and a
challenge on a later in-dialog request (a re-INVITE, a BYE) increments it again.
`Dialogs` stays an observer; the proxy applies the offset in `_prepare_forward` and
`_on_response`. Kamailio does the same in its `uac` module; it is contained, and it is the
single largest piece of mechanism in this list.

`digest::respond(credentials, challenge, method, uri)` is the client half `src/digest.cpp`
lacks: HA1 from username, realm and password, cnonce and nc for `qop=auth`, MD5 or
SHA-256 as the challenge offers. `types::Authorization::to_string()` serialises it.

### 5. Outbound TLS to anyone

`Core::channel_connect` refuses outbound TLS without the cluster certificates
(`src/core.cpp:256-259`) and `_secure_flow` always uses the cluster context, which
verifies against the cluster CA. A carrier is not in the cluster.

A second client context for trunks: the system CA store by default, or the trunk's
`tls_ca`, with host name verification against the trunk's host. `channel_connect` takes
which to use from the target: a `TargetSpec` with a trunk gets the trunk context, a
cluster forward gets the cluster context, and anything else over TLS is still refused.

With that comes a hazard that must be closed first: `Channel::peer_node()` is the verified
peer certificate's CN (`servers/tls_connection.h:83-91`), and `_authorize :220-222` trusts
any channel with one. A channel to a carrier verified against the system store would have
a `peer_node`, and the carrier would be a peer. `peer_identity` must be set only when the
chain ends at the cluster CA, which means the connection records which context verified
it. This closes a hole regardless of trunks; it is simply not reachable today.

Outbound WS and WSS stay refused. No carrier speaks them.

### 6. Header edits with a guard

A trunk call needs headers the node has never rewritten: the From for an outbound caller
id, `P-Asserted-Identity` and `Privacy` (RFC 3325) towards a carrier, a `P-Asserted-Identity`
read and stripped from a carrier's INVITE before it reaches a phone, and the Request-URI
and To user rewritten to the number the carrier wants.

`RequestView` gains `set_header`, `add_header`, `remove_header`, `set_request_user` and
`set_from`, applied to the copy the node forwards, not to what came in, so the dialog's
own identity (From and To tags, Call-ID) is unchanged. A guard list refuses Via, Route,
Record-Route, Path, CSeq, Call-ID, Max-Forwards, Content-Length, Contact and the
tags: a script that sets one gets an error, not a broken dialog. A From rewrite changes
the display name and user, never the tag.

### 7. Ring timeout per target

A hunt sequence ("ring Alice for 20 seconds, then her mobile") needs a branch to give up
before timer C's four minutes. `TargetSpec::Options::ring_timeout`: a timer like C's that
cancels the branch and reports 408 to `on_failure`. Small, and the same code path as
timer C's first firing (`proxy.cpp:1620-1689`).

### 8. Shared counters with expiry

Rate limits, concurrent-call caps per trunk and "has this number called in the last
minute" need a counter every node sees. `Datastore::counter_incr(key, ttl)` and
`counter_get(key)` on the datastore, `INCR` and `EXPIRE` in Redis and a map in memory,
exposed to scripts as `store.counter`. Defaulted virtuals, like the rest.

### 9. The source address on the request

`RequestView::source` carries transport, address, port, `flow_id`, `is_reliable`,
`peer_node` and what the channel is authenticated as. All of it exists on `Channel` and
`Connection`; this is exposure, not new mechanism. It is what matches an inbound call to
a trunk's `inbound_addresses`.

### 10. Observability for trunks

The call record names the trunk on each leg that used one; `GET /api/v1/call-records`
can then answer "what went over acme today". Metrics: `athenasip_trunk_calls_total`,
`athenasip_trunk_registered` per trunk. `athenasip --check` resolves each trunk's host
and, with `--check-trunks`, sends it an OPTIONS and reports the answer.

### 11. The off-node media policy

Found while mapping this: a call to a Request-URI outside every served realm gets a
default-constructed `MediaPolicy` (`src/call.h:92`: anchor, mirror), because
`_determine_targets :539-547` never sets `context->media_policy` and `Core::_on_dialog_change`
(`src/core.cpp:866-892`) creates the call without one. The server's `behaviour.media_anchor:
false` is ignored for exactly the calls a trunk makes. The `route` decision carries the
media policy, and the standard script sets it from `config.behaviour` for this case, which
fixes it; it is also worth a unit test on its own.

## Scripts in a cluster

Trunk records, attributes and counters live in the datastore and are the same on every
node. Scripts are files on each node, and must be the same too: a node routes only what
reaches it, so two nodes with different scripts are two different servers. The operator
installs them as they install `config.yaml`. Each node publishes the hash of its loaded
scripts in `nodes/<id>/status`; `GET /api/v1/nodes` and the console flag a node whose
hash differs from the others.

## Security

- A script runs with no `io`, no `os`, no `debug`, no `package`; `require` reads only from
  `policy.lua.path`. It reaches the network and the store only through the primitives.
- A script error is a 500 to the caller and a log line naming the script, the line and
  the hook. A script that runs past its instruction budget is stopped the same way. The
  transaction completes; nothing hangs.
- Digest verification, nonces, the peer certificate check and the guard list are native.
  A script can decide not to authenticate a request; it cannot accept a bad password.
- The trunk password is in the datastore in the clear, as it has to be. [Authentication](../authentication.md)
  gains a paragraph saying so and what that means for the Redis instance.

## Migration

In this order, each step leaving every test green:

1. **The seam.** Add the `policy` kind, `RequestView` and the decision types. Move today's
   `_authorize`, the lookups in `_determine_targets`, the registrar's realm and expiry
   steps and `_authorize_relay` into a `builtin://` driver. No behaviour changes; the
   unit suite and the three harnesses pass unchanged.
2. **The engine.** The Lua engine and bindings (part 2), the `lua://` driver and the
   standard scripts. Run the unit suite and the harnesses with `policy.url` set to each
   driver in turn; they must agree. `tests/` gets the differential as a fixture so the
   agreement is checked, not asserted, and stays checked: `builtin://` is kept, so every
   later change to one driver's behaviour is a change to both. Lua 5.4 becomes a build
   dependency beside yaml-cpp; `docs/compiling.md`, the Dockerfile and the Linux quick
   start say so.
3. **Trunk mechanism.** Items 1 to 10 above, each with its unit tests; the TLS identity
   fix (item 5) first, as it stands on its own.
4. **Trunk scripts and the harness.** The standard trunk script and the examples (part 2),
   and sipp scenarios with sipp as the carrier: a registrar that challenges, a UAS that
   challenges the INVITE, an inbound call from the carrier's address. Milestone 3's trunk
   scenario, waiting since the plan was written, lands here.

Step 1 is a refactor; the design work is 2 and 3. Part 2 has the engine.
