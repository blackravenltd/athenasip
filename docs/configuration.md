# AthenaSIP - Configuration

AthenaSIP uses a YAML configuration file. With no `--config`, the first of these that
exists is read:

1. `$ATHENASIP_CONFIG`
2. `/etc/athenasip/config.yaml`
3. `~/.athenasip/config.yaml`

In that order on purpose: what the command line was told beats the environment, the
environment beats the system path a package installs to, and a home directory is last
because it is a person's checkout rather than a service.

## What this node is actually running on

Most of what decides a node's behaviour is a default nobody wrote down, so reading the
file answers a different question from reading the node:

```
athenasip --print-config
```

That prints the effective values - the file, the search path and the defaults all
resolved - as YAML, says which file it came from, and exits without starting a listener or
constructing a driver. A plugin's own section is copied through rather than interpreted,
since only the plugin knows what it means. There are no API tokens to show: a file that
still sets `http.api.tokens` is refused, and administrators are users made with
`athenasip --add-user`.

## Configuration

`config/config.example.yaml` is the annotated reference: every setting appears in it with
its own default and a note on what it is for, so a line deleted from a copy of it changes
nothing. This page is the longer prose for the settings that need it.

A working configuration is much shorter than the example. A node id, one listener and the
driver URLs will run:

```yaml
sip:
  node_id: sip-0001

udp:
  enable: true
  address: 0.0.0.0
  port: 5060

datastore:
  url: "memory://"

events:
  url: "local://"

media:
  url: "builtin://"
```

### `sip` Section

This section configures the server itself.

#### `node_id`

This node's name, which must be unique across the cluster. It identifies the node on the
event bus and in `GET /api/v1/nodes`, so two nodes sharing one is two nodes nobody can
tell apart.

#### `public_address`

The address this node tells the outside world to reach it on, and what it writes into the
`Via`, `Record-Route` and `Service-Route` it generates.

A listener bound to `0.0.0.0` answers on every address the host has and can name none of
them, so without this those fields say `0.0.0.0` and nothing can route back. Leave it
unset on a single-homed host, where the address of the flow itself is right; set it in a
container, behind a load balancer, or on a NAT'd public IP.

#### `localnet`

```yaml
sip:
  public_address: 203.0.113.5
  localnet: ["192.168.0.0/16", "10.0.0.0/8"]
```

The prefixes on this node's side of the router, which is Asterisk's `localnet`. A peer
whose address is inside one is given the node's local address and port, in every `Via`,
`Record-Route`, `Service-Route` and request the node writes to it. Everyone else is given
`public_address`. Without this a phone on the same LAN is handed the public address, which
reaches the node only if the router hairpins, and plenty of routers do not. A bare address
is a prefix of one. A listener bound to `0.0.0.0` gives the address the host would use to
reach that peer.

The builtin media relay follows the same rule: a leg inside `localnet` is told to send its
media to that local address rather than `media.builtin.public_address`, which works when
the relay is bound to `0.0.0.0` or to that address. rtpengine chooses its addresses from
its own interface configuration and is not affected.

#### `public_port`, on each listener

```yaml
udp:
  port: 5060
  public_port: 5080
```

The port a router forwards to this listener, when it is not the one the listener is bound
to. It is what peers outside `localnet` are given and what the node list says. Leave it out
when the ports are the same.

Realms and their nonce secrets are not configured here. They are provisioned over the
admin API - `POST /api/v1/realms` - because a cluster shares them and a file on one node
does not.

#### `allow_unencrypted`

If `true`, the server will reject SIP `INVITE` requests that do not describe encrypted media.
This setting can be `true` even if the TCP server is enabled - in which case the SIP flow will
be unencrypted, but the server will still reject attempts to initate unencrypted calls.


#### `files.spa`

Single-page application mode. Defaults to `true`.

A path with nothing behind it - `/sip/realms`, `/diagnostics/softphone` - is answered
with `index.html` from the document root, so a client-side route survives a reload or a
pasted link. Three things are never answered this way:

- anything under `/api/`, so an unknown API path still 404s rather than returning a web
  page and telling a script that its request succeeded;
- any path whose last segment names a file extension, so a missing asset stays missing
  instead of hiding behind a 200 with HTML in it;
- anything but `GET` and `HEAD`, because a write to a path that does not exist is not a
  page view.

Set `false` for an ordinary document root.

#### `log_messages`

If `true`, every message in and out is logged in full rather than by its first line.
Defaults to `false`.

The first line is the readable trace and is always logged. This adds the headers and
the body, and the body is the reason to want it: a session description is the one thing
a node is better placed to show than either end of a call, and reading it off both
endpoints instead is what the interop runbook otherwise has to tell you to do. It is
also most of the bytes, which is why it is asked for rather than assumed.

`Authorization`, `Proxy-Authorization`, `WWW-Authenticate` and `Proxy-Authenticate` are
logged as `<redacted>`. A Digest response is a hash rather than the password, but it is
replayable for as long as its nonce lives, and a challenge carries the nonce the next
response is computed over. The line itself is kept, because knowing that a request
carried credentials is part of reading the exchange.

#### `media_timeout`

How long a call may carry no media at all before this node stops holding it open, in
seconds - Defaults to `300`. Zero turns it off.

A phone that loses power sends no BYE. A call between two endpoints that never negotiated
a session timer (see `session_min_se`) has no expiry of its own either, so nothing in the
signalling plane will ever say that call ended - and the node goes on holding its dialog,
its call record and its relay ports. The ports come from a finite pool, so left long
enough a node stops being able to anchor new calls. Only a restart clears them.

The media plane knows what the signalling plane cannot: the relay records when it last
carried a packet. RTCP counts as well as RTP, which is what keeps a call on hold, or one
whose codec suppresses silence, from reading as dead - RFC 3550 has reports sent for the
life of the session whether or not there is anything to carry.

Being wrong means cutting the media on a call that is still up, so the default is minutes
rather than seconds. Shorten it only if you know your endpoints.

When it fires the node releases the call and says nothing to either end. RFC 4028 section
8.3 is explicit that a proxy "MUST NOT send a BYE": this node is on the path of the
dialog, not an end of it. Both endpoints run their own timers and will each send their
own when they notice.

Only calls this node anchors are covered. Where the media engine declined the description
- a WebRTC offer at the plain-RTP relay - or media otherwise goes end to end, there is
nothing to observe and the call is left alone.

#### `max_call_duration`

The longest this node will hold any call open, in seconds - Defaults to `0`, which is no
cap at all.

This is the backstop for what the other two mechanisms cannot see. `media_timeout` only
reaches a call this node anchors; `session_expires` only reaches one whose far end
implements RFC 4028. A call that is neither - media end to end, and two endpoints that
have never heard of session timers - is held until the node restarts, and this is the only
thing that catches it.

It is blunt by nature. There is nothing in it about whether the call is alive: set it to
four hours and a legitimate four-hour call is cut. Set it only on a deployment that knows
what its calls look like, and prefer the other two where they reach.

Like them, when it fires the node releases the call and says nothing to either end
(RFC 4028 section 8.3).

#### `require_session_timer`

Whether to insist on a session timer by putting `Require: timer` on a request whose caller
did not advertise support - Defaults to `false`, and it should usually stay that way.

RFC 4028 section 8.1 allows it and calls it NOT RECOMMENDED in the same breath. The
consequence is concrete: an endpoint that does not implement the extension answers
`420 (Bad Extension)`, so the call **fails outright** rather than merely going without an
expiry. That can be a reasonable trade on a closed fleet where every handset is known and
a call with no expiry is the worse outcome. It is the wrong one on anything a stranger can
call.

With it off, a caller that cannot do session timers is still offered one through
`session_expires`, and the callee decides whether the session gets a timer. That is the
same protection without the failure mode.

#### `session_expires`

The session interval this node offers a call that asked for none, in seconds - Defaults to
`1800`. Zero leaves such a call without one.

RFC 4028 section 8.1 lets a proxy add a `Session-Expires` to a request that carried none,
which is how a call gets an expiry when the caller never thought to ask for one. It is
worth having alongside `media_timeout` because the two cover different gaps: the media
sweep only sees calls this node anchors, and this only helps where the far end implements
RFC 4028.

It cannot break a call. A callee that does not implement session timers answers without
one, and section 8.2 then leaves the call with no expiration at all - exactly where it
started. A callee that does implement them takes the refreshing on itself when the caller
cannot, which section 9's Table 2 requires of it. This node never names a refresher: 8.1
forbids a proxy from doing so, and which end refreshes is for the endpoints to settle.

An interval already in the request is left alone, and an inserted one is never lower than
a `Min-SE` the request carried. Section 4 puts the absolute floor at 90 seconds and
recommends 1800; a value below 90 is refused and one below 1800 is taken with a warning.

#### `session_min_se`

The shortest session interval this node will carry a call on, in seconds - Defaults to
`90`.

A session timer (RFC 4028) is how a call that has gone silent - one end powered off, a
network that went away without a BYE - is eventually cleaned up rather than held for
ever. The node keeps state for the length of the interval the two ends agree on, so how
short that interval may be is its business as well as theirs.

A caller that asks for less than this and advertises `Supported: timer` is answered
`422 (Session Interval Too Small)` with this value in a `Min-SE` header, and is expected
to ask again with something longer. A caller that does not advertise support cannot read
that answer, so refusing it would only fail the call: its interval is raised to this
value on the way through instead, and a `Min-SE` is added so the far end knows why.

A call where neither end asked for a session timer still has no interval and is not given
one. RFC 4028 allows a proxy to insert one; this node does not, because ending a call
that nobody said would end is worse than holding its state.

RFC 4028 sets a floor of 90 seconds, a little over twice the longest a SIP transaction
can take, so that a refresh has time to complete before the session it refreshes expires.
A lower value here is refused, with an error in the log, and the default kept.

#### `timers`

The SIP implementation timers and multipliers as specified in [RFC 3261](https://datatracker.ietf.org/doc/html/rfc3261).
Please note for nearly all normal use cases, you should not adjust these from their defaults.

**NOTE:** Timers A to K are the transaction timers of RFC 3261 section 17, and every one
of them is expressed as a multiple of T1 or T4. Timer C is the exception: it belongs to
the proxy rather than to a transaction (section 16.6 step 11), so it is an absolute
value in milliseconds.

##### `t1_rtt_ms`

The SIP Timer T1 (Retransmit) - this is the initial time between retransmits,
and is used by the other timers as a multiplier. Defaults to `500`.

##### `t2_max_retransmit_interval_ms`

The SIP T2 (Max retransmit interval) timeout - Defaults to `4000` (4s).

##### `t4_network_propagation_ms`

The SIP Timer T4 (Max network latency) - Defaults to `5000` (5s).

##### `a_invite_initial`

The SIP Timer A (Client INVITE retransmit interval) multipler of T1 above - Defaults to `1` (500ms).

Controls retransmit interval for INVITE requests.

##### `b_invite_timeout`

The SIP Timer B (Client INVITE max time to receive response) multipler of T1 above - Defaults to `64` (32s).

Maximum time to receive a response.

##### `d_invite_duration`

The SIP Timer D (Server INVITE transaction discard timer, UDP only) multipler of T1 above - Defaults to `64` (32s).

Delay before discarding completed INVITE transaction

##### `e_non_invite_initial`

The SIP Timer E (Client Non-INVITE retransmit interval) multipler of T1 above - Defaults to `1` (500ms).

Controls retransmit interval for non-INVITE requests.

##### `f_non_invite_timeout`

The SIP Timer F (Client Non-INVITE max time to receive response) multipler of T1 above - Defaults to `64` (32s).

Maximum time to wait for a final response.

##### `g_server_invite_initial`

The SIP Timer G (Server Non-INVITE retransmit interval) multipler of T1 above - Defaults to `1` (500ms).

Retransmit interval for final responses to unreliable transport.

##### `h_server_invite_timeout`

The SIP Timer H (Server Non-INVITE max time to retransmit response) multipler of T1 above - Defaults to `64` (32s).

Maximum time to retransmit final response.

##### `i_server_invite_duration`

The SIP Timer I (Server INVITE transaction termination timer) multipler of T4 above - Defaults to `1` (5s).

##### `j_server_non_invite_duration`

The SIP Timer J (Server non-INVITE transaction termination timer) multipler of T1 above - Defaults to `64` (32s).

##### `k_non_invite_duration`

The SIP Timer K (Client non-INVITE transaction termination timer) multipler of T4 above - Defaults to `1` (5s).

##### `c_invite_proxy_ms`

The SIP Timer C (how long a proxied INVITE may go on answering provisionally without
finishing), in milliseconds - Defaults to `240000` (4 minutes).

This is the one timer that is not a multiple of T1 or T4, because it is the proxy's and
not a transaction's (RFC 3261 section 16.6 step 11). It is what gives up on a callee that
starts ringing and then goes silent: Timer B bounds a branch that never answers at all,
but the first provisional response ends Timer B, and from then until Timer C fires
nothing else is watching. When it fires the node cancels the branch, and if the branch
ignores that too it is abandoned and the caller is answered.

RFC 3261 requires this to be larger than 3 minutes. A smaller value is refused, with an
error in the log, and the default kept - a shorter timer would hang up on calls that are
only still ringing.

#### Finding a host by name

There is nothing to configure. When a request has to go to a URI that names a host - a
trunk, `sip:+15551234567@sip.provider.example` - the node looks it up the way RFC 3263
says: NAPTR for which transports the domain offers, SRV for the servers and ports, then
their addresses, and it tries each in turn until one answers. It asks the nameservers in
`/etc/resolv.conf`, five seconds a try and twice round, and keeps each answer for as long as
its TTL says. A URI with an address, or with a port, skips the lookups it makes unnecessary.
A provider that publishes only an A record still works: that is the last step.

### `tcp` Section

This section configures TCP listener for the server. TCP is one possible transport
for SIP.

**WARNING** The TCP transport is unencrypted.

#### `enabled`

Whether to enable the TCP server, defaults to `true` if the section is present.

#### `address`

The address to bind to, e.g. `0.0.0.0`

#### `port`

The port to listen on, defaults to `5061`.

### `tls` Section

This section configures Transport Layer Security for the server.

#### `enabled`

Whether to enable the TLS server, defaults to `true` if the section is present.

#### `cert_pem_filename`

The filename for the PEM format server certificate.

#### `key_pem_filename`

The filename for the PEM format server key.

### `datastore` Section

Where realms, accounts, registrations, nonces and call records live. One of the three
plugin kinds: the URL's scheme picks the driver, and a section named after that driver
carries anything the URL cannot express.

#### `url`

The URL of the datastore. AthenaSIP ships with two:

| Datastore                  | Scheme   | Example URL              |
|:---------------------------|:---------|:-------------------------|
| In memory                  | `memory` | `memory://`              |
|[Redis](https://redis.io/)  | `redis`  | `redis://127.0.0.1:6379` |

`memory://` keeps realms, accounts, registrations, nonces and calls in the server
process. It needs no external service and nothing survives a restart, which makes it
the right choice for a single node you are trying out, and for the tests.

`redis://` is the canonical backend: registrations survive a restart and are shared
across a cluster. `rediss://` and `redis+ssl://` are the same driver over TLS.

### `media` Section

Where the RTP goes. One of the three plugin kinds: the URL's scheme picks the driver,
and a section named after that driver carries anything the URL cannot express.

#### `url`

The URL of the media engine. AthenaSIP ships with two:

| Engine                                              | Scheme      | Example URL                  |
|:----------------------------------------------------|:------------|:-----------------------------|
| Built-in relay                                       | `builtin`   | `builtin://`                 |
|[rtpengine](https://github.com/sipwise/rtpengine)     | `rtpengine` | `rtpengine://127.0.0.1:2223` |

`builtin://` relays plain RTP from the server process and needs nothing installed. It
advertises the `bridge` capability and nothing else, and it declines an offer that asks
for ICE, DTLS or SRTP rather than answering one it cannot carry. That decline is not a
failed call: the description travels on untouched and the media goes end to end.

`rtpengine://host:port` is the canonical engine and the one that does WebRTC, because
ICE, DTLS and SRTP are what a browser requires. It also records and transcodes. The port
is rtpengine's ng control port, `2223` unless its own configuration says otherwise.

#### `builtin`

| Setting | Default | What it is |
| --- | --- | --- |
| `bind_address` | `0.0.0.0` | Where the relay binds |
| `public_address` | `0.0.0.0` | The address written into the descriptions it hands out |
| `port_min` | `22000` | The bottom of the RTP port range |
| `port_max` | `23000` | The top of it |

`public_address` has to be somewhere the endpoints can actually send: `0.0.0.0` in a
`c=` line is a black hole. On a NAT'd host it is the public address, and the port range
has to be forwarded to the host as a contiguous block.

#### `rtpengine`

| Setting | Default | What it is |
| --- | --- | --- |
| `timeout_ms` | `500` | How long to wait for one answer before asking again |
| `attempts` | `3` | How many times to ask in all |
| `media_address` | unset | The address rtpengine should advertise, when it should not choose |

The ng protocol runs over UDP, so a request can be lost. rtpengine caches its answer
against the request's cookie, which makes asking again a request for the same answer
rather than for the work a second time, and this driver asks again rather than failing a
call for one lost datagram. `timeout_ms` bounds one attempt and not the operation:
three attempts at half a second is well inside the thirty-two seconds RFC 3261's timer B
gives a transaction, which a fork with several bindings has to share.

`media_address` is only needed where rtpengine cannot work out for itself which of its
addresses to put in the SDP it hands back. Leave it unset and let rtpengine's own
interface configuration decide.

#### Whether media is anchored, and how

That is behaviour rather than media configuration, and lives in the `behaviour` section
below, where a realm can override it.

### behaviour

```yaml
behaviour:
  media_anchor: true
  media_profile: mirror
  qualify_interval: 0
  rewrite_contact: false
```

[Behaviour](behaviour.md) is the whole of it, with how to set it to match the server you
are moving from. Where the standards leave a choice, this section makes it. Leave it out and the node
behaves as the standards say, with one deliberate deviation: it anchors media when an
engine is configured, which is what most servers in front of rtpengine do and what a
call through NAT needs.

Every realm has the same section, provisioned over the admin API as `behaviour` on the
realm (see [`docs/api/openapi.yaml`](api/openapi.yaml)). A realm overrides only what it
sets and takes the rest from here, so changing this file changes every realm that has
not chosen otherwise. A realm that sets a value to `null` goes back to inheriting it.
The API returns what a realm chose (`behaviour`), what that comes to on the node
answering (`behaviour_effective`), and that node's default on its own
(`behaviour_default`).

`media_anchor` off leaves every session description untouched and lets the media go
end to end, as RFC 3261 16.6 has a proxy do. That is right for two endpoints that can
reach each other and wrong for anything behind a NAT.

`media_profile` decides what the engine is asked to produce for a leg that has not yet
said what it speaks. A leg that has is always answered in kind.

| Value | What a callee is offered | Behaves like |
|---|---|---|
| `mirror` (default) | What the caller offered | A proxy that imposes nothing |
| `transport` | WebRTC over `ws`/`wss`, plain RTP otherwise | Kamailio's usual WebSocket routing |
| `rtp` | Plain RTP | A realm whose WebSocket clients are SIP phones (RFC 7118 requires no WebRTC) |
| `webrtc` | WebRTC | A browser-only realm |
| `srtp` | SRTP with keys in the description (RFC 4568) | Desk phones that want encryption and do not do DTLS |

An account can say what its endpoint is with the same `media_profile` in its own
`behaviour` section, which is Asterisk's `webrtc=yes`: AthenaPhone signals over TCP and
its media is WebRTC, which neither the realm nor the transport can tell. Precedence, for
the first description produced towards a leg, is what that leg has itself said in an
offer or answer or in answer to an OPTIONS, then its account, then its realm, then this
section.

`qualify_interval` is how often, in seconds, each registered client is sent an OPTIONS
down the flow it registered on, which is Asterisk's `qualify` and Kamailio's nathelper
ping. 0, the default, sends none, because RFC 3261 does not ask a registrar to; otherwise
it is 5 to 86400. The probe keeps a NAT's mapping for the client open, and a client that
answers with a session description (RFC 3261 11.2) has said what media it takes, which
then counts as the client's own word: it decides the first offer towards it ahead of the
account, the realm and this section. `GET /api/v1/qualify` lists the clients being probed,
when each last answered and what it said.

`rewrite_contact`, off by default, rewrites the Contact in what this node forwards to the
address and port the message came from, as Asterisk's `rewrite_contact` does; see
[Behaviour](behaviour.md#rewrite_contact).

When a callee answers an offer the engine produced for it with 488 Not Acceptable Here,
the node offers it the other profile once, WebRTC for plain RTP or the reverse, as a
Kamailio failure route would. Under `mirror` the other profile is the other of what the
caller offered, which is what lets a browser call a desk phone. Nothing is learned from
it: `GET /api/v1/media/reoffers` lists the accounts that needed it and the
`media_profile` that would save them the round trip, and setting it is the operator's
call.

An unknown setting or value is an error at startup and a 400 from the API, never a
guess.

