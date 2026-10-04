# AthenaSIP - Live call test

A manual test: a browser calls AthenaPhone on a real device through this node, with
rtpengine on the media path, and a person listens. The browser half is automated by
`test/interop/browser.sh`; this adds a real phone, its signalling transport, and ears.

## Before you start

- A device with AthenaPhone on the same network as this machine: not a guest network,
  and no VPN routing its traffic elsewhere.
- Docker running, and a build of the admin client:
  `(cd ../athenasip-admin && npm run build)`.
- Free ports. AthenaPhone's Asterisk fixture uses 5060, 5061 and 8088 too, so stop it or
  move this fixture (step 1).
- On macOS, allow incoming connections when asked, or the phone's REGISTER never arrives.
- A debug build of AthenaPhone loads its JavaScript over `adb reverse tcp:8081 tcp:8081`.
  Without it the app exits on launch, which looks like a native crash.
- A working microphone as the browser's input device. A virtual loopback device sends
  silence.

## 1. Bring the fixture up

```bash
test/interop/up.sh --rtpengine --admin
```

It advertises this machine's LAN address, publishes every port on `0.0.0.0`, signs a TLS
certificate naming the address, and prints what it settled on. Use those values below.

If the default ports are taken:

```bash
ATHENA_INTEROP_SIP_PORT=15060 ATHENA_INTEROP_TLS_PORT=15061 \
ATHENA_INTEROP_WS_PORT=18088  ATHENA_INTEROP_API_PORT=18080 \
ATHENA_INTEROP_RTP_MIN=23000  ATHENA_INTEROP_RTP_MAX=23020 \
ATHENA_INTEROP_NAME=athenasip-interop-alt test/interop/up.sh --rtpengine --admin
```

The container is named `ATHENA_INTEROP_NAME`, `athenasip-interop` by default; use that
name in the `docker logs` commands below.

## 2. Prove the fixture

```bash
test/interop/smoke.py --host <the address it printed>
```

Expect four transports and four 200s. If it fails, stop and fix the fixture. Add
`--sip-port`, `--tls-port` and `--ws-port` if you moved the ports.

## 3. Register AthenaPhone

| | |
|---|---|
| Server | the address `up.sh` printed, not `127.0.0.1` |
| Port | 5060 for UDP or TCP, 5061 for TLS, or the moved numbers |
| Domain / realm | the same address |
| User | `1001` |
| Password | `athenaphone` |
| CA, for TLS | `tls/ca/snakeca.crt` |

The realm must be the address: the registrar finds a realm by the host in the address of
record (RFC 3261 10.3).

Start with UDP, then repeat on TCP and TLS. Watch it register:

```bash
docker logs -f athenasip-interop
```

## 4. Open the browser end

On this machine, over loopback, whatever address the node advertises:

```
http://127.0.0.1:8080/
```

`getUserMedia` needs a secure origin, which loopback is and the LAN address over HTTP is
not. Register the softphone as `1002` in the same realm.

## 5. Call, each way

1. Browser calls `sip:1001@<address>`. Answer on the phone. Talk in both directions.
2. Hang up from the browser. Both ends clear.
3. Phone calls `1002`. Answer in the browser. Talk in both directions.
4. Hang up from the phone. Both ends clear.

For video, repeat with the softphone's Video option. The phone needs its camera
permission granted; without it AthenaPhone answers a video call with 486.

## 6. Read the counters during each call

In another terminal:

```bash
test/interop/media-stats.py --watch
```

Expect two legs with packets climbing on both, `UDP/TLS/RTP/SAVPF`, a crypto suite,
`DTLS fingerprint verified` and 0 errors:

```
  relayed  209 packets, 13358 bytes, 0 errors
  pvbtm4p2o8   port 23012  UDP/TLS/RTP/SAVPF  AEAD_AES_256_GCM
               from 192.0.2.10:25208  103 packets, 6856 bytes, 0 errors
               DTLS fingerprint verified, ICE, rtcp-mux, DTLS-SRTP
```

One leg at 0 packets means that end never sent. Both at 0 means nothing reached the
engine.

## 7. Capture the session descriptions before hanging up

- The node logs every message in full, with credentials redacted:
  `docker logs athenasip-interop | grep -A 30 "INVITE sip:"`
- The browser: `window.__athenaSoftphone.state()` in the console gives `localSdp` and
  `remoteSdp`; `stats()` gives the selected candidate pair, the codec and the DTLS state.
- AthenaPhone: its own SIP trace.

## 8. Record the result

```
Date:
AthenaPhone version / device / OS:
Browser:
Node version:
rtpengine version:
Transport AthenaPhone signalled on:
Fixture address and ports:

Registration:
Browser calls phone:
  audio browser -> phone:
  audio phone -> browser:
Phone calls browser:
  audio phone -> browser:
  audio browser -> phone:
Hang up from browser:
Hang up from phone:

rtpengine counters, per call and leg:
SDP the browser offered / was answered:
SDP AthenaPhone offered / was answered:
SDP the node produced for each:
Anything the node logged that looked wrong:
```

## 9. Take it down

```bash
test/interop/up.sh down
```

Pass the same `ATHENA_INTEROP_NAME` if you moved the fixture.

## If it fails

- **Registration fails but `smoke.py` passes.** The difference is in the client. Compare
  its REGISTER in the node's log with what `smoke.py` sends.
- **The call connects with no audio.** `media-stats.py` says which leg. A leg at 0
  packets never sent. Both at 0 on a LAN usually means a firewall between the device and
  this machine. If packets flow, check the microphone on the sending end.
- **A leg stuck with a handful of packets and DTLS never verified.** Read rtpengine's
  log for that leg's DTLS role and handshake: `docker logs athenasip-interop-rtpengine`.
- **A 488.** The two ends could not agree on media. Capture both descriptions; setting
  the realm's `media_profile` is described in `README.md`.
- **Works on UDP but not on TLS.** The certificate names the address this machine had
  when `up.sh` ran. If the machine has changed networks, bring the fixture up again.
