# AthenaSIP - Presence

Built in `0.12.0`: [Presence and BLF](../presence.md) is the reference for what shipped,
and `TODO/COMPLETED.md` records where it differs from this design. Step 9's real client is
still open (`TODO/ACTIVE.md`, Milestone 7).

What the node must gain to be the notifier for the `presence` and `dialog` event packages
for the subscribers in its realms, and the order it is built in. Written 2026-10-11 against
`develop` at `ae46edc`; line numbers are from that commit. The decisions it builds on are
Tom's of the same day, in `TODO/ACTIVE.md` under Decisions:

- The node is the notifier for `presence` and `dialog`. It answers SUBSCRIBE (RFC 6665),
  accepts PUBLISH (RFC 3903), keeps published state in the datastore and sends the NOTIFYs.
  Line state for BLF (RFC 4235) comes from the dialogs the node already tracks, so a desk
  phone needs nothing of its own beyond a BLF key.
- Watchers are the same realm by default. Anyone else is refused 403. A policy hook lets a
  script decide otherwise. No XCAP and no per-user rule documents.
- No message-waiting indication. A SUBSCRIBE for any package other than the two is
  forwarded to the devices, as every SUBSCRIBE is today.

## What a user sees

A receptionist's desk phone has a BLF key for 1001. When 1001's phone rings, the key
flashes; when 1001 answers, it lights; when the call ends, it goes dark. Nothing about the
desk phone or 1001's phone has to be configured beyond the key itself.

A softphone that shows a buddy list sees each colleague as online when the colleague's
client says so (PUBLISH), and, for a colleague whose client says nothing, online while the
colleague has a registration and offline when the last one goes.

An administrator sees, in the console, who is watching whom and what each subscriber's
state is.

Turning it off is one line, `presence.enable: false`, after which SUBSCRIBE and PUBLISH
are proxied exactly as they are today.

## Where it plugs in

The notifier is the fifth transaction user, beside the registrar, the proxy, the dialogs
and the local UA (Architecture in `TODO/ACTIVE.md`). Like the others it is built by
`Core::_ensure_transaction_users` (`src/core.cpp:597`) and handed `std::shared_ptr<Core>`
and a logger, with its own `LoggerScoped` named `notifier`.

### Requests reaching it

Two ways in, one for each kind of request.

**Out-of-dialog SUBSCRIBE and PUBLISH go through the proxy's authorisation first.** Today
`Core::_deliver_to_tu` (`src/core.cpp:795`) sends REGISTER to the registrar, CANCEL to the
proxy's CANCEL path and everything else to `Proxy::on_request` (`src/proxy.cpp:202`), which
authorises in `_proceed` (`:242`) and then determines targets in the continuation at `:251`.
The notifier is reached from that continuation, in place of `_determine_targets`, when all
of these hold:

1. `presence.enable` is true;
2. the method is SUBSCRIBE with an `Event` package of `presence` or `dialog`, or PUBLISH;
3. no Route remains once this node's own are taken off (`_preprocess_routes`), and the
   request carries no flow token: it is addressed to the AOR, not passing through;
4. the Request-URI host is a realm this node serves.

Taking it after authorisation means a SUBSCRIBE or PUBLISH is proved exactly as an INVITE
is: the standard policy challenges a From in a served realm (Digest, the credential's user
must be the From user), and lets anyone else in only as far as a served realm. Nothing about
who the caller is gets decided twice, and a script's `authorize` already covers presence.

The check sits in the proxy rather than in `_deliver_to_tu` because the proxy owns
authorisation and route preprocessing. Moving them out of the proxy so the notifier could
call them separately would touch every request's path for the sake of two methods.

**In-dialog requests go straight to it.** A refresh or an unsubscribe is a SUBSCRIBE with
a To tag, addressed to the Contact the node gave in its 200. That Contact is in the
presentity's realm, not at this node's address, and carries the subscription's ID as a URI
parameter (`athena-sub`), so a refresh can reach any node that serves the realm. The realm
is resolved as the watcher already resolves it to register, which is how it found this
node in the first place; a connection-oriented or outbound client sends the refresh down
its existing flow regardless. `_deliver_to_tu` hands the
notifier any request whose Request-URI carries that parameter and names a served realm,
after the dialog tracker has looked at it and before the proxy. The notifier reads the
subscription from the datastore by its ID and checks the Call-ID and tags against it (RFC
3261 12.2.2): no match is 481. Nothing else in the request path touches the datastore for
this. These are authorised by the dialog, as any in-dialog request is.

Responses to the notifier's NOTIFYs come back through the client transactions that sent
them (`Core::client_transaction_start`, `src/core.cpp:875`), as the trunk registrar's do.

### Requests leaving it

A NOTIFY is a UAC request of the node's own, built and sent as `TrunkRegistrar::_send`
(`src/trunk_registrar.cpp`) builds a REGISTER: a raw request with a fresh branch, a Via for
the channel's advertised address (`Core::advertised_for`), handed to
`client_transaction_start` with the channel to send it on.

The channel is the watcher's flow: the one its latest SUBSCRIBE arrived on. A watcher
behind NAT is reachable down that flow and nowhere else (RFC 5626). So the stored
subscription names the node and flow it last arrived on, as a binding does
(`types::Location::node_id` and `flow_id`), and **only that node sends its NOTIFYs**. A
refresh arriving at another node moves the subscription there: the record is rewritten with
the new node and flow, and the NOTIFY that follows the refresh goes from the new holder. A
node checks the stored holder before each NOTIFY, so the old holder stops as soon as the
record has moved. If the holder's channel has closed, the NOTIFY goes down the flow of the
watcher's binding whose Contact matches the subscription's remote target (the proxy's
`_target_for` already does this lookup for a binding). Failing that, it goes to the remote
target by RFC 3263 (to the first Route, if the dialog has a route set), and if that fails
too, the subscription ends. Whichever flow a NOTIFY went down is the subscription's flow
from then on. The binding is matched on the Contact's user, host and port, and only a
binding held by this node counts, since only its flows are open here.

The local UA (`src/local_ua.*`) is the other model in the tree, a request entering the proxy
as though a peer had sent it. It does not fit here. A BYE goes to a dialog's far end through
the route set the proxy recorded. A NOTIFY goes from an endpoint, the node itself, to the
other end of a dialog the node is party to.

## The subscription dialog

RFC 6665 4.2 and 4.3, from the notifier's side. The node is a UAS for the SUBSCRIBE and a
UAC for each NOTIFY, in one dialog.

| What | Where it comes from |
|---|---|
| Dialog ID | The SUBSCRIBE's Call-ID and From tag, and a To tag the node generates (RFC 3261 12.1.1) |
| Remote target | The SUBSCRIBE's Contact; a SUBSCRIBE without one is 400 (RFC 6665 4.1.2.1) |
| Route set | The SUBSCRIBE's Record-Route, in order (12.1.1 for a UAS); usually empty |
| Local Contact | The presentity's realm, with the transport of the arriving channel and the subscription's ID as a parameter |
| Local CSeq | The node's own, from 1, raised for each NOTIFY |
| Remote CSeq | The SUBSCRIBE's, checked for order on refresh (12.2.2) |
| Package and `id` | The `Event` header, with its `id` parameter |

**Accepting.** 200 with `Expires` (RFC 6665 4.2.1.1; 202 is not used). The granted
expiry is the requested one clamped to `presence.max_expires`, the package's 3600 when the
request gave none. Under `presence.min_expires` is 423 with `Min-Expires`. A package
neither handled here nor forwarded is not a case that arises, since everything else is
forwarded. An `Accept` that excludes the package's body type is 406.

**The first NOTIFY** follows the 200 at once, with `Subscription-State: active;expires=N`
and the current state in full (4.2.1.2). There is no `pending`: the watcher check answers
yes or no before the 200 is sent.

**Refresh** is an in-dialog SUBSCRIBE: a new expiry, a 200, and a NOTIFY with the current
state (4.2.1.2 asks for one on every refresh).

**Unsubscribe** is a refresh with `Expires: 0`: a 200, then a final NOTIFY with
`Subscription-State: terminated;reason=timeout` and the current state, as Kamailio sends.

Any other request in the dialog is 405 with `Allow: SUBSCRIBE`: only the watcher sends in
it, and only to refresh or end it. A SUBSCRIBE in it for another package or `id` is 489.

**Expiry** without a refresh ends the subscription with a NOTIFY saying
`terminated;reason=timeout`. Expiry runs on the injected `TimerSource`, so the tests
advance a `ManualTimerSource` rather than waiting.

**The node ends it** when the presentity is deleted (`terminated;reason=noresource`) and
when the policy reloads and would no longer allow the watcher (`reason=rejected`). It does
not end it on shutdown: the subscription is a record, so another node takes it over or this
one resumes it on restart (The cluster). `reason=deactivated` would only send every watcher
to subscribe again, perhaps to the node that is going down. Changed 2026-10-11 while
building the core, once subscriptions were records.

**A NOTIFY that fails** ends the subscription. A 481 always does; RFC 6665 4.2.2 also
removes a soft-state subscription on a failing response or a timeout. The exact list of
codes is taken from the RFC text when the core is built, not from this paragraph.

**One subscription per dialog.** RFC 6665 4.5.2 deprecates dialog reuse. A SUBSCRIBE
inside a subscription's dialog for a different package or `id` is refused rather than
starting a second one in the same dialog.

**Subscriptions are stored, as registrations are.** The whole subscription dialog is a
record in the datastore: the rows of the table above, the package, the watcher, the granted
expiry, the holder's node and flow, the `dialog-info` version reached, a digest of the
state the last NOTIFY carried, and when the last presence NOTIFY went. It is written when
the subscription is accepted and on every refresh and NOTIFY, and removed when it ends. The
holder keeps a copy in memory only to run its timers, and rebuilds it from the record. The record is the truth, and a node that restarts or
takes a subscription over reads it back. So a subscription outlives its node: see The
cluster.

## The packages

### `dialog` (RFC 4235)

The state is the presentity's calls, read from the call records already in the datastore
(`Call`, `src/call.h`; `athena:call:*` in Redis, which stores each participant's tags). No
second record of a call is kept for presence: the call record is the one fact, and it
gains what the package needs.

**Which calls count against which AOR.** A participant counts against a subscriber's AOR
when it is the caller the node authenticated as that subscriber, or the callee whose
binding the node rang. The callee is the subscriber whose binding was rung, not the To
header: a script that routes `reception` to 1001's phone lights 1001's key, which is what
a receptionist expects. `Call::Participant` gains `subscriber`, the AOR, set by the proxy
where it authenticates the caller and where a binding becomes a target. The caller's node
knows both, since it resolved the callee's binding before forwarding, and it is the node
that writes the record. A participant that is a trunk, or in another domain, has no
`subscriber` and counts against nothing.

The call record is written on creation and on every state change
(`Core::_on_dialog_change`, `src/core.cpp:972`), which are exactly the moments the
package's state changes. The datastore gains an index by subscriber so the package can
read one AOR's calls without listing every call.

**State mapping**, from the side of the presentity:

| `Call::State` | `direction` | `<state>` |
|---|---|---|
| Trying | initiator | `trying` |
| Trying | recipient | not reported: the phone has not been reached |
| Ringing | initiator | `early` (`proceeding` is the same to every BLF key known) |
| Ringing | recipient | `early` |
| Connected | either | `confirmed` |
| Closing | either | `terminated` |
| Closed | either | not listed: a full document without the dialog says it has ended |

Each `<dialog>` carries `id`, `call-id`, `local-tag` and `remote-tag` (which a phone needs
for call pickup with `Replaces`, RFC 3891), `direction`, `<state>`, and the local and remote
`<identity>`. Watchers are the presentity's own realm by default, so the remote party is
shown. A script that widens who may watch takes on that disclosure.

**The document.** `application/dialog-info+xml`, `entity` the presentity's AOR,
`state="full"` every time. Partial notifications are optional in RFC 4235 and every phone
handles full ones. `version` starts at 0 for each subscription and rises by one with every
NOTIFY on it (RFC 4235 4.1). The `call-id`, `to-tag` and `from-tag` Event parameters,
which narrow a subscription to one dialog, are not supported in v1. Such a subscription is
answered for the whole AOR, as a notifier that does not support them does.

**Rate.** Not throttled. A BLF key that lights five seconds late is a broken BLF key.
Changes within one turn of the strand are coalesced into one NOTIFY.

### `presence` (RFC 3856, PIDF RFC 3863)

The state is what the presentity has published, composed, or, when it has published
nothing, what its registrations say.

**Composition.** Every unexpired publication for the AOR contributes its `<tuple>`s to one
`<presence entity="...">` document. Two publications from two devices are two tuples. A
tuple id that appears in two publications is kept from the more recent one, which the node
takes to be the one that lasts longer, since a publication record holds no time of
publishing and a device refreshes before it lapses. Anything else in a publication
(`<note>`, extension elements such as RPID's `<dm:person>`) is carried through, with the
namespaces the root declared for it, and an element with an `id` is deduplicated as a
tuple is. The node does not interpret a publication beyond finding its elements
(`src/pidf.cpp`, by hand, as the other parsers are).

**Nothing published.** One tuple, `<status><basic>open</basic></status>` while the AOR has
an unexpired binding and `closed` when it has none. That is the decision's "a subscriber
with no bindings closed when nothing is published". No contact URIs are given out.

**Rate.** RFC 3856 asks that a presentity's notifications not exceed one every five
seconds. Changes within the window are coalesced and the latest state sent when it opens.
A NOTIFY answering a SUBSCRIBE goes at once, as RFC 6665 4.2.1.2 requires, and opens a new
window. The window is the holder's, in memory, on the injected clock: a node taking a
subscription over starts a fresh one, which costs at most one NOTIFY inside five seconds.
Changed 2026-10-11 while building the package, from keeping it in the record; a wall-clock
second in the record would be too coarse to keep to the window and not needed to keep the
state.

**Registration changes.** The core signals `presence/<aor>/changed` when a binding is
written or removed (`Core::subscriber_register`, `subscriber_unregister`), beside the
`subscribers/<uri>/status` event it already published on a write. Expiry happens inside the
datastore and signals nothing; the reconcile below sees it on its next pass, which is soon
enough for a buddy list. A publication lapsing is the same.

### PUBLISH (RFC 3903)

Only for the `presence` package. A PUBLISH for another package is 489 with
`Allow-Events: presence`.

Only the presentity may publish its own state. The authenticated From must be the
Request-URI's AOR, or 403. This is fixed, not policy, in v1.

| Request | Answer |
|---|---|
| Body, no `SIP-If-Match` | A new publication: 200 with a new `SIP-ETag` and the granted `Expires` |
| No body, `SIP-If-Match` known | Refresh: new expiry, 200 with a new `SIP-ETag` |
| Body, `SIP-If-Match` known | Modify: the body replaced, 200 with a new `SIP-ETag` |
| `Expires: 0`, `SIP-If-Match` known | Remove: 200, the publication gone |
| `SIP-If-Match` unknown or expired | 412 Conditional Request Failed |
| No body, no `SIP-If-Match` | 400 |
| Body not `application/pidf+xml` | 415 with `Accept: application/pidf+xml` |
| Under `presence.min_expires` | 423 with `Min-Expires` |

Entity tags are random, 128 bits, and a new one is issued on every successful PUBLISH, as
RFC 3903 requires. Publications live in the datastore with their expiry, so a refresh may
reach any node.

## State, and where it lives

All of it is held through the `Datastore` plugin, so it lives wherever the deployment's
datastore does: `memory://` for one node, `redis://` for a cluster, or any third-party
driver. This is the cluster's rule rather than a choice for presence: the nodes share the
datastore and the event system, and that is what makes them a cluster (Tom, 2026-10-11).
What a node holds in memory is a cache of a record, a timer, or a transport flow, and
nothing is lost with it that another node cannot read back.

| State | Datastore record | Lifetime |
|---|---|---|
| Subscriptions | New: by subscription ID, indexed by presentity, with the holder's node and flow | Its granted expiry |
| Publications | New: by entity tag, indexed by presentity | Their `Expires` |
| Calls | The call record there is, with each participant's `subscriber` and an index by it | The call, then its retention as a record |

The event bus carries one thing, `presence/<aor>/changed`: a signal to look again, never
the state. It is not storage. A new subscription, a refresh, a takeover and the reconcile
below all read the datastore, and none of them waits on the bus.

**The reconcile.** The bus need not deliver: the MQTT driver publishes at most once
(`src/events/mqtt_event_system.cpp:335`), and a third-party event system may promise no
more. So every `presence.reconcile` seconds (30 by default) each node reads the state of
the presentities its own subscriptions watch and sends a NOTIFY wherever the state differs
from the digest the subscription's record holds. A lost `changed` costs at most that long. The bus makes a change
prompt, and the reconcile makes it certain.

**The datastore contract.** Five new methods for the two new records, and one for the
call record, as built in `src/datastores/datastore.h`. Like `lease` and the counters they
are optional, with `_unsupported` defaults, and `supports_presence()` says whether a driver
has them, so a third-party datastore built against the
previous contract keeps working. On such a driver the node turns presence off, says so at
start and in `--check`, and proxies SUBSCRIBE and PUBLISH as before. `API_VERSION`
(`src/plugins/plugin.h:27`) goes from 19 to 20, for these and for `subscriber` on the
participant.

```
struct PresenceRecord {      // src/types/presence_record.h
  enum class Kind { Subscription, Publication };
  Kind kind;
  std::string id;            // subscription ID or entity tag
  std::string aor;           // the presentity: the index
  std::string node_id;       // the holder; empty for a publication
  std::string content_type;
  std::string body;          // PIDF for a publication; compact JSON for a subscription
  std::time_t expires_at;    // filled by the store
};

bool supports_presence() const
presence_put(on, PresenceRecord, seconds, StatusHandler)
presence_get(on, kind, id, Handler<std::optional<PresenceRecord>>)
presence_list(on, aor, kind, Handler<std::vector<PresenceRecord>>)
presence_delete(on, kind, id, StatusHandler)       // succeeds whether or not it was there
presence_list_node(on, node_id, Handler<std::vector<PresenceRecord>>)

call_list_for_subscriber(on, aor, Handler<std::vector<std::shared_ptr<Call>>>)   // open calls only
```

`memory://` and `redis://` both implement them. In Redis a presence record is a key with
a TTL, and each presentity and kind, and each holder, has an index set, pruned of lapsed
members as it is read. A record and its indexes are written and deleted by one script, so
a subscription moving holder leaves the old holder's index in the same step. The call
index is a set per subscriber of the calls not yet closed, kept by `call_create` and
`call_update`, because a closed call's record is kept for a month and reading a
subscriber's history to find its open calls would grow without bound.

Call records are rendered into `dialog-info` per subscription, because `version` is per
subscription. The datastore holds the facts, not XML.

## The cluster

The node that writes a call record publishes `presence/<aor>/changed` for each
participant with a `subscriber`, once the write completes; a node that writes a
publication does the same for its presentity. Every node subscribes to
`presence/+/changed`. On a change for a presentity that subscriptions it holds are
watching, it reads that presentity's state back from the datastore and notifies them.

A call between two nodes, 1001 on A calling 1002 on B:

```
1001 --INVITE--> A --INVITE (cluster TLS)--> B --INVITE--> 1002
                 |
                 writes the call record: 1001 the caller, 1002 the callee whose
                 binding A resolved and forwarded to B
                 publishes changed/1001 and changed/1002
```

A receptionist subscribed through node C to 1002 hears A's `changed` and gets a NOTIFY
from C, down the receptionist's own flow.

**A node dying.** Its call records and its subscriptions name it. When the node
directory (`src/node_directory.h`) marks it stale, two things follow.

- Its calls are gone. The orphan sweep that was already there (`Core::_orphan_sweep`)
  closes their records: the live node with the lowest id closes an unanchored call once
  the node is found gone, and an anchored one once its engine says the media has stopped,
  since rtpengine carries the media on without the node and the people on it are still
  talking. Closing a record now signals `changed` for each subscriber on it, so the lines
  go dark wherever they are watched; the reconcile finds any that signal missed. Changed
  2026-10-11 while building the cluster step, from leaving stale nodes' open calls out of
  every document: the sweep already decides when such a call has ended, and a second rule
  for it would disagree with the first while the media still flows.
- Its subscriptions are still in the datastore. A watcher that uses outbound has, or soon
  has, a flow to a surviving node: that node lists the dead node's subscriptions
  (`presence_list_node`), takes over each whose watcher has a binding flow on it (by
  instance, else by Contact) through a datastore `lease` on the subscription's ID so that
  only one node does, rewrites the holder, and sends a NOTIFY with the current state. It
  is matched by Contact alone, as the fallback above is: the subscription does not keep
  the watcher's instance, and a client's SUBSCRIBE Contact is its registered one. A
  watcher with no flow anywhere is taken over by whichever node its next refresh reaches,
  since the Contact names the realm, not the node. One that never refreshes lapses at its
  expiry.

A restarting node's orphan sweep closes the open call records that name it from before
it started, which signals each participant, so no line stays lit for a call that died with
it. Its subscriptions it reads back (`presence_list_node`) at its first reconcile, arms
their expiry from the record, and notifies any whose state is not what was last sent,
unless another node has taken them. The digest of what was last sent is in the record;
for `dialog` it leaves out the `version`, which rises with every NOTIFY.

**Single node.** `memory://` and `local://` serve all of this unchanged: the bus is local
and the datastore is the node's own.

The event bus is still never on the call setup path. No SIP request waits on a
`presence/` message, and an INVITE's progress does not wait on the call record's write, as it does not today.

## The policy hook for watchers

A new question on `policy::Policy` (`src/policy/policy.h:309`), asked after `authorize`
when a SUBSCRIBE for a handled package reaches the notifier:

```
struct WatchDecision {
  enum class Kind {
    Accept,   // the node is the notifier
    Reject,   // code and reason, 403 by default
    Forward,  // not the node's: route it as any other request, as before presence
  };
  ...
};

virtual void watch(plugins::Executor on, std::shared_ptr<RequestView> request,
                   std::string watcher, std::string presentity,
                   std::string package, plugins::Handler<WatchDecision> handler);
```

`watcher` is the address of record the watcher was proven to be (`sip:carol@example.com`),
empty when it was not; `presentity` the one watched. Plain strings rather than identities,
as built in step 7: a script compares them, and an AOR is what both are. In Lua the hook
is `watch(request, watcher, presentity, package)`, with `watcher` nil when unproven.

The default, which `builtin://` uses and which is also what a script without a `watch`
function gets (as `on_failure` falls back today, `src/policy/lua_policy.cpp:414`): accept
when the watcher was authenticated by Digest in a realm and that realm is the presentity's,
and reject 403 otherwise. `scripts/athenasip/standard.lua` gains `std.watch` with the same
rule and `tests/policy` keeps the two in step, as for the other hooks. The prelude gains
`athenasip.watch.accept()`, `.reject(code, reason)` and `.forward()`. Existing `main.lua`
files assign the hooks they replace by name, so they keep working without change; fi-1's
does.

`Forward` is how a deployment with its own presence server keeps using it: the script
says so, and the SUBSCRIBE is routed exactly as it is today.

PUBLISH has no hook in v1. Its rule (only the presentity) is fixed.

## The API and the console

| Route | Role | What |
|---|---|---|
| `GET /api/v1/presence/subscriptions` | `view-cluster-status` | Every subscription in the cluster: watcher, presentity, package, holder, expiry, read from the datastore |
| `GET /api/v1/realms/{realm}/subscribers/{user}/presence` | `manage-realm-subscribers` | The subscriber's composed state: the PIDF basic status and its source (published or registrations), each publication with its expiry, and its dialogs |

The `/api/v1/events` stream carries `presence/<aor>/changed` like any other event, so the
console can update a subscriber's state without polling. The console's screens are the
admin session's to build; this document fixes only what the API returns.

As built in step 8 (`src/api/presence_api.cpp`, through `Notifier::subscriptions` and
`Notifier::state`, so the API composes exactly what a watcher is sent):

- The datastore lists subscriptions by holder only, so "every subscription in the
  cluster" is this node's and each node's the directory has heard of, a stale one
  included. One held by a node the bus never heard is missed; it lapses at its expiry.
  This keeps the datastore contract as it is rather than adding a list of everything.
- A subscriber's state also carries the composed PIDF `document`, and each publication
  its own. `basic` is open when any tuple is.
- With presence off, or a datastore that cannot hold it, the state is 404 `presence_off`
  rather than a state no watcher is sent. The subscription list is what the datastore
  holds, so it is empty only on a datastore without presence: a node with presence off
  still shows what the rest of the cluster serves.

## Configuration

```
presence:
  enable: true        # false: SUBSCRIBE and PUBLISH are proxied, as before
  min_expires: 60     # 423 below this, for SUBSCRIBE and PUBLISH
  max_expires: 3600   # longer requests are granted this
  reconcile: 30       # seconds between each node's re-reads of what its subscriptions watch
```

On by default, because a desk phone's BLF key should work against a fresh install with
nothing configured. All four go into `src/config_schema.cpp`,
`docs/configuration-reference.md` and `config/config.example.yaml`, as every setting does.

`Allow-Events: presence, dialog` is added to the OPTIONS answer the node gives for itself
(`Proxy::_answer_options`, `src/proxy.cpp:2129`) and to the notifier's own responses (RFC
6665 7.2.2), and PUBLISH and SUBSCRIBE to that answer's `Allow`.

## The order it is built in

Each step ends with its tests passing and is a commit of its own. The tests are written
from the RFCs first and watched failing, per `CLAUDE.md`.

1. **The datastore contract**: `subscriber` on `Call::Participant`, the index by it, and
   the five presence methods, in `memory://` and `redis://`, tested against a real Redis as
   the other Redis tests are. Everything after this stores through it.
2. **The notifier core** (RFC 6665): the hand-off from the proxy and from
   `_deliver_to_tu`, the subscription dialog as a stored record, 200, 423, 489 never arising, 403 from the
   default watch rule, the immediate NOTIFY, refresh, unsubscribe, expiry, NOTIFY failure,
   and a fetch (Expires 0). One package, `dialog`, with an empty state, so the
   mechanics are proved before any state exists. GoogleTest with `ManualTimerSource`.
3. **The `dialog` package**: the proxy setting `subscriber` on each participant, the
   state mapping and the XML, single node, the change signalled where the record is
   written (`_on_dialog_change`, `src/core.cpp:972`). A sipp scenario: subscribe, call, watch the states go by.
4. **PUBLISH**: entity tags and the table above.
5. **The `presence` package**: composition, the registration fallback and its timer, the
   five-second window.
6. **The cluster**: the bus topic, the reconcile, a refresh moving a subscription, takeover
   of a stale node's subscriptions under a lease, stale nodes' open calls left out, and a
   restarting node closing its open call records and resuming its subscriptions. `test/e2e/cluster.sh` gains a watcher on
   one node and a call on the other.
7. **The policy hook**: `watch` in the contract, `builtin://`, the Lua binding,
   `standard.lua`, `tests/policy` in step.
8. **API and events**, for the console.
9. **sipp scenarios and a real client**: the harness (`test/e2e/run.sh`) gains
   subscribe, publish and BLF scenarios, and a softphone that does BLF and presence is run
   against a live node.
10. **Documentation**: a presence section in the docs, the configuration reference, and a
   quick start for BLF on a desk phone.

## What this does not do

- No `pending` subscriptions, no authorisation documents (RFC 5025), no XCAP.
- No `message-summary`: forwarded, as decided.
- No presence derived from calls (on-the-phone in PIDF). The `dialog` package already says
  it, and a softphone that wants it can publish it.
- No partial `dialog-info` and no per-dialog filters.
- No RLS (RFC 4662): a buddy list is one subscription per buddy.
- No NOTIFY sent by a node other than the subscription's holder: a takeover moves the
  holder first.
