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
Date:
AthenaPhone version / device / OS:
Browser:
Node version:                       athenasip --version
rtpengine version:
Transport AthenaPhone signalled on:  udp / tcp / tls
Fixture address and ports:

Registration:            ok / failed -
Browser calls phone:     connected / ringing only / failed -
  audio browser -> phone: yes / no
  audio phone -> browser: yes / no
Phone calls browser:     connected / ringing only / failed -
  audio phone -> browser: yes / no
  audio browser -> phone: yes / no
Hang up from browser:    both ends cleared / -
Hang up from phone:      both ends cleared / -

rtpengine counters (media-stats.py, during each call):

SDP the browser offered / was answered:

SDP AthenaPhone offered / was answered:

SDP this node produced for each (from the node's log):

Anything the node logged that looked wrong:
```

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
