# AthenaSIP - Configuration

AthenaSIP uses a YAML configuration file. The server will search, in order:

1. ~/.athenasip/config.yaml
2. /usr/etc/athenasip/config.yaml

## Configuration

```yaml
sip:
  realm: "sip.athenasip.org"
  nonce_secret: "your_nonce_secret_here"
  allow_unencrypted: true

tcp:
  enable: true
  address: 0.0.0.0
  port: 5060

tls:
  enable: true
  address: 0.0.0.0
  port: 5061
  cert_pem_filename: "../tls/snakeoil.cer"
  key_pem_filename: "../tls/snakeoil.key"

rtprelay:
  enable: true
  address: 0.0.0.0
  public_address: 0.0.0.0
  port_min: 22000
  port_max: 23000

db:
  url: "memory://"
```

### `sip` Section

This section configures the server itself.

#### `realm`

The default SIP realm of the server.

#### `nonce_secret`

The secret for constucting authentication nonces. 

#### `allow_unencrypted`

If `true`, the server will reject SIP `INVITE` requests that do not describe encrypted media.
This setting can be `true` even if the TCP server is enabled - in which case the SIP flow will
be unencrypted, but the server will still reject attempts to initate unencrypted calls.


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

### `rtprelay`

Configures the built-in UDP/RDP relay. The relay exposes pairs of ports and relays UDP packets
between them, sending outgoing packets to the source endpoint of the first packet received. 

#### `enable`

Whether to enable the UDP/RDP relay, defaults to `true` if the section is present.
  
#### `address`

The address to bind to, e.g. `0.0.0.0`

#### `public_address`

The IP address to advertise (and replace in SDP) - this should be a real public IP address.

#### `port_min`

The first port in the available port range for the relay, e.g. `22000`

#### `port_max`

The last port in the available port range for the relay, e.g. `23000`

### `db` Section

This section configures the database.

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
