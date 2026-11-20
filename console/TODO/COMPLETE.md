# AthenaSIP Admin - Completed Work

What exists and works, by release. The commit history has the detail.

## 0.4.2 (2026-10-13)

- **No call without a microphone.** On a page that is not a secure context the phone fails a
  call at once and declines an incoming one with 488, saying why, where JsSIP used to hang.
  The phone run refuses a page that is neither https nor loopback.
- **TURN from the nodes.** A test pins the nodes' TURN entries, with their transport, as
  passed to the browser unchanged.
- **Docs for node 0.12.0.** Presence and BLF added to what the console does not manage yet.

## 0.4.1 (2026-10-08)

- **The sixth role.** The console knows `manage-trunks` (node 0.10.0), so editing a user who
  holds it no longer drops it on save. Manage cluster now says what it permits.
- **Docs for node 0.10.0.** Which node the console needs (0.9.0 or later), and what the node
  does that the console does not manage yet: routing scripts and their reload, trunks,
  attributes, ending calls and the event stream.

## 0.4.0 (2026-10-08)

- **The Phone.** A top-level browser phone for any signed-in user, held by the shell so a call
  outlives a change of screen: dial pad and tones, audio and video, answer, decline, mute,
  hold, ringing and notifications, device choice, a silence warning, recent calls from
  `/call-records` and a realm directory. Remembers everything but the password, and reads
  nothing its user's roles do not allow.
- **The phone run.** `e2e/phone-call.spec.ts` calls a real phone with video and asserts the
  media. The harness page takes `video=1`, and the readout gains `video()`.
- **Signed with the line.** The Phone and the softphone read `/subscriber/{realm}/config` with
  HTTP Digest using the line's own SIP credentials (SHA-256, MD5 over plain http), so the
  console user needs no role for it. Follows the node's removal of `/client/config`.
- **The suite.** `npm run test:athenasip` for the server's `test/suite/run.sh`, and
  `e2e/node.spec.ts`: sign-in and roles, provisioning through the Digest config, and the
  console's Phone registering and calling against a live node with `/events` watched.
- **Password recovery** named on the sign-in page: `athenasip --reset-password`.
- **Docs and comments** rewritten short and current.

## 0.3.0 (2026-10-03)

- **Sign-in and users.** Username and password, five roles, role-aware navigation, a Users
  screen, your own page at `/me`, session expiry warning. No configured tokens.
- **Realms and subscribers** with inheritable behaviour: media anchoring and profile, OPTIONS
  probing, contact rewriting; a subscriber's own media profile.
- **Live view.** Calls with per-leg rates and one-way audio flagged; the media engine and its
  re-offers; probed clients; the cluster on Overview.
- **Node 0.8.0.** `/subscribers`, cascading realm delete, rate limits handled everywhere.
- **The softphone.** ICE and WebSocket from `/client/config`, wss from https, video, a working
  far-end meter, a plain message when the page is not a secure context.
- **The relay phase** of the browser-to-browser run, through coturn.

## 0.2.0 (2026-09-23)

- **Rebuilt** on Vite, TypeScript and Vitest with macha-client's visual language, against the
  server's admin API through `AdminApi`, with an in-memory fake for development and tests.
- **The softphone** as a driveable controller and harness page (`softphone.html`), and the
  first browser-to-browser call through AthenaSIP and rtpengine.

## Before 0.2.0

A Create React App prototype against hardcoded fixtures, replaced by the rebuild.
