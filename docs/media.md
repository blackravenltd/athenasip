# AthenaSIP - Media

SIP sets a call up; the voice and video travel separately, as RTP packets. Each end says
where it wants its media sent in the SDP of the INVITE and the 200
([How a call works](how-a-call-works.md#media)). Most phones are behind NAT and name an
address nobody can send to, so by default the node puts a **media engine** in the middle:
it rewrites each SDP to point at the engine, and the engine relays the packets. That is
**anchoring** ([Glossary](glossary.md)).

## The two engines

| | `builtin://` | `rtpengine://host[:port]` |
|---|---|---|
| What it is | A relay inside the AthenaSIP process | [rtpengine](https://github.com/sipwise/rtpengine), a separate media server the node controls over its ng port (default 2223) |
| Plain RTP (desk phones, softphones) | Yes | Yes |
| WebRTC (browsers: ICE, DTLS-SRTP) | No | Yes |
| SRTP with keys in the SDP | No | Yes |
| Browser to phone | No | Yes, it converts between the two |
| Recording | No | rtpengine has the capability; where recordings go is rtpengine's own configuration |
| Needs | Nothing | rtpengine installed and running |

Choose **builtin** for phones and softphones only, or to try AthenaSIP out. Choose
**rtpengine** as soon as a browser, SRTP or recording is involved.

When the builtin relay is handed an SDP it cannot handle (a browser's WebRTC offer, or
SRTP), it declines and the SDP goes through untouched, so media goes end to end. The log
says so:

```
Media engine declined the session description, passing it through - builtin media engine handles plain RTP only: use rtpengine:// for ICE, DTLS or SRTP
```

## builtin

```yaml
media:
  url: "builtin://"
  builtin:
    bind_address: 0.0.0.0
    public_address: 203.0.113.5
    port_min: 22000
    port_max: 23000
```

- `public_address` is the address written into SDP, so it must be one the phones can send
  to: on a host behind NAT, the router's public address. The default, `0.0.0.0`, is an
  address nobody can send to; always set it. It may be a host name, looked up at start
  and every minute.
- Each audio or video stream in a call takes two ports from the range, one for RTP and
  one for RTCP. A call with audio only takes two; the default range holds about 500 such
  calls. When the range runs out the log says `Available ports exhausted` and new calls
  go end to end.
- The relay sends each end's packets to wherever the other end's packets actually come
  from, so it works when a phone's SDP names a private address.

## rtpengine

```yaml
media:
  url: "rtpengine://127.0.0.1:2223"
  rtpengine:
    media_address: 203.0.113.5   # optional
```

Install rtpengine and give it a control port (`--listen-ng`) and a port range
(`--port-min`, `--port-max`). The node sends it a command for each SDP and rtpengine
answers with the rewritten SDP. Leave `media_address` unset to let rtpengine's own
`--interface` setting choose the address it advertises, which is usually right; set it to
override that.

Keep the ng port private between the node and rtpengine: anyone who can reach it can
control the media.

Browsers behind strict NAT also need a TURN server to reach rtpengine. List STUN and TURN
servers in `http.api.ice_servers`; the node hands them to web clients at
`GET /api/v1/subscriber/{realm}/config`, with short-lived TURN credentials when `turn_shared_secret`
is set ([Configuration](configuration.md#httpapi)). `docker/up.sh` brings up rtpengine and
coturn already wired together ([Try it in Docker](quick-start/docker.md)).

### More than one engine

```yaml
media:
  url: "rtpengine://10.0.0.5:2223"
  rtpengine:
    engines: ["10.0.0.6:2223"]
    timeout_ms: 500       # the defaults
    attempts: 3
    ping_interval: 10
```

The engines are a pool. Each new call goes to one chosen from its Call-ID among the engines
that are answering, so calls spread across the pool, and every node with the same pool, in
the same order, makes the same choice. The engine is recorded on the call (`media_engine` in
`GET /api/v1/calls/{call}`), and every later request for that call goes to it, from
whichever node asks.

The node pings every engine every `ping_interval` seconds. An engine that stops answering
gets no new calls, and the call that found it out costs one timeout
(`timeout_ms` times `attempts`, a second and a half by default) before it moves to another. Calls already on that engine
stay there: their media cannot move. The engine takes calls again once it answers a ping.

Give every node in a cluster the same pool, listed in the same order: the choice is made
over the list as written, so two nodes with the engines in a different order place the
same call differently. A call recorded on an engine a node does not
have is left alone by that node rather than sent to another engine.

## Ports to open

| Engine | Open, or forward from the router, to the media host |
|---|---|
| builtin | UDP `port_min` to `port_max` on the AthenaSIP host |
| rtpengine | UDP `--port-min` to `--port-max` on the rtpengine host, and the TURN server's ports if you run one |

Forward the whole range: a call can use any port in it. The SIP ports
([Configuration](configuration.md#listeners)) are separate.

## Public addresses and the local network

A node behind NAT serves two kinds of client: those outside, which must be given the
public address, and those on the same LAN, which often cannot reach the public address
from inside. `sip.localnet` lists the LAN:

```yaml
sip:
  public_address: 203.0.113.5
  localnet: ["192.168.0.0/16"]
media:
  builtin:
    public_address: 203.0.113.5
```

A client inside `localnet` is given the node's local address, for SIP and for media;
everyone else gets the public one. With rtpengine, a leg inside `localnet` is advertised
the node's local address in place of `media_address`. `localnet` applies only when the
node has a public address: `sip.public_address`, or one it discovered
([Clustering](clustering.md#the-nodes-own-address)).

## What gets anchored, and what is offered

Two [behaviour](behaviour.md) settings, set for the server and overridable per realm:

- **`media_anchor`** (default `true`): whether the engine is put in the middle at all.
  `false` leaves every SDP alone and media goes end to end, which fails for phones behind
  NAT. `sip.media_timeout`, which ends calls whose media has stopped, only sees anchored
  calls.
- **`media_profile`** (default `mirror`): what kind of media the engine offers the
  callee. `mirror` offers what the caller offered. `transport` offers WebRTC to clients
  connected over WebSocket and plain RTP to the rest. `rtp`, `webrtc` and `srtp` force one
  kind. A subscriber can have its own `media_profile`, which is how a browser-only
  subscriber is marked `webrtc`. If the callee refuses the offer with 488, the node offers
  the other kind once; `GET /api/v1/media/reoffers` lists who needed that.

[Behaviour](behaviour.md#media_profile) has the full rules.

## Is media flowing?

A call that rings and answers says nothing about whether audio moves. These do. Each API
endpoint needs a session with the `view-cluster-status` role.

**`GET /api/v1/media`** names the engine, whether the node is connected to it, and what it
can do:

```json
{"engine":"rtpengine","connected":true,"capabilities":["bridge","record","transcode"]}
```

With a pool, `connected` is true while any engine answers; it does not say every engine is
up.

**`GET /api/v1/calls`** lists the calls in progress on this node. An anchored call has a
`media` object with `idle_seconds` (how long since any packet) and, per end per stream,
`packets_in`, `bytes_in`, `packets_out` and `bytes_out`. One end with `packets_in` at zero
is one-way audio: that end's packets are not reaching the engine.

**`GET /metrics`** is in the Prometheus text format. With the builtin relay it includes
`athenasip_media_packets_relayed_total`, the packets relayed since start; a number that
does not grow during a call means nothing is being relayed. rtpengine does not report it.

**`test/interop/media-stats.py`** asks rtpengine directly over its ng port what it is
holding, per call and per stream, including the address each stream's packets come from:

```sh
test/interop/media-stats.py --host 127.0.0.1 --port 2223          # every call
test/interop/media-stats.py --host 127.0.0.1 --port 2223 --watch  # once a second
```

Its default port, 22222, is the one the interop fixture uses.

**`sip.log_messages: true`** logs every SIP message in full, so you can read the address
and port each SDP names before and after the engine rewrote it.

[Troubleshooting](troubleshooting.md#one-way-or-no-audio) goes through the usual causes.
