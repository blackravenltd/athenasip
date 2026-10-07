# AthenaSIP - Behaviour

`behaviour` holds the choices RFC 3261 leaves open and SIP servers make differently:
whether media goes through the server, what a callee is offered, whether registered
clients are pinged, and whether Contacts are rewritten.

The defaults follow the standards with one deviation: media is anchored when a media
engine is configured, which is what a call through NAT needs.

## Where it is set

Three levels, each overriding only what it sets:

| Level | Where | Settings |
|---|---|---|
| Server | `behaviour:` in the config file | all four |
| Realm | `behaviour` on the realm, over the admin API | all four; `null` inherits the server's |
| Subscriber | `behaviour` on the subscriber, over the admin API | `media_profile` only |

```yaml
# config.yaml: the server's defaults
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

# A subscriber whose endpoint is WebRTC
curl -X PUT http://127.0.0.1:8080/api/v1/realms/example.com/subscribers/1001 \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"behaviour":{"media_profile":"webrtc"}}'
```

A realm read from the API carries three versions:

| Field | Is |
|---|---|
| `behaviour` | What the realm set; `null` for what it inherits |
| `behaviour_effective` | The result on the node that answered |
| `behaviour_default` | That node's server default |

In the configuration file, a value the setting does not take stops the node at startup,
and a setting it does not know is logged as a warning and ignored. The API answers 400 to
either.

## The settings

### media_anchor

| Value | Effect |
|---|---|
| `true` (default) | When a media engine is configured, it rewrites each SDP so both ends send to it, and relays. |
| `false` | SDP is untouched and media goes end to end (RFC 3261 16.6). Fails for endpoints behind NAT. |

### media_profile

What the engine produces for a leg that has not yet said what it speaks.

| Value | A callee is offered |
|---|---|
| `mirror` (default) | What the caller offered |
| `transport` | WebRTC over `ws` and `wss`, plain RTP over anything else |
| `rtp` | Plain RTP (RTP/AVP) |
| `webrtc` | WebRTC: ICE, DTLS-SRTP (UDP/TLS/RTP/SAVPF) |
| `srtp` | SRTP with keys in the SDP (RFC 4568, RTP/SAVP) |

The first of these that says anything decides:

1. What the leg said in an offer or answer during this call.
2. What it said in SDP answering an OPTIONS (see [`qualify_interval`](#qualify_interval)).
3. Its subscriber's `media_profile`.
4. Its realm's `media_profile`.
5. The server's `media_profile`.

When a callee refuses the engine's offer with 488, the node offers the other profile once:
WebRTC for plain RTP, or the reverse. Under `mirror`, "the other" is the other of what the
caller offered. Nothing is remembered: `GET /api/v1/media/reoffers` lists the subscribers
that needed the second offer and the `media_profile` that would avoid it.

The builtin relay produces plain RTP only, so behind it there is no second offer and the
488 goes back to the caller. `webrtc`, `srtp` and browser-to-phone conversion need
rtpengine ([media configuration](configuration.md#media)).

### qualify_interval

Seconds between OPTIONS to each registered client, down the flow it registered on. `0`
(the default) sends none; otherwise 5 to 86400.

- The probe keeps the client's NAT mapping open.
- A client that answers with SDP (RFC 3261 11.2) has said what media it takes, which
  counts as its own word under `media_profile`.
- Any final answer, including 405, counts as present. An unanswered probe is counted and
  probing continues.

`GET /api/v1/qualify` lists the clients probed on each node, when each last answered and
what it said.

### rewrite_contact

| Value | Effect |
|---|---|
| `false` (default) | Contacts are forwarded as written (RFC 3261 16.6). |
| `true` | The Contact in each forwarded request and response is rewritten to the address and port the message came from. The user part and parameters stay. |

For endpoints behind NAT whose Contact names a LAN address, talking to a peer that ignores
Record-Route. WebSocket clients are never rewritten (RFC 7118). The realm of the call
decides, and in-dialog requests keep what the call started with.

## Matching another server

AthenaSIP is a proxy. Asterisk and FreeSWITCH are back-to-back user agents, so these
reproduce what clients and media see, not a setting-for-setting mapping. Check the quoted
defaults against the version and configuration you are moving from.

| Their setting | AthenaSIP |
|---|---|
| Kamailio `natping_interval`, Asterisk `qualify_frequency`, FreeSWITCH `nat-options-ping` | `qualify_interval` |
| Kamailio `fix_nated_contact()`, Asterisk `rewrite_contact=yes` | `rewrite_contact: true` |
| Asterisk `webrtc=yes` on an endpoint | subscriber `media_profile: webrtc` |
| Asterisk `media_encryption=sdes` | subscriber `media_profile: srtp` |
| FreeSWITCH `bypass-media` | `media_anchor: false` |
| A Kamailio failure route retrying a 488 with other rtpengine flags | Built in |

### A plain RFC 3261 proxy

```yaml
behaviour:
  media_anchor: false
  qualify_interval: 0
```

### Kamailio or OpenSIPS with rtpengine

```yaml
behaviour:
  media_anchor: true
  media_profile: transport   # ICE and DTLS towards ws/wss, plain RTP elsewhere
  qualify_interval: 0        # or your natping_interval
  rewrite_contact: false     # true where the script called fix_nated_contact
```

### Asterisk (res_pjsip)

```yaml
behaviour:
  media_anchor: true
  media_profile: rtp
  qualify_interval: 0        # or your qualify_frequency, commonly 60
  rewrite_contact: false     # true where endpoints had rewrite_contact=yes
```

Then set `media_profile` to `webrtc` or `srtp` on each subscriber whose endpoint had
`webrtc=yes` or `media_encryption=sdes`.

### FreeSWITCH

```yaml
behaviour:
  media_anchor: true         # false where you used bypass-media
  media_profile: transport
  qualify_interval: 0        # 30 or so where nat-options-ping was on
```
