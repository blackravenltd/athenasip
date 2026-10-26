# AthenaSIP - Conferencing

What the node must gain for three or more people to be on one call, and the order it is
built in. Written 2026-10-26 against `develop` at `2d82de9`. The decisions it builds on are
Tom's, in `TODO/ACTIVE.md` under Decisions:

- (2026-09-17) Conferencing is RFC 4579 focus routing. FreeSWITCH `conference` is the first
  focus; an SFU (Janus, mediasoup, LiveKit, or rtpengine publish/subscribe) comes later
  behind the same URI scheme.
- (2026-09-17) `MediaEngine` carries a `conference` capability, and `Call` is multi-party
  from the start: participants, and an optional conference focus.
- (2026-10-03) Web video calling and conferencing are first class; nothing built may assume
  a call is audio only.
- (2026-10-11) All state lives in the datastore.

The questions this design cannot settle on its own are at the end, under Decisions to
make. The steps before them need no answer.

## What a user sees

Three colleagues dial `conf-sales` from their desk phones, or tap it in a softphone, and
hear each other. A fourth joins by dialling the same name; the softphone's roster shows
who is in the room as people come and go. A receptionist's BLF key for `conf-sales` lights
while anyone is in it. A browser joins the same room with video, and sees the others who
have it.

Nothing about a phone has to be configured: a conference is a name in the realm, dialled
like a colleague.

## The shape

RFC 4579 separates the two things a conference needs: a *focus*, the SIP endpoint every
participant has a dialog with, which mixes or forwards the media; and the *conference
URI*, which names the room and is what a participant dials. The focus answers INVITEs to
the conference URI, puts `isfocus` in its Contact so clients know what they joined, and is
the notifier for the `conference` event package (RFC 4575), whose NOTIFYs carry the roster.

This node is a proxy. It never mixes media and is not the focus. Its job is:

1. **To know what a conference URI is**, so a request to one is routed to the focus and
   not to a subscriber, and so a name that is a room is refused to a REGISTER.
2. **To route to the focus**: an INVITE, a SUBSCRIBE for `conference`, a REFER, each
   proxied to the focus as a call to a trunk is, with the node's challenges, Record-Route,
   flow tokens and media anchoring exactly as for any call. Towards the focus the leg is
   the conference URI's; towards the participant it is the participant's as ever.
3. **To hold the conference on the call records**: each participant's call is one record,
   as it is today, with `Call::focus` set to the conference URI, so the API can list a
   conference as the calls whose focus is the same URI. The BLF key for the room follows
   from that, through the `dialog` package, with the room as presentity.
4. **To give the roster to the API**, from the focus's own RFC 4575 NOTIFYs, by being a
   watcher of the conference itself (step 6).

The focus is a **trunk**: a SIP peer the node sends calls to, with credentials, a URI, an
outbound proxy, TLS and `inbound_addresses`, kept over `/api/v1/trunks` and read by the
routing policy. Nothing of the trunk mechanism changes; a focus is a trunk whose
`attributes` say so:

```json
{
  "name": "rooms",
  "uri": "sip:freeswitch.example:5080;transport=tcp",
  "inbound_addresses": ["10.0.0.5/32"],
  "attributes": {
    "focus": {
      "prefix": "conf-",
      "realms": ["example.com"],
      "profile": "default"
    }
  }
}
```

`athenasip.trunks` reads it, as it reads `prefixes` and `numbers`: a Request-URI whose
user starts with `conf-` in one of the realms named (every realm when none is) goes to the
trunk with the user part kept, `conf-sales@freeswitch.example`, which a FreeSWITCH dialplan
hands to `conference conf-sales@default`. This is the same shape as the SFU to come: a
different trunk, the same attribute, the same script.

Nothing the node does is specific to FreeSWITCH. What FreeSWITCH needs is a dialplan
extension matching `^conf-(.*)$` that runs `conference $1@default` and a SIP profile that
accepts the node as a trusted peer (its `inbound_addresses` the other way round). The
quick start for conferencing writes that dialplan out.

## Where it plugs in

**Routing** (`src/policy/`, `scripts/athenasip/trunks.lua`). `trunks.route` already sends a
number out by the trunk that carries it. It gains: a Request-URI that is a conference URI
of a focus trunk in the caller's realm goes to that trunk by `a.route.trunk(trunk, {user =
request.uri.user, focus = request.uri})`, for any method. `builtin://` does not: what a
room is belongs to policy (decision 2), so a node that wants rooms runs `lua://`, and the
builtin policy goes on reading trunks only to answer their challenges. The node reads the
`focus` attribute itself only for the API's views (the rooms a client may dial, and the
focuses as a view on the trunks).

**The target** (`policy::Target`, `src/policy/policy.h`; `Proxy::Target`, `src/proxy.h`).
A target gains `focus`, the conference URI the request is for. The proxy sets
`Call::focus` from it when the call is registered (`Core::call_register`), which the
record already carries (`src/datastores/redis_datastore.cpp`, `obj["focus"]`). Nothing
else in the proxy changes: a call to a focus is a call to a trunk.

**The registrar.** A REGISTER for an address of record that is a conference URI of the
realm is refused 403: a room is not a subscriber. The registrar asks the policy's
`register` hook already; `standard.lua` refuses the name, and `builtin://` knows no rooms.

**The room's name.** The call records the room as `sip:` + the user part as dialled + `@`
+ the host lowercased, with no port, parameters or headers (`SIPUri::canonical_address`),
and the API canonicalises the `{uri}` it is asked for the same way, so a client that dialled
`sip:conf-sales@EXAMPLE.com;transport=ws` finds its room as `sip:conf-sales@example.com`.

**The notifier** (`src/notifier.*`). A SUBSCRIBE for `conference` is forwarded to the
focus by the routing above, as any package the node is not the notifier for is forwarded
today. For the `dialog` package with the room as presentity, the notifier counts the calls
whose `focus` is the room: `Notifier::dialogs_of` takes a presentity and the open calls;
it gains the rule that a call's focus is a presentity of its own, confirmed while anyone
is in the room.

**The roster** (step 6). The node subscribes to the conference at the focus, as a watcher,
once a first participant's call to it is confirmed, through the local UA (`src/local_ua.*`
builds a request of the node's own in a dialog; this is a new dialog, so it is the trunk
registrar's model instead, `TrunkRegistrar::_send`, a UAC of the node's own with its
Call-ID and CSeq). The NOTIFYs' `conference-info` (RFC 4575 section 5) are parsed for the
`users` and kept on a record beside the calls, read by `GET /api/v1/conferences/{uri}`.
The subscription ends when the last participant's call closes.

**The media engine.** Unchanged for a focus elsewhere: each participant's leg is anchored
towards the focus as towards a carrier, and a browser's WebRTC is converted to what the
focus takes by the engine, as `media_profile` on the trunk says (`rtp` by default). The
`conference` capability stays false on both engines until an engine mixes itself (step
8), which is what makes this node a focus and is out of this design's scope.

## State, and where it lives

| Fact | Record | Written by |
|---|---|---|
| A focus and its rooms | The trunk, `attributes.focus` | The operator, over the API |
| Who is in a room, as the node sees it | Each participant's call record, `focus` | The node the participant reached |
| Who is in a room, as the focus says | `conference:<uri>` beside the calls, the last `conference-info` | The node watching the focus, under a lease named by the room |
| The node's subscription to the focus | Its dialog, as a trunk registration is held: the node's own, not a record | The watching node |

One node watches each room at a time, under a lease as one node registers a trunk; when
it stops, the next participant's node takes the lease and subscribes afresh. The focus's
NOTIFYs are the signal and the record; the bus carries `conferences/<uri>/changed` so a
console on another node reloads.

## The API

- `GET /api/v1/conferences`: every room with anyone in it: the URI, the focus trunk, the
  participants as the calls say (identity, since when) and as the focus says (the RFC 4575
  `users`, when watched), `view-cluster-status`.
- `GET /api/v1/conferences/{uri}`: one room.
- `DELETE /api/v1/conferences/{uri}/calls/{call}`: a participant removed, by the node's own
  BYE to both ends of that call (`LocalUA::hang_up`), `manage-cluster`. Muting and the
  like are the focus's and not proxied.
- `GET /api/v1/subscriber/{realm}/config` gains `conferences`: the prefix a client dials
  and the rooms it may list, for a softphone's "join a room" screen.

The OpenAPI document carries them; the console's screen is the admin session's.

## Configuration

Nothing in `config.yaml`: a focus is a trunk, over the API, so every node sees it. The
realm's `attributes` may name a default room for a group of subscribers (a script's
business, not the node's).

## The order it is built in

Each step ends with its tests passing and is a commit of its own, the tests written from
the RFCs first and watched failing. Steps 1 to 4 need no focus to prove; 5 onward need a
FreeSWITCH, which `docker-compose.yml` gains, and sipp as a participant.

1. **A focus is a trunk**: `attributes.focus` documented in `docs/scripting.md` under
   Trunks without writing Lua, `trunks.is_conference(uri)` and `trunks.focus_for(uri)`, the
   route for any method with `focus` on the target, and the registrar refusing the name,
   in `athenasip.standard` alone (decision 2). `tests/policy`, `tests/registrar_test.cpp`.
2. **`Call::focus` from the target**, shown by `/api/v1/calls` and grouped by
   `/api/v1/conferences`, and the `dialog` package confirmed for the room while a call has
   it as focus (a BLF key for a room). `tests/proxy_test.cpp`, `tests/api`,
   `tests/notifier_dialog_test.cpp`.
3. **A participant removed** over the API, by the local UA.
4. **The subscriber's configuration**: `conferences` in `/api/v1/subscriber/{realm}/config`.
5. **FreeSWITCH in the compose files** and a sipp scenario: three participants join
   `conf-test`, the calls show one focus, one leaves, the room closes when the last does.
   `test/e2e/run.sh --conference`.
6. **The roster**: the node's subscription to the focus under a lease, the `conference-info`
   parser, the record, the bus signal, the API fields. Proved against FreeSWITCH's NOTIFYs.
7. **Video**: a browser (the console's phone) in a room with two desk phones through
   rtpengine, as the Milestone 3 harness did for a call.
8. **An engine that mixes**: the `conference` capability, rtpengine publish/subscribe as a
   basic SFU behind a trunk-shaped URI, then a real SFU. Its own design when it comes.

## Decisions made (Tom, 2026-10-26)

1. **A focus is a trunk.** `/api/v1/focuses` is a view on the trunks that carry a `focus`
   attribute: the same records, listed and read by that name, with the attribute's fields
   beside the trunk's. No record of its own.
2. **`builtin://` does not route conferences.** The point of `lua://` is that this behaviour
   is configurable: conferencing is `athenasip.standard`'s, so a node that wants rooms runs
   `lua://` (the standard scripts need no script written). The node reads a trunk's `focus`
   attribute for its API views only, never to route or to refuse a REGISTER; the equivalence
   test covers the conference tests under `lua://` alone. Step 1 was first built with a
   builtin exception and is taken back to this.
3. **Mute state from the start.** The node watches each room at its focus for RFC 4575
   `conference-info` (step 6) as part of the feature, not an option: the roster the API
   gives is the focus's, with the call records as what the node routed.
4. **The prefix is the operator's.** The quick start shows `conf-` and says a numeric prefix
   such as `8` suits phones that cannot dial letters.

## What this does not do

- The node is not a focus: no mixing, no `isfocus` of its own, no conference factory
  (RFC 4579 5.4). A room exists when the focus accepts the name.
- No RFC 4575 NOTIFYs of the node's own to participants: the focus sends them, and the
  node forwards the SUBSCRIBE.
- No floor control (BFCP), no sidebars, no moderator controls through the node: the focus's.
- No conference across two foci.
