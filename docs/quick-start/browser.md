# Quick Start - Calling From a Browser

A browser is a SIP client like any other, with three differences: it signals over a
secure WebSocket, it sends WebRTC media (ICE, DTLS-SRTP) that a desk phone cannot take,
and it often sits behind NAT strict enough to need a TURN server. This guide adds all
three to a node, so that browsers can call each other and call phones.

[Try it in Docker](docker.md) already has all of this wired together. This guide does it
on a host, for a node that a browser on another network can use.

## What it takes

| | Why |
|---|---|
| [rtpengine](https://github.com/sipwise/rtpengine) as the media engine | The builtin relay handles plain RTP only. rtpengine speaks WebRTC, and converts between WebRTC and plain RTP when a browser calls a phone. |
| A secure WebSocket listener | A page served over HTTPS may only open `wss://` |
| A certificate browsers trust | A browser refuses a WebSocket with an untrusted certificate without asking anyone |
| A TURN server, for browsers behind strict NAT | Some networks let no media through except by a relay |

## 1. rtpengine

On Debian or Ubuntu:

```
apt install --no-install-recommends rtpengine-daemon
```

In `/etc/rtpengine/rtpengine.conf`, give it the address to relay on, a control port only
the node can reach, and a range of media ports:

```ini
[rtpengine]
table = -1
interface = 203.0.113.5
listen-ng = 127.0.0.1:2223
port-min = 30000
port-max = 30999
```

```
systemctl restart rtpengine-daemon
```

`interface` is the address rtpengine writes into SDP. Behind NAT, give it the local
address and set the public one in the node's `media.rtpengine.media_address`. Open or
forward UDP `port-min` to `port-max` to the rtpengine host. Keep the ng port private:
anyone who can reach it controls the media.

## 2. A certificate

The secure WebSocket and HTTPS listeners need a certificate naming the host the browser
connects to, from an authority browsers trust. [Certificates](../certificates.md#from-a-public-authority)
has the Let's Encrypt steps. Below, the files are in `/etc/athenasip/tls/`.

## 3. TURN, if browsers will call from other networks

[coturn](https://github.com/coturn/coturn) is the usual TURN server. On Debian,
`apt install coturn`, and in `/etc/turnserver.conf`:

```ini
listening-port=3478
external-ip=203.0.113.5
use-auth-secret
static-auth-secret=<a long random string>
realm=example.com
min-port=31000
max-port=31999
```

Open UDP and TCP 3478 and UDP 31000 to 31999. The node mints short-lived TURN credentials
under the same secret, so nothing long-lived is ever handed to a browser.

## 4. The node

```yaml
media:
  url: "rtpengine://127.0.0.1:2223"

websocket:
  enable: true
  port: 9443
  tls: true
  cert_pem_filename: /etc/athenasip/tls/fullchain.pem
  key_pem_filename: /etc/athenasip/tls/privkey.pem

behaviour:
  media_profile: transport         # WebRTC to browsers, plain RTP to phones

http:
  port: 8080
  tls:
    enable: true
    port: 8443
    cert_pem_filename: /etc/athenasip/tls/fullchain.pem
    key_pem_filename: /etc/athenasip/tls/privkey.pem
  api:
    ice_servers:
      - url: "stun:turn.example.com:3478"
      - url: "turn:turn.example.com:3478"
    turn_shared_secret: "<the same string as coturn's static-auth-secret>"
```

`media_profile: transport` offers WebRTC to clients on a WebSocket and plain RTP to the
rest, which is what a mix of browsers and phones needs. The default, `mirror`, offers the
callee what the caller sent, and a phone that refuses WebRTC is offered plain RTP once
after a 488. [Behaviour](../behaviour.md#media_profile) has the rules.

Restart the node and check it:

```
athenasip --check
```

It should say `ok` for the media engine and for each certificate.

## 5. What a web client needs

A browser client signs in with the subscriber's own credentials and asks the node what to
use:

```
curl --digest -u alice:alice-secret https://sip.example.com:8443/api/v1/subscriber/example.com/config
```

```json
{
  "websocket_uri": "wss://sip.example.com:9443",
  "ice_servers": [
    { "urls": "stun:turn.example.com:3478" },
    { "urls": "turn:turn.example.com:3478", "username": "1760000000:alice_example.com", "credential": "..." }
  ],
  ...
}
```

The request is answered with HTTP Digest, the same scheme and the same credentials as a
SIP REGISTER, so a client that can register can fetch it. `websocket_uri` is where to open
the SIP WebSocket, and `ice_servers` goes to `RTCPeerConnection` unchanged; the TURN
credential in it expires on its own. With [JsSIP](https://jssip.net/), and `config` the
answer above:

```js

const ua = new JsSIP.UA({
  sockets: [new JsSIP.WebSocketInterface(config.websocket_uri)],
  uri: "sip:alice@example.com",
  password: "alice-secret",
});
ua.start();

ua.call("sip:bob@example.com", {
  mediaConstraints: { audio: true, video: true },
  pcConfig: { iceServers: config.ice_servers },
});
```

The page itself must be served over HTTPS, or the browser gives it no microphone or
camera. The node can serve it from `http.files` on the HTTPS listener, which is how the
[AthenaSIP Admin](https://github.com/blackravenltd/athenasip-admin) console's softphone
works.

## When it does not work

[A browser cannot connect](../troubleshooting.md#a-browser-cannot-connect) and
[One-way or no audio](../troubleshooting.md#one-way-or-no-audio) cover the usual causes.
`GET /api/v1/media` says whether the node reaches rtpengine, and
[Media](../media.md#is-media-flowing) has the tools for watching packets move.
