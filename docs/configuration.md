# AthenaSIP - Configuration

AthenaSIP uses a YAML configuration file. The server will search, in order:

1. ~/.athenasip/config.yaml
2. /usr/etc/athenasip/config.yaml

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

Realms and their nonce secrets are not configured here. They are provisioned over the
admin API - `POST /api/v1/realms` - because a cluster shares them and a file on one node
does not.

#### `allow_unencrypted`

If `true`, the server will reject SIP `INVITE` requests that do not describe encrypted media.
This setting can be `true` even if the TCP server is enabled - in which case the SIP flow will
be unencrypted, but the server will still reject attempts to initate unencrypted calls.


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
