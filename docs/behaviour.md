# Behaviour

SIP servers disagree about the things RFC 3261 leaves open: whether media goes through
the server, what a callee is offered when nobody knows what it can take, and whether
registered clients are pinged. AthenaSIP puts every one of those choices in one section,
`behaviour`, so it can be set to do what the server it replaces did and what that
server's clients expect.

The shipped default is the standards, with one deliberate deviation: media is anchored
when a media engine is configured, because that is what a call through NAT needs and
what most servers in front of rtpengine do.

## Where it is set

There are three levels, and each one overrides only what it sets:

1. **The server**: `behaviour:` in the config file. A setting left out takes the shipped
   default.
2. **A realm**: `behaviour` on the realm in the admin API. A setting that is `null` takes
   the server's, so changing the server's default changes every realm that has not
   chosen otherwise.
3. **An account**: `behaviour` on the account, which has one setting, `media_profile`.
   That is the one setting that is about an endpoint rather than a realm.

```yaml
# config.yaml: the server's default
behaviour:
  media_anchor: true
  media_profile: mirror
  qualify_interval: 0
  rewrite_contact: false
```

```sh
# A realm that differs
curl -X PUT http://127.0.0.1:8080/api/v1/realms/example.com \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"behaviour":{"media_profile":"transport","qualify_interval":60}}'

# Back to the server's default for one setting
curl -X PUT http://127.0.0.1:8080/api/v1/realms/example.com \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"behaviour":{"qualify_interval":null}}'

# An endpoint the operator knows is WebRTC
curl -X PUT http://127.0.0.1:8080/api/v1/realms/example.com/accounts/1001 \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"behaviour":{"media_profile":"webrtc"}}'
```

A realm read back from the API has three versions of the section:
- `behaviour` is what the realm chose, with `null` for what it inherits.
- `behaviour_effective` is what that comes to on the node that answered.
- `behaviour_default` is the node's own default.

An unknown setting or value never gets a guess. In the config file it stops the node from
starting. From the API it is a 400, and nothing changes.

## The settings

### media_anchor

`true` (the default) puts the node in the media path whenever a media engine is
configured. The engine rewrites each session description so that both ends send media
to it, and it relays between them. `false` leaves every description untouched and the
media goes end to end, which is what RFC 3261 16.6 has a proxy do. That is right for two
endpoints that can reach each other and wrong for anything behind a NAT.

### media_profile

What the engine is asked to produce for a leg that has not yet said what it speaks.

| Value | A callee is offered |
|---|---|
| `mirror` (default) | What the caller offered |
| `transport` | WebRTC over `ws` and `wss`, plain RTP over anything else |
| `rtp` | Plain RTP (RTP/AVP) |
| `webrtc` | WebRTC: ICE, DTLS-SRTP, UDP/TLS/RTP/SAVPF |
| `srtp` | SRTP with the keys in the description (RFC 4568, RTP/SAVP) |

A leg's own word always comes first. In order, the first of these that says anything
decides:

1. What the leg said in an offer or answer during this call.
2. What it said in a session description in answer to an OPTIONS (see
   `qualify_interval`).
3. Its account's `media_profile`.
4. Its realm's `media_profile`.
5. The server's `media_profile`.

When a callee refuses the offer the engine made for it with 488 Not Acceptable Here, the
node offers it the other profile once: WebRTC for plain RTP, or the reverse. Under
`mirror` "the other" means the other of what the caller offered, which is what lets a
browser call a desk phone. The node does not learn from this. `GET /api/v1/media/reoffers`
lists the accounts that needed the second offer and the `media_profile` that would save
them the round trip. Setting it is the operator's decision.

### qualify_interval

Seconds between OPTIONS to each registered client, sent down the flow the client
registered on. `0` (the default) sends none, because RFC 3261 does not ask a registrar to.
Otherwise it is 5 to 86400.

The probe does two things:
- It keeps a NAT's mapping for the client open.
- It tells the node what the client takes. A client that answers with a session
  description (RFC 3261 11.2) has said what media it takes, and that counts as its own
  word under `media_profile` above.

Any final answer, including a 405, counts as the client being there. An unanswered probe
is counted, and probing goes on. `GET /api/v1/qualify` lists the clients being probed on
each node, when each last answered and what it said.

### rewrite_contact

`false` (the default) forwards each Contact as the endpoint wrote it, which is what RFC
3261 16.6 has a proxy do. `true` rewrites the Contact in every request and response this
node forwards to the address and port the message actually came from. The user part and
the parameters stay. This is Asterisk's `rewrite_contact` and Kamailio's
`fix_nated_contact`. It is for endpoints behind a NAT whose Contact names their own LAN,
talking to something that ignores the Record-Route this node writes. A WebSocket client is
never rewritten, because its Contact names nothing reachable on purpose (RFC 7118) and it
is reached through its flow.

The realm decides for the calls it is asked to route, and a request inside a call keeps
what the call started with.

## Configure it like

AthenaSIP is a proxy. Asterisk and FreeSWITCH are back-to-back user agents, which end
each call and start a new one towards the callee, so neither maps onto AthenaSIP
setting for setting. What these examples reproduce is what a client and its media see.
The defaults quoted are the out-of-the-box values of current releases. Check them
against the version you are moving from, and against anything your own configuration
changed.

### A plain RFC 3261 proxy

Nothing in the media path and nothing sent that a client did not ask for:

```yaml
behaviour:
  media_anchor: false
  qualify_interval: 0
```

### Kamailio or OpenSIPS with rtpengine

The usual WebRTC gateway script calls rtpengine on every call and picks its flags from
the transport: ICE and DTLS towards `ws` and `wss`, plain RTP towards the rest. NAT
pinging is the nathelper module's `natping_interval`, which is `0` (off) unless set.

```yaml
behaviour:
  media_anchor: true
  media_profile: transport
  qualify_interval: 0      # or your natping_interval, if you set one
  rewrite_contact: false   # true where the script called fix_nated_contact
```

A failure route that re-tries a 488 with the other flags is what AthenaSIP does by itself
on a 488. Nothing needs configuring for it.

### Asterisk (res_pjsip)

Asterisk terminates the media itself, so leave anchoring on. WebRTC is decided per
endpoint with `webrtc=yes`, which is an account's `media_profile`. Qualifying is the
AOR's `qualify_frequency`, which is `0` (off) unless set.

```yaml
behaviour:
  media_anchor: true
  media_profile: rtp
  qualify_interval: 0      # or your qualify_frequency, commonly 60
  rewrite_contact: false   # true where endpoints had rewrite_contact=yes
```

Then, for each account whose endpoint had `webrtc=yes`:

```sh
curl -X PUT .../api/v1/realms/example.com/accounts/1001 \
  -d '{"behaviour":{"media_profile":"webrtc"}}' ...
```

An endpoint with `media_encryption=sdes` is `"srtp"` in the same way.

### FreeSWITCH

FreeSWITCH relays media unless `bypass-media` is set on the profile, and its WebRTC
clients connect to a profile with a WebSocket binding. Pinging registered clients is the
profile's `nat-options-ping`, which is off unless set.

```yaml
behaviour:
  media_anchor: true      # false where you used bypass-media
  media_profile: transport
  qualify_interval: 0     # 30 or so where nat-options-ping was on
```

## What this page does not cover

The media engine itself (`builtin://` or `rtpengine://`) is in
[configuration](configuration.md#media-section). The builtin relay does plain RTP only, so a
`webrtc` or `srtp` profile needs rtpengine.
