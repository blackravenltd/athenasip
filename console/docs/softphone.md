# AthenaSIP Admin - The phone, the harness page and the end-to-end runs

One controller, `src/softphone/Softphone.ts`, registers a subscriber over the node's
WebSocket transport and places or answers one call at a time. Three things use it.

| | Where | For |
| --- | --- | --- |
| The Phone | `/phone` in the console | Making and taking calls. |
| The diagnostic softphone | `/diagnostics/softphone` in the console | Proving a node by hand, with the full negotiation on screen. |
| The harness page | `/softphone.html`, a plain file with no console around it | The end-to-end runs. |

All three need a secure context: https, or `localhost`. On any other page the browser gives
no microphone, so a call fails at once with a message saying so, and an incoming call is
declined with 488.

## The Phone

Any signed-in user can open it. It signs in to a line as a subscriber, separately from the
console: SIP address and password. It stays registered, and keeps a call up, while other
screens are used, until the user signs out of the console.

- **Calling.** A bare number or name dials that user in the line's own realm. Audio or video,
  answer or decline, mute, hold, and dial pad tones sent as RFC 4733 events in the media.
- **Devices.** Microphone, camera and speaker are chosen on the page. Allow the microphone
  before registering: changing a browser permission reloads the page.
- **Ringing.** Tones are generated in the browser. A browser notification, once allowed,
  announces a call while the tab is hidden.
- **Remembered** in the browser: the SIP address, the WebSocket and the devices. Never the
  password.

Where to connect and what to use for ICE come from `GET /subscriber/{realm}/config`, signed
with HTTP Digest using the line's own SIP address and password, so the console user needs no
role for them. The phone asks when it registers with the WebSocket field left empty, and again
as each call is placed or answered, because TURN credentials expire. The password is held in
memory while the line is in use. The request is sent with `credentials: 'omit'`, so the
browser does not offer its own login box on the challenge. A console served over plain http
answers with MD5, because a browser gives WebCrypto's SHA-256 only to a secure context.

The rest uses the console user's roles, and each part says so when a role is missing:

| Reads | Role | Without it |
| --- | --- | --- |
| `GET /call-records` | View cluster status | No recent calls. |
| `GET /registrations` | View cluster status | The directory does not show who is registered. |
| `GET /realms/{realm}/subscribers` | Manage subscribers | The directory lists only who is registered now. |

On an https page the phone signals over the node's `wss` listener (`websocket_uri`); on an
http page over `ws`. A node's TURN entries carry their transport (`?transport=udp` or `tcp`)
and are handed to the browser as given.

## The harness page

`/softphone.html` and the diagnostic softphone are the same component. The harness page calls
no API: everything it needs arrives in its query string.

### Query string

| Parameter | Meaning |
| --- | --- |
| `ws` | The WebSocket URL to register over. |
| `uri` | The SIP URI to register as. |
| `password` | Its password. |
| `target` | The SIP URI the Call button dials. |
| `register=1` | Register on load. |
| `answer=1` | Answer a call as it arrives. |
| `ice` | A JSON array of ICE servers, as `GET /subscriber/{realm}/config` gives `ice_servers`. A `turn:` entry with no credential, or an expired one, is dropped. |
| `relay=1` | Media only through TURN (`iceTransportPolicy: "relay"`). |
| `video=1` | Send the camera on calls placed and answered. |

`password` and `ice` are removed from the address bar once read. Nothing is stored.

### Readout

While mounted, the page sets `window.__athenaSoftphone`:

| Member | Gives |
| --- | --- |
| `version` | `1`. Bumped when a field changes meaning. |
| `state()` | `registration`, `call`, `direction`, `remoteIdentity`, `cause`, `localSdp`, `remoteSdp`, the signalling, ICE and connection states, `notice`. |
| `history()` | Every state change since load, oldest first, with timestamps. |
| `stats()` | A promise of the browser's counters. Top level is audio: packets and bytes each way, packets lost, the far end's level and accumulated energy, codec, DTLS state, and the selected candidate pair. `video`, when the call has any: `packetsSent`, `packetsReceived`, `framesDecoded`, `codec`. |
| `video()` | Each end's video m-line (`port`, `direction`, `bundled`) and `outcome`: `accepted`, `bundled`, `declined` (port 0) or `absent`. |
| `call(target)`, `answer()`, `hangUp()` | The buttons, callable. |

`registration` is `unregistered`, `connecting`, `registered` or `failed`. `call` is `idle`,
`calling`, `ringing`, `incoming`, `connected`, `ended` or `failed`.

The DOM mirrors it: `[data-testid=softphone-registration]` and
`[data-testid=softphone-call-state]` carry `data-state`, and the buttons are
`softphone-register`, `softphone-call`, `softphone-answer`, `softphone-hangup` and
`softphone-unregister`.

The query string, the readout and the environment names below are a contract with the
server's repository. Change them in both.

## The end-to-end runs

Playwright drives Chromium with fake media devices, so no hardware and no permission prompt
is needed. The node and its fixture belong to the server's repository; nothing here starts or
stops them.

### Browser to browser

```
(cd .. && test/interop/browser.sh)
```

`browser.sh` brings the interop fixture up with rtpengine, runs `e2e/browser-call.spec.ts`
and takes the fixture down. Two pages register as the fixture's subscribers and call each
way. Each test asserts that both ends connect, ICE and DTLS complete, packets flow both ways
and the received audio is not silence. Where the engine advertises a non-loopback address,
each browser must be sending to it.

It runs twice: `--direct`, and `--relay`, where the engine is reachable only through the
fixture's coturn. In the relay phase the spec fetches ICE servers from `/subscriber/{realm}/config`,
signed with the first subscriber's SIP credentials, and opens the pages with `ice` and `relay=1`. A relayed pair is recognised by its local port
lying in coturn's range, not by the browser's candidate label.

Each test writes its descriptions, state history and counters to `e2e/results/`.

| Variable | Default | Meaning |
| --- | --- | --- |
| `ATHENA_INTEROP_API_PORT` | `8080` | The page is opened at `http://127.0.0.1:<port>/softphone.html`. |
| `ATHENA_INTEROP_WS_PORT` | `8088` | The socket is `ws://127.0.0.1:<port>`. |
| `ATHENA_INTEROP_PUBLIC_ADDRESS` | `127.0.0.1` | The address the node advertises. |
| `ATHENA_INTEROP_REALM` | the public address | The subscribers' realm. |
| `ATHENA_INTEROP_SUBSCRIBERS` | `1001,1002` | The subscribers. The console uses the first two; the rest are AthenaPhone's. |
| `ATHENA_INTEROP_PASSWORD` | `athenaphone` | Their password. |
| `ATHENA_INTEROP_RESULTS` | `e2e/results` | Where records are written. |
| `ATHENA_INTEROP_PAGE_URL` | `http://127.0.0.1:<api port>` | Where the page is served, if not by the node. |
| `ATHENA_INTEROP_WS_URL` | `ws://127.0.0.1:<ws port>` | The socket, outright. |
| `ATHENA_INTEROP_RTPENGINE_ADVERTISE` | the public address | The address rtpengine advertises. Any other value selects the relay phase. |
| `ATHENA_INTEROP_TURN_MIN`, `_MAX` | none | coturn's relay port range. Required in the relay phase. |

If the default ports are taken, `up.sh` suggests alternates, and `browser.sh` passes every
`ATHENA_INTEROP_*` through:

```
ATHENA_INTEROP_SIP_PORT=15060 ATHENA_INTEROP_TLS_PORT=15061 \
ATHENA_INTEROP_WS_PORT=18088  ATHENA_INTEROP_API_PORT=18080 \
ATHENA_INTEROP_RTP_MIN=23000  ATHENA_INTEROP_RTP_MAX=23020 \
ATHENA_INTEROP_NAME=athenasip-interop-alt test/interop/browser.sh
```

### Browser to phone

`e2e/phone-call.spec.ts` registers one page and calls a device that answers by itself, with
video. It asserts ICE and DTLS connected, audio packets both ways, the video line accepted,
video sent, and a minimum of frames decoded from the phone, then hangs up. The record is
written before the assertions, so a failed call leaves its evidence.

It runs only when `ATHENA_INTEROP_TARGET` is set, and the browser-to-browser spec skips when
it is.

| Variable | Default | Meaning |
| --- | --- | --- |
| `ATHENA_INTEROP_TARGET` | none | The SIP URI to call. Selects this run. |
| `ATHENA_INTEROP_PAGE_URL` | `http://127.0.0.1:<api port>` | Where `softphone.html` is served. |
| `ATHENA_INTEROP_WS_URL` | | The node's socket; `wss://` for a remote node. |
| `ATHENA_INTEROP_IGNORE_TLS` | off | `1` accepts the node's certificate unverified. |
| `ATHENA_INTEROP_REALM` | | The realm the page registers in. |
| `ATHENA_INTEROP_SUBSCRIBERS` | | The first is the page's subscriber. |
| `ATHENA_INTEROP_PASSWORD` | `athenaphone` | Its password. |
| `ATHENA_INTEROP_ANSWER_SECONDS` | `45` | How long the phone has to answer. |
| `ATHENA_INTEROP_MEDIA_SECONDS` | `5` | How long media runs before the counters are read. |
| `ATHENA_INTEROP_MIN_FRAMES` | `30` | Frames that must be decoded from the phone by then. |
| `ATHENA_INTEROP_RTPENGINE_ADVERTISE` | none | When set, the page must be sending to this address. |

```
npm run build
npx vite preview --port 4173 --strictPort &
ATHENA_INTEROP_PAGE_URL=http://127.0.0.1:4173 \
ATHENA_INTEROP_WS_URL=wss://sip.example.org:8089 ATHENA_INTEROP_IGNORE_TLS=1 \
ATHENA_INTEROP_REALM=sip.example.org ATHENA_INTEROP_SUBSCRIBERS=1002 \
ATHENA_INTEROP_TARGET=sip:1003@sip.example.org \
npx playwright test e2e/phone-call.spec.ts
```

### The suite

The server's `test/suite/run.sh` brings the fixture up and calls `npm run test:athenasip` once
per phase, with `ATHENA_SUITE_PHASE` set to `direct` or `relay` and `ATHENA_SUITE_RESULTS`
naming that phase's own directory. The script runs the unit and contract tests (in the direct
phase only), then every Playwright spec, and writes `$ATHENA_SUITE_RESULTS/admin/summary.json`
(`phase`, `passed`, `failed`, `skipped`, `failures`), with the reports, records and traces
beside it. It exits 0 only when
nothing failed, and a test that did not run because an earlier one failed counts as failed.
It needs a live build (`VITE_ATHENASIP_LIVE=true`) in `build/`, and never builds or starts
anything itself.

`e2e/node.spec.ts` runs only when the fixture's administrator is given. It checks sign-in and
roles, and a realm and subscriber round trip through the Digest-signed config (SHA-256 and
MD5). It then drives the console's own Phone in a browser: a user with no roles signs in,
registers with the WebSocket left empty, and calls a harness page, while `/events` must report
the registration and the call. It creates a `suite-*.invalid` realm and `suite-*` users, and
removes them. It signs in four times per phase, within the node's limit of five a minute per
address, and waits out a 429.

`phone-call.spec.ts` needs a device, so in the suite it runs only with `ATHENA_SUITE_DEVICE=1`.
