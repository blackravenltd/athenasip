# AthenaSIP - Troubleshooting

Find the symptom, then the log line or response code, then the fix. Log lines are quoted
as the node writes them; `<...>` stands for the part that varies.
[How a call works](how-a-call-works.md) explains the steps these come from.

## Where to look

| Tool | What it tells you |
|---|---|
| The log | Each refusal is logged with its reason and the code sent. Keep `log.level` at `debug` or `info` while investigating. |
| `sip.log_messages: true` | Every SIP message in full, including the SDP. Digest headers are redacted. |
| `athenasip --check` | Tries the datastore, event bus, media engine, every certificate and, in a cluster, each peer. Safe beside a running node. |
| `athenasip --print-config` | The configuration the node actually reads, with defaults filled in, and which file it came from. |
| `GET /api/v1/registrations` | Who is registered, from where, until when |
| `GET /api/v1/calls` | Calls in progress on this node, with media packet counts |
| `GET /api/v1/nodes` | The nodes this node knows of, and their status |
| `GET /api/v1/media`, `/api/v1/media/reoffers` | The media engine; callees that refused the first media offer |

The status endpoints need a session with the `view-cluster-status` role; looking up a
realm or its subscribers needs the realm roles ([Authentication](authentication.md)).

## Cannot register

| Response | Log | Cause and fix |
|---|---|---|
| 401, again and again | `REGISTER for <aor> with a Digest response that does not match - challenging` | Wrong password, or the client's authentication user name differs from its user name. Reset the password over the API. |
| 401, again and again | `REGISTER for unknown subscriber <aor> - challenging` | No such subscriber in that realm. Check the user part and that the subscriber exists under `/api/v1/realms/<realm>/subscribers`. |
| 401, again and again | `REGISTER nonce <nonce> not found or expired - challenging` | The nonce the client answered is unknown. In a cluster, check every node uses the same `redis://` datastore. |
| 403 | `REGISTER for <uri>, a domain not served here, and sip.forward_register is never - 403` | The client registers to a domain that is not a realm on this node. Create a realm with that exact name, or set the client's domain to an existing realm. |
| 404 | `REGISTER for unserved domain <domain> - 404` | The domain after `@` in the client's address is not a realm here. Same fix as 403. |
| 423 | `REGISTER asked for <n>s, below the realm minimum - 423` | The client asked for a shorter registration than the realm's `registration_minimum`. Lengthen it in the client, or lower the realm's setting. |
| 423 | `REGISTER asked for push with <n>s, too brief to be woken in time - 423` | A push client must register for at least `push.refresh` + 60 seconds (240 by default). |
| 555 | `REGISTER asked for <service> push, which is not served or lacks what it needs - 555` | The client asked for a push service not in `push.urls`, or its Contact lacks `pn-param` where the service needs one. See [Push does not wake a phone](#push-does-not-wake-a-phone). |

No response at all usually means the client cannot reach the node: check the listener is
up (the log says `Listening on <address>:<port> (<transport>://)` at start), the firewall, and that the
client uses the transport and port the node listens on.

## Calls fail

| Response | Log | Cause and fix |
|---|---|---|
| 403 | `Request from <caller> authenticated as <user> - 403` | The caller proved it is one subscriber but calls as another. The client's `From` and its authentication user name must match. |
| 403 | `Request from <caller> to <uri>, neither of them here - 403` | Neither end is in a realm on this node, so it will not relay. Usually the caller's domain is not the realm name. |
| 404 | `No subscriber for <uri> - 404` | Nobody of that name in the callee's realm. |
| 480 | `No bindings for <uri> - 480` | The callee exists but is not registered. Check `GET /api/v1/registrations`. |
| 480 | `Nothing to send <uri> to - DNS has no SIP service there` | Calling a domain elsewhere that has no SIP service in DNS, or the node has no nameserver. |
| 480 | `<contact> did not register again within push.timeout - 480` | The callee's phone was pushed and did not wake in time. |
| 408 | `No response from target - trying the next` | The callee's device did not answer at all, and no other device did better. Often a phone whose NAT mapping has closed: set `behaviour.qualify_interval` (say 30) to keep it open. |
| 482 | `Request has been here before with nothing changed - 482` | A routing loop: the request came back to this node unchanged. |
| 483 | `Max-Forwards exhausted - 483` | The request passed through too many servers, usually a loop between two of them. Check what routes to what. |
| 488 | `<uri> refused the offer it was made - offering the other profile` | The callee refused the kind of media offered; the node tries the other kind once. A 488 that reaches the caller means the second offer was refused too, or there was no other kind to offer: the builtin relay offers plain RTP only. See [Media](media.md). |
| 500 | (from the callee's side) | The node never sends 503 for a call. A 503 from the next server is tried against the next DNS target, and if none is left it reaches the caller as 500. |

A callee with several devices: they are tried one at a time, newest registration first.
A device that keeps ringing holds the others back until the caller gives up or the node's
timer C (`sip.timers.c_invite_proxy_ms`, four minutes) runs out.

## One-way or no audio

The call connects but somebody hears nothing. Look at the call while it is up:
`GET /api/v1/calls` shows each end's `packets_in`. An end at zero is not reaching the
media engine. [Media](media.md#is-media-flowing) has the other tools.

| Cause | How to tell | Fix |
|---|---|---|
| `media.builtin.public_address` not set, or set to an address the phones cannot reach | The rewritten SDP (`sip.log_messages`) names `0.0.0.0`, `127.0.0.1` or a private address | Set it to the address phones reach the node on |
| The media ports are not open or forwarded | `packets_in` zero for the far end; SDP looks right | Open UDP `port_min` to `port_max` (builtin) or rtpengine's range, and forward it on the router |
| The engine declined the SDP | `Media engine declined the session description, passing it through - <reason>` | With builtin, a browser or SRTP call needs rtpengine. `no relay ports available` means the range is too small. |
| Media is not anchored | `behaviour.media_anchor` is `false` for the realm (`GET /api/v1/realms/<realm>`, `behaviour_effective`) | Set it to `true` for phones behind NAT |
| LAN phones given the public address | Phones on the node's own network get no audio, outside phones do | List the LAN in `sip.localnet` |
| rtpengine advertises the wrong address | The SDP names an address phones cannot reach | Fix rtpengine's `--interface`, or set `media.rtpengine.media_address` |
| A browser behind strict NAT | Browser calls fail through some networks only | Add a TURN server to `http.api.ice_servers` and set `turn_shared_secret` |

## A browser cannot connect

- **A page served over HTTPS can only open `wss://`.** Enable a secure WebSocket listener:
  `websocket.tls: true`, or `websocket.secure_port` beside the plain one.
- **The browser must trust the certificate**, and it must name the host name the page
  connects to. A browser does not ask about a WebSocket certificate; it just fails. The
  snakeoil certificate in `tls/` is trusted by nobody; use a real one
  ([Certificates](certificates.md#certificates-for-clients)). The node logs
  `TLS handshake failed: <reason>` for each refused attempt.
- **The certificate files must load.** `athenasip --check` shows a `websocket certificate`
  line. At start, `Cannot load WebSocket TLS certificates` (with `websocket.tls`) or
  `Cannot load the certificates for 'websocket.secure_port': set them in the websocket
  section or the tls section` stops the node.
- **The console's softphone** also needs the admin listener on HTTPS (`http.tls`), because
  a browser grants no microphone to a plain HTTP page.
- **It registers but there is no audio.** Browsers send WebRTC media, which the builtin
  relay cannot handle. Use rtpengine ([Media](media.md)).

## Push does not wake a phone

| Log | Cause and fix |
|---|---|
| `REGISTER asked for <service> push, which is not served or lacks what it needs - 555` | The service is not in `push.urls`, or the client sent no `pn-param` where the service needs one, or (APNs) the app belongs to another team. |
| `Push service <url> did not start` at startup | The service's section is incomplete or its key file cannot be read. The node does not start. |
| `The push to <contact> failed - <reason>` | The push service refused. Check the service-account file (FCM), the key, key ID, team ID and `environment` (APNs: `sandbox` for a development build), or the VAPID key (Web Push). |
| `<contact> did not register again within push.timeout - 480` | The push was sent, but the app did not register again in time. Raise `push.timeout` (up to 30 seconds), and check the app registers on waking. |

An iOS app using VoIP push is never pushed to refresh its registration; it must do that
itself. [Configuration](configuration.md#push) has each service's settings.

## Cluster nodes do not see each other

Run `athenasip --check` on each node; it reports the bus, every peer, and what peers
found when they tried this node.

| `--check` says | Cause and fix |
|---|---|
| `FAIL  events ...` | The broker is unreachable. Check `events.url`. |
| `peers  no other node has said it is up on the event bus`, with another node running | The nodes are not on the same bus: a different broker, a different `events.mqtt.prefix`, or `events.url` left at `local://`. |
| `FAIL  peer <node>  <address>:5062 - Connection refused` | The peer's inter-node listener is not running or a firewall blocks `cluster.port`. |
| `FAIL  peer <node>  ... - certificate verify failed` or similar | The peer's certificate does not name the address in its `cluster.advertise`, or the two nodes' certificates come from different authorities. Issue it again with the right `--san` and `--replace`. |
| `FAIL  reached by peers  <nodes> tried this node's inter-node listener and could not reach it` | Peers cannot connect to this node's `cluster.advertise` address and port. The log says `No other node can reach this node's inter-node listener; it is marked unreachable and is not forwarded to`. |
| `FAIL  public address  sip.public_address is <a> but <stun server> sees <b>` | The configured address is not the one the outside world sees. |

Also check:

- Every node has its own `sip.node_id`. Two nodes with one name also share an MQTT
  client identifier (unless `events.mqtt.client_id` is set), and the broker keeps
  disconnecting one of them.
- All nodes use the same `redis://` datastore. A subscriber made on one node and missing
  on another means they do not.
- At startup, `'cluster' needs ca, cert and key: athenasip --ca-init and --ca-node make
  them (docs/certificates.md)` means a file is not named under `cluster`, and
  `Cannot load the cluster certificates` that one cannot be read.

[Running a cluster](clustering.md) is the setup from the start.
