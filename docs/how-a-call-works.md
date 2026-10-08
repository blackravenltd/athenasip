# AthenaSIP - How a call works

One call, followed through one node: Alice registers, calls Bob, they talk, and Alice
hangs up. Knowing these steps makes the log and most settings readable. Terms are in the
[Glossary](glossary.md).

```
Alice                         AthenaSIP                          Bob
  |-- REGISTER ------------------>|                                |
  |<----------------------- 401 --|                                |
  |-- REGISTER + Digest --------->|  binding stored                |
  |<----------------------- 200 --|                                |
  |                               |                                |
  |-- INVITE (SDP offer) -------->|                                |
  |<----------------------- 407 --|                                |
  |-- ACK ----------------------->|                                |
  |-- INVITE + Digest ----------->|  Bob's bindings looked up      |
  |<----------------------- 100 --|-- INVITE (SDP rewritten) ----->|
  |                               |<------------------------ 180 --|
  |<----------------------- 180 --|                                |
  |                               |<--------- 200 (SDP answer) ----|
  |<---------- 200 (SDP rewritten)|                                |
  |-- ACK ----------------------->|-- ACK ------------------------>|
  |                               |                                |
  |<=========== RTP ============> relay <=========== RTP =========>|
  |                               |                                |
  |-- BYE ----------------------->|-- BYE ------------------------>|
  |<----------------------- 200 --|<------------------------ 200 --|
```

The node is a proxy: it forwards requests and responses, and never answers a call itself.
It can end one, by sending each end a BYE ([Hanging up](#hanging-up)).

Who must prove themselves and where a call goes are the policy's decisions. This page
follows the default, `builtin://`, which serves the node's own realms; with `lua://` a
script makes them instead ([Scripting](scripting.md)), and the node carries out the rest
the same way.

## Registration

A phone tells the node where it is with a REGISTER, for a limited time.

1. The first REGISTER carries no credentials. The node answers **401 Unauthorized** with
   a Digest challenge: the realm name and a nonce, a random value that expires.
2. The phone sends the REGISTER again with a Digest response, a hash of its username,
   password, the realm and the nonce. The password itself never crosses the network.
3. The node checks the hash against the subscriber's stored credentials and answers
   **200 OK**. It stores a **binding**: the address of record (`sip:alice@example.com`)
   tied to the phone's Contact and the connection it registered over.

The realm is found from the domain in the REGISTER's `To`. A binding lasts what the phone
asked for, capped by the realm's `registration_timeout` (5000 seconds for a new realm),
and the phone refreshes it before it runs out. `GET /api/v1/registrations` lists bindings.

What the log says:

```
REGISTER with no Authorization - challenging
```

A second challenge in a row means the credentials were wrong; see
[Troubleshooting](troubleshooting.md#cannot-register). The details of Digest are in
[Authentication](authentication.md).

## The INVITE

Alice dials Bob. Her phone sends an INVITE addressed to `sip:bob@example.com`, with an
SDP offer in the body (see [Media](#media) below).

1. **407 Proxy Authentication Required.** Alice's `From` is in a realm this node serves,
   so she must prove who she is. Her phone acknowledges the 407 with an ACK and sends the
   INVITE again with Digest credentials. A phone that registered over TCP, TLS or
   WebSocket and calls on the same connection skips this: that connection is already
   authenticated. Over UDP every call is challenged.
2. **Finding Bob.** The node reads Bob's bindings from the datastore. No subscriber is a
   404; a subscriber with no bindings is a 480.
3. **Forking.** Bob may have several devices registered. The node tries them one at a
   time, most recently registered first. A device that answers with a failure, or does
   not answer at all, moves the call to the next one. The first 2xx wins. When none
   answers, the caller gets the best failure.
4. **Record-Route.** Before forwarding, the node adds itself to the INVITE's
   `Record-Route`, so every later request in the call (ACK, re-INVITE, BYE) comes back
   through it. That is what lets it relay the media and write a call record.
5. **100, 180, 200.** If nothing else has gone back to Alice after 200 ms, the node sends
   `100 Trying` so her phone stops resending. Bob's phone rings and sends `180 Ringing`, which the node passes back. Bob
   answers with `200 OK` carrying his SDP answer, which the node also passes back.
6. **ACK.** Alice's phone acknowledges the 200 with an ACK, which follows the
   Record-Route through the node to Bob. The call is up.

## Media

The voice itself does not travel in SIP. **SDP** (Session Description Protocol) is the
small text body in the INVITE and the 200 that says "send my audio to this address and
port, in these codecs". The INVITE carries the offer, the 200 the answer.

Phones usually sit behind NAT: a home or office router that gives them a private address
such as `192.168.1.20`. The SDP names that private address, which nobody outside the
router can send to. Sent end to end, the call connects and nobody hears anything, or only
one side does.

**Anchoring** fixes this. The node hands each SDP to its media engine, which rewrites the
address and port to its own. Both phones send their audio to the engine, which relays it
on to wherever the other phone's packets actually came from. This is
`behaviour.media_anchor`, on by default.

- **builtin** relays plain RTP from inside the node. It needs `media.builtin.public_address`
  set to an address the phones can reach.
- **rtpengine** is a separate media server. It also converts between plain RTP and the
  encrypted media browsers use (WebRTC), and handles SRTP.

[Media](media.md) covers choosing and configuring the engine.

## Hanging up

Alice hangs up. Her phone sends a BYE, which follows the Record-Route through the node to
Bob, and Bob's 200 comes back the same way. The node releases the media ports, ends the
call and writes a call record (`GET /api/v1/call-records`).

A phone that loses power sends no BYE. When the call's media stops (`sip.media_timeout`)
or it reaches `sip.max_call_duration`, the node sends each end a BYE itself and releases
the call. When a session timer lapses it releases the call and sends nothing, as RFC 4028
8.3 requires. An administrator ends a call the same way with
`DELETE /api/v1/calls/{call}`. See [Ending dead calls](configuration.md#ending-dead-calls).

Hanging up before Bob answers is a CANCEL instead of a BYE. The node stops Bob's ringing
device and answers Alice's INVITE with `487 Request Terminated`.

## A browser over WebSocket

A browser cannot open a raw SIP connection. It speaks SIP over a WebSocket (RFC 7118),
and a page served over HTTPS can only open a secure one, `wss://`.

- **No address to reach.** A browser's Contact is a made-up name, usually ending in
  `.invalid`. The node reaches it only down the WebSocket it registered on, which it
  keeps open. That connection lives on one node; if it closes, the browser must register
  again before it can be called.
- **WebRTC media.** Browsers only send encrypted media, with ICE and DTLS-SRTP. The
  builtin relay cannot handle that, so it passes the SDP through untouched, and media
  goes end to end. Browser to browser that can work; browser to desk phone cannot.
  rtpengine converts between the two, so a browser and a phone can talk.
- **Choosing the media for each side.** When a phone calls a browser, the node has to
  decide what to offer the browser. `behaviour.media_profile` makes that choice; if the
  first offer is refused with 488, the node tries the other kind once. See
  [Behaviour](behaviour.md#media_profile).

## A phone woken by push

Phone operating systems put SIP apps to sleep, closing their connection. A sleeping phone
cannot be reached, so the app asks for push when it registers (RFC 8599).

1. The app registers with `pn-provider`, `pn-prid` and, for some services, `pn-param` on
   its Contact. The node answers 200 and marks the binding for push.
2. When a call comes in, the node sends a push through Apple, Google or the browser's
   push service and holds the INVITE. The log says
   `Pushing to <contact> through <service> and holding the request`.
3. The phone wakes, the app registers again, and the node sends the held INVITE down the
   new connection. In a cluster the app may register through any node.
4. If the app has not registered again within `push.timeout` (10 seconds), the node moves
   on to Bob's next device, or answers Alice with 480.

The node also pushes the app shortly before its registration would expire, so it can
refresh. [Configuration](configuration.md#push) has the settings for each service.
