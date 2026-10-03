# The first call

The last Milestone 3 item, and the one nothing can automate: a browser calls an
AthenaPhone on a real device, through this node, with rtpengine on the media path.
Somebody has to hear it.

The browser half is already automated and passing (`test/interop/browser.sh`), and
AthenaPhone's media is the same WebRTC over any transport it signals on. So what this
adds over the automated run is exactly one thing: AthenaPhone's signalling transport,
and the fact that a person heard the audio. Keep that in mind when something fails -
most of what could break here is already known to work.

Write the result down at the bottom whichever way it goes. A failure recorded is the
next item; a success recorded is the first row of the interop matrix.

## Before you start

- A device on the same network as this machine, with AthenaPhone on it. Not on a guest
  network, and not on a VPN that routes its traffic elsewhere.
- Docker running.
- The ports. AthenaPhone's own Asterisk fixture uses 5060, 5061, 8088 and 8089 on
  purpose, so only one of the two can hold them. Either stop it, or move this one and
  tell the phone the moved numbers.
- macOS will ask whether to allow incoming connections the first time. Say yes, or the
  phone's REGISTER never arrives and everything below fails at step one.

## 1. Bring the fixture up

```bash
test/interop/up.sh --rtpengine --admin
```

It detects this machine's address on its own network, advertises that rather than
loopback, publishes every port on `0.0.0.0`, signs a TLS certificate that names the
address, and prints what it settled on. Everything below uses those numbers. If the
Asterisk fixture has the ports:

```bash
ATHENA_INTEROP_SIP_PORT=15060 ATHENA_INTEROP_TLS_PORT=15061 \
ATHENA_INTEROP_WS_PORT=18088  ATHENA_INTEROP_API_PORT=18080 \
ATHENA_INTEROP_RTP_MIN=23000  ATHENA_INTEROP_RTP_MAX=23020 \
ATHENA_INTEROP_NAME=athenasip-interop-alt test/interop/up.sh --rtpengine --admin
```

## 2. Prove the fixture before involving the phone

```bash
test/interop/smoke.py --host <the address it printed>
```

Four transports, four 200s. If this fails, stop: the fixture is not serving what it
claims and nothing you learn from the phone is about the phone. If it passes and the
phone cannot register, the difference is in the phone, which is the whole point of
having this step.

## 3. Point AthenaPhone at it

| | |
|---|---|
| Server | the address `up.sh` printed, not `127.0.0.1` |
| Port | 5060 for UDP or TCP, 5061 for TLS (or the moved numbers) |
| Domain / realm | **the same address** |
| User | `1001` |
| Password | `athenaphone` |
| CA, for TLS | `tls/ca/snakeca.crt`, which now names this machine's address |

The realm has to be the address, because the registrar looks a realm up by the host in
the address of record and finds nothing under any other name (RFC 3261 10.3 step 2).

Start with UDP. It is the least likely to be the thing that is wrong, and if the call
works on UDP the other transports are a smaller question. Watch it register:

```bash
docker logs -f athenasip-interop-alt
```

## 4. Open the browser end

On **this machine**, at loopback, whatever address the node advertises:

```
http://127.0.0.1:8080/
```

Loopback matters: `localhost` is a secure origin and `getUserMedia` needs one. A page
opened at the LAN address cannot reach a microphone without turning Chrome's origin
checks off, and that is not a thing to do to prove a call works. The SIP WebSocket and
the media still go wherever the node says.

Register the softphone as `1002` in the same realm.

## 5. The call, each way

1. Browser calls `sip:1001@<address>`. Answer on the phone. Talk in both directions.
2. Hang up from the browser.
3. Phone calls `1002`. Answer in the browser. Talk in both directions.
4. Hang up from the phone.

Hanging up from each end in turn is deliberate: a BYE from the callee travels a
different path from a BYE from the caller, and it is what releases the media.

## 6. Read the counters, while the call is up

```bash
test/interop/media-stats.py --watch
```

The engine's own counters are the only thing that says media moved rather than that a
call was signalled - a description the engine declined travels on untouched and the two
endpoints reach each other directly, which looks identical from outside. Run it in
another terminal during the call.

What good looks like: two legs, packets climbing on both, `UDP/TLS/RTP/SAVPF`, a crypto
suite, `DTLS fingerprint verified`, and 0 errors.

```
  relayed  209 packets, 13358 bytes, 0 errors
  pvbtm4p2o8   port 23012  UDP/TLS/RTP/SAVPF  AEAD_AES_256_GCM
               from 192.0.2.10:25208  103 packets, 6856 bytes, 0 errors
               DTLS fingerprint verified, ICE, rtcp-mux, DTLS-SRTP
```

One leg at 0 packets means that end never sent; both at 0 means the engine was asked
and nothing arrived, and the tool says so in as many words.

## 7. Where the session descriptions are

The fixture logs every message in full, bodies included, so the node's own view is the
first place to look:

```bash
docker logs athenasip-interop-alt | grep -A 30 "INVITE sip:"
```

What each end offered and what this node produced for the other are all in there, in
order. Credentials are redacted; nothing else is.

The two ends are the corroboration:

- the browser: `window.__athenaSoftphone.state()` gives `localSdp` and `remoteSdp` in
  the console, and `stats()` gives the selected candidate pair, the codec and the DTLS
  state;
- AthenaPhone: its own log.

Capture all three before hanging up. What each end was offered is the thing to keep,
because a leg is profiled from what it has said and this is the first time a real
client has been the one saying it.

## The record

Fill this in and keep it. It is the first row of the interop matrix.

```
Date:                                2026-09-30
AthenaPhone version / device / OS:   0.2.1, develop a8eb8fa, debug build served by Metro
                                     Blackview A85 (A85_EEA), Android 12, SDK 31
Browser:                             Chrome on macOS, softphone.html from athenasip-admin
Node version:                        0.7.0
rtpengine version:                   9.4.0.0+0~mr9.4.0.0 git-master-1393dbfc
Transport AthenaPhone signalled on:  tcp
Fixture address and ports:           10.35.1.132 - udp/tcp 15060, tls 15061, ws 18088,
                                     api 18080, rtpengine 22000-22100, coturn 3478
                                     (moved off the defaults: athenaphone-asterisk held
                                     5060, 5061, 8088 and 8089)

Registration:            ok - digest MD5, four messages, Service-Route returned
Browser calls phone:     connected (the first attempt was refused 488, see below)
  audio browser -> phone: yes - on the fixed build; no on every attempt before it
  audio phone -> browser: yes - "full sound with feedback"
Phone calls browser:     connected - AthenaPhone's first outbound INVITE to a non-Asterisk
  audio phone -> browser: yes
  audio browser -> phone: yes - on the last call; one earlier call was heard one way only
Hang up from browser:    both ends cleared
Hang up from phone:      both ends cleared

Heard, both ways, both directions, on the build carrying the fix below. Before the fix
every call was silent, and this record keeps the silent attempts because they are what
found the bug.

rtpengine counters (from the engine's own Final packet stats):

  browser calls phone, attempt 2      m7ou3bnvqde6tekveghs
    phone    22032 <> 10.35.1.164:49537  opus/48000/2  1427 p, 84790 b, 0 e
    browser  22050 <> 10.35.1.132:54121  unknown          4 p,   336 b, 0 e, SSRC 0

  browser calls phone, attempt 3      m7ou3pmq5fgtn69flit7
    phone    22088 <> 10.35.1.164:39878  opus/48000/2   611 p, 38578 b, 0 e
    browser  22000 <> 10.35.1.132:62585  unknown          4 p,   336 b, 0 e, SSRC 0

The browser leg is byte-identical across two independent attempts. A third attempt with
the browser forced through coturn (relay=1) failed the same way, and a fourth - the phone
calling the browser - also carried nothing.

  on the fixed build
    browser calls phone   gd54fha7e9drf4jhl9ao
      browser  22042 <> 10.35.1.132:56667  2608 p, 174461 b, 0 e
      phone    22028 <> 10.35.1.164:55324  2584 p, 228435 b, 0 e
    phone calls browser   1umi2v4ip2jq4i2dh8ve  (live, mid-call)
      browser  22076 <> 10.35.1.132:55082  2310 p  AEAD_AES_256_GCM  DTLS fingerprint verified
      phone    22090 <> 10.35.1.164:39692  2292 p  AES_CM_128_HMAC_SHA1_80  DTLS fingerprint verified

SDP the browser offered / was answered:

  offered   m=audio 61974 UDP/TLS/RTP/SAVPF 111 63 9 0 8 13 110 126, a=setup:actpass
  answered  m=audio 22050 UDP/TLS/RTP/SAVPF ..., c=IN IP4 10.35.1.132, a=setup:passive,
            a=rtcp-mux, sha-256 fingerprint, a=candidate:... 10.35.1.132 22050 typ host

SDP AthenaPhone offered / was answered:

  answered  m=audio 49537 UDP/TLS/RTP/SAVPF 111 63 9 0 8 13 110 126
            c=IN IP4 10.35.1.164, a=setup:active, a=rtcp-mux, sha-256 fingerprint
            host candidates only - 10.35.1.164:49537, :43142, and a global IPv6
  offered   step 3, roles reversed; captured by AthenaPhone's SipTrace

SDP this node produced for each (from the node's log):

  toward the phone, FIRST attempt - THE BUG
    m=audio 22000 RTP/AVP 111 63 9 0 8 13 110 126
    c=IN IP4 10.35.1.132
    no a=fingerprint, no a=setup, no a=ice-ufrag, no a=ice-pwd, no a=rtcp-mux

  toward the phone, after the fix (as AthenaPhone's trace received it)
    m=audio 22032 UDP/TLS/RTP/SAVPF 111 63 9 0 8 13 110 126
    c=IN IP4 10.35.1.132, a=setup:actpass, a=rtcp-mux, sha-256 fingerprint,
    a=ice-ufrag / a=ice-pwd, a=candidate:... 10.35.1.132 22032 typ host

Anything the node logged that looked wrong:

  [core] SRTP output wanted, but no crypto suite was negotiated
  [ice] Created candidate pair ... between 172.32.0.30 and 192.168.65.1:61675, type prflx

  The second line is the whole finding. 192.168.65.1 is Docker Desktop's gateway, and it
  appeared on the browser's leg in every attempt.
```

### What this established

A pass, on the second build of the evening. The first build was silent on every call,
and finding out why took the whole session. What the run established, none of which any
automated layer covers:

- AthenaPhone registers against AthenaSIP over TCP with digest auth. Its first
  conversation with anything other than its own Asterisk fixture.
- **Inbound INVITE to a registered AthenaPhone works.** This path had never been
  exercised at all. The node routed down the flow and never consulted the Contact.
- CallKeep presents, rings and answers an inbound call on real hardware.
- The phone negotiates DTLS-SRTP and sends real opus that rtpengine decrypts with zero
  errors - 1427 packets on one call, 611 on another.
- AthenaPhone places an outbound INVITE to this node and the browser answers it.
- Two-way audio, heard by a person, in both call directions, with a BYE from each end.

### Why there was no audio, and the fix

The browser's leg never completed DTLS. ICE reached `connected`, `connectionState` stayed
at `connecting`, and the engine counted 4 packets and 336 bytes on that leg in every
attempt - the caller's leg, whichever end the caller was.

The cause was this node, in `src/media/rtpengine_media_engine.cpp`: the WebRTC profile
put `DTLS=passive` on every command, the answer included. rtpengine, as the answerer
towards an offerer that said `a=setup:actpass`, chooses active and starts the handshake
the moment ICE comes up - which is seconds before any 200 OK exists when the callee is a
phone somebody has to pick up. From the engine's own log of one call:

```
794.741  offer received
794.746  Creating active DTLS connection context     engine goes active toward the caller
794.753  Sending DTLS packet                         ClientHello, 12 ms in
796.000  Sending DTLS packet                         retransmit
796.894  answer received  (our command: DTLS=passive)
796.894  Resetting DTLS connection context
796.894  Creating passive DTLS connection context    role flipped under a handshake in flight
         caller sends STUN only, never a ClientHello, for the rest of the call
```

The answer's `DTLS=passive` reset an active handshake, the SDP then told the caller the
engine was passive, and neither end started again. Any real ring time exposed it; the
automated browser run never saw it because it answers within milliseconds and wins the
race. RFC 5763 section 5 is the rule: the answerer chooses the role, and this node was
overriding a choice the engine had already acted on. The fix is to say `DTLS=passive`
only in the offer, and `tests/media/rtpengine_media_engine_test.cpp` now holds the test,
written from the RFC and watched failing first.

Two wrong explanations were written into this section before the right one, and both are
worth keeping as a warning:

- **Docker Desktop's NAT.** Real - the engine sees a browser on this Mac arrive from the
  gateway `192.168.65.1` as a peer-reflexive candidate - and irrelevant, because the same
  NAT is in the path when the call works. Written down on four reproductions of the
  failure and no positive control; `browser.sh --direct` passed the moment it was run.
- **Unlike legs.** Ruled out by reproducing the stall browser-to-browser with a
  9-second ring. The control that had "ruled out" ring delay earlier had read the
  callee's state, and the broken leg was the caller's.

AthenaPhone's observation that the stall followed the offerer, not the browser, is what
turned it around. What settled it was the engine's own per-leg DTLS timeline at
`--log-level=7`, read next to the commands this node sent it.

One caveat on the last phone-to-browser call, for whoever runs this next: the phone's
inbound audio energy over 74 s was about a fortieth of its own microphone's, with the
level mostly near zero and one burst. Tom reported hearing both ways; the number says the
browser's send was quiet for most of the call. The browser's capture was verified live on
the USB device before that call, so this reads as nobody at the microphone rather than a
fault, but a repeat with somebody speaking into it continuously would close it.

### Defects this found

Ours:

- **The realm's media profile offered a WebRTC-only client plain RTP/AVP.** `FromTransport`
  reads ws and wss as WebRTC and everything else as plain RTP, so AthenaPhone signalling
  over TCP was classified as a desk phone and rtpengine was told to bridge the browser's
  media down. The phone refused with 488, correctly. Worked around for this run with
  `PUT /api/v1/realms/{realm} {"media_profiles":"webrtc"}`.
- **The fix for that is not the setting, and is not decided.** AthenaPhone's registration
  carried `+sip.ice` in its Contact (RFC 5768), and this record first said that profiling a
  leg from that tag would have offered SAVPF with nothing configured. It would, and it
  would do the same to every PJSIP softphone with ICE switched on, which sends the tag by
  default and wants plain RTP/AVP. The tag means ICE; nothing a client registers with says
  DTLS-SRTP. What to offer a callee that has not yet described itself is decision 3 in
  `TODO/ACTIVE.md`.

AthenaPhone's, recorded here because this run is what surfaced them:

- `verboseSipLogging` was wired to nothing - the SIP trace this record depends on did not
  exist when the evening started. Written and fixed during the run.
- Contact advertises `<host>.invalid;transport=ws` on non-WebSocket transports. Masked by
  our flow routing while the connection holds, unroutable the moment it drops. Not fixed.
- A SELF_MANAGED ConnectionService never rings on Android; the call was presented silently
  and one attempt timed out unanswered because of it. Fixed during the run, along with a
  missing `stopRingtone` that the fix would otherwise have exposed.
- It does not send `rport` despite its own documentation saying it does.
- Every account defaults to `stun:stun.l.google.com:19302`, so its outbound INVITE carried
  two `typ srflx` candidates and a `c=` line of a public address - for a call that never
  left the subnet. Two things follow for us: a real AthenaPhone will disclose its public
  address to this node by default and contact a third party to learn it, and its INVITE was
  2340 bytes, which is past the 1300-byte threshold its own UDP transport warns at. Expect
  large REGISTERs and INVITEs from it, and expect fragmentation if it is ever tested on UDP.

Environmental, and worth keeping:

- `adb reverse tcp:8081 tcp:8081` is required for a wireless debugging session. Without it
  RN 0.87 bridgeless kills the process rather than showing the red screen, which looks
  exactly like a native crash on launch.

### The browser calls AthenaPhone on corvus-fi-1 (2026-10-03)

The console softphone as subscriber 1001, in Chrome on a Mac, calling
`sip:athenaphone@10.35.1.20` on the A85 registered over TLS. The node on `corvus-fi-1` runs
the builtin relay, which does plain RTP only, so both descriptions passed through untouched
and the media went directly between the Mac and the phone.

| | First call, 21:02 | Second call, 21:09 |
|---|---|---|
| Signalling | INVITE, 100, 180, 200, ACK, BYE, 200. One INVITE, no 488, no re-offer | the same |
| Offer the phone received | `UDP/TLS/RTP/SAVPF`, the browser's own | the same |
| ICE, DTLS | connected, connected | connected, connected |
| Candidate pair | host to host on the LAN | host to host on the LAN |
| Browser packets | 1310 sent, 1278 received, 0 lost | 1821 sent, 1727 received, 0 lost |
| Audio | phone to Mac sent; Mac to phone was encoded silence | heard both ways (Tom) |

The silence on the first call was the Mac, not the call: its default input device was a
virtual loopback device, so Chrome captured nothing and sent it faithfully. The phone
measured an inbound level of exactly 0.000 with packets arriving at the normal rate. A USB
microphone fixed it.

What it took to make the call at all, each of which is a defect or a gap:

- The console at `http://10.35.1.20:8080` is not a secure context, so the browser gives the
  softphone no microphone and no call can start. The test ran through an SSH tunnel to
  `localhost`. Browser calling from the console's own address needs HTTPS on the admin
  listener.
- Before the first call the AthenaPhone app would not start: a debug build loads its
  JavaScript through `adb reverse`, the phone had dropped off Wi-Fi, and the forward was
  gone. The note above about `adb reverse` is the same thing.
- The node logged "The media engine cannot produce webrtc ... - offering what it can" for an
  offer that was already WebRTC and went through untouched. The warning is for an offer
  the node would have had to convert, and here it is wrong.

## If it fails

- **Registration fails but `smoke.py` passes.** The difference is in the client. Get its
  REGISTER out of the node's log and compare it with what `smoke.py` sends.
- **The call connects and there is no audio.** `media-stats.py` says which leg. A leg at
  0 packets never sent; both at 0 and the engine was asked but nothing arrived, which on
  a LAN is a firewall between the device and this machine.
- **A 488.** The two ends could not agree on media. Capture both descriptions: this is
  the case the per-leg profile work was for, and a 488 here is the interesting failure
  rather than the boring one.
- **Everything works on UDP and not on TLS.** The certificate names this machine's
  address, but only the one it had when `up.sh` ran. If the machine has moved networks
  since, bring the fixture up again.
