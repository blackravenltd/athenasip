# AthenaSIP — Applications in Real-Time Voice Translation

*26 October 2026 · revised after reading `develop` at `220cb35`, and again for the SDP extension and translating media engine*

## Context

AthenaSIP is a clusterable, standards-first SIP server from BlackRaven, written in C++ and licensed GPLv3. A single node runs on its own with no database, broker or media server. Redis, MQTT and rtpengine are added when wanted.

The real-time voice translation market has a gap at the telephony layer. The translation engines are now good and cheap: OpenAI gpt-realtime-translate, Gemini 3.5 Live Translate, Azure Live Interpreter, DeepL Voice, Krisp and Palabra. None of them accept SIP calls for translation directly; all of them expect WebSocket or WebRTC streams of PCM audio. Existing bridges, such as Asterisk AudioSocket, FreeSWITCH mod_audio_stream and LiveKit SIP, are built for AI agents rather than two-party translation. The open-source options are also weak at injecting audio back into the call.

## What AthenaSIP is today (from the repo)

**It is a proxy, not a B2BUA.**
- It implements an RFC 3261 section 16 proxy and registrar, with the four transaction state machines.
- The conferencing design states it plainly: the node never mixes media and is never the focus.
- Calls are serial-forked, Record-Routed dialogs between the two real endpoints.

**It does anchor media, through a plugin.** `MediaEngine` is a plugin kind selected by URL scheme. There are two drivers:

| | `builtin://` | `rtpengine://` (pool) |
|---|---|---|
| What it does | Relays plain RTP packets in-process | Controls rtpengine over the ng protocol |
| Decodes audio? | No — packet relay only | Inside rtpengine (transcode, DTMF tones) |
| WebRTC / SRTP | No | Yes |
| Capabilities advertised | bridge | bridge, record, transcode, dtmf |

The contract (`src/media/media_engine.h`) works per call and per participant. It covers:
- SDP `offer` and `answer`
- `release` and `query` (packet and byte counters per leg)
- `play_dtmf` and `dtmf_listen`
- `start_recording` and `stop_recording`
- `join` and `leave` (the `conference` capability is declared but not yet implemented)

The contract is versioned (plugin `API_VERSION` 24) and async. External plugins load as shared libraries at start.

**Relevant pieces already built:**
- **Routing:** Lua routing policy (`lua://`) with a sandbox. `athenasip.trunks` routes out by prefix and in by number with failover. Trunks carry free-form `attributes` that scripts can read.
- **DTMF:** every flavour is handled (RFC 4733, SIP INFO, in-band via rtpengine), and proved against a real carrier.
- **Calls API:** list, get and hang-up calls, plus call records and a per-call media view (`/api/v1/calls`).
- **Events:** events are published over MQTT, and there are Prometheus metrics.
- **Clustering:** nodes share state through Redis and MQTT and forward calls to each other over mutual TLS. A call in progress ends if its node dies.

**Maturity:**
- The live nodes are 0.12.0, and neither is production.
- One real carrier (2talk, NZ) has carried calls.
- An external security review on 2026-10-25 raised 48 findings; most are fixed and the rest are in progress.
- There is no fuzzing harness yet, and no hosted CI.

## Where translation plugs in

**Design (superseded the trunk-routing idea on 26 October):** translation lives in the media plane, behind rtpengine's ng protocol, and endpoints ask for it in SDP.

- **Signalling:** `draft-cully-mmusic-sdp-translate-00`. It defines one media-level SDP attribute, `a=translate:request|refuse|active`. Languages come from RFC 8373's `hlang-send` and `hlang-recv`. There is no new header, option tag or method, and endpoints that don't understand the attribute ignore it.
- **Media:** a translating media engine (`translating-media-engine-design.md`). An ng shim sits in front of an unmodified rtpengine. It splits each translated call into two rtpengine sessions with a translator worker in the middle. rtpengine keeps ICE, SRTP, WebRTC, transcoding, DTMF and recording; the translator handles one codec of plain RTP.
- **AthenaSIP:** parses `hlang-*` and `translate`, adds a `translate` policy hook (Lua, or a `translation` behaviour setting), and passes one `translation` key in ng. The proxy still never mixes media.
- **Reach:** Kamailio and OpenSIPS get the same engine by passing that one key.

**Fallback:** for proxies with no rtpengine, a translator can sit behind a focus-style trunk (A → node → translator → node → B), following the conferencing pattern.

## Positioning

**Translation as a property of the media plane, asked for in SDP.**

Any proxy that drives rtpengine can offer translation by swapping in the shim and passing one ng key. Endpoints that support the draft attribute can request or refuse translation, and are told when it is active. AthenaSIP is the reference implementation of both halves.

The translator's per-leg PCM interface is the same thing AI voice agents and live transcription need. "Point your trunk at it and get clean PCM per leg" remains the developer pitch.

## Applications

These are ordered roughly by how clearly someone would pay for them.

### 1. Translation trunk for contact centres
- AthenaSIP sits between the carrier and the PBX or CCaaS platform. The carrier side is already proven: registration, challenges, TLS and RFC 4733.
- The caller's language is chosen per number (the `numbers` already routed in), by SIP header, or by automatic detection.
- The agent and the caller each hear the other party translated.
- Both the original and translated tracks are recorded, giving the QA and audit trail buyers ask for.
- This replaces over-the-phone interpreting, which already has a known budget line.

### 2. Engine-neutral routing and failover
- Each call goes to OpenAI, Gemini, Azure or a self-hosted model, based on language pair, cost or data residency. The choice lives in trunk attributes and Lua.
- Calls fail over to another engine on degradation. This mirrors the existing trunk failover and the rtpengine pool.
- Clustering lets signalling nodes and translators scale independently.

### 3. Human-interpreter fallback
- A human interpreter is brought in over SIP when the AI cannot cover the language, or when the stakes are high (medical, legal, emergency).
- With route A, this is a third leg on the translator, or a conference focus once conferencing lands.

### 4. Event dial-in
- The venue AV desk dials one number to feed floor audio into the system.
- Listeners join in one of two ways:
  - **Browser:** over WebRTC, which rtpengine and the TURN credential minting already support.
  - **Phone:** by dialling a per-language number.
- Phone dial-in serves attendees without smartphones and those with accessibility needs, which the event platforms do not cover.

### 5. Multilingual conference bridge
- Each participant leg carries its own language.
- Each listener gets their own translated mix.
- This builds on the RFC 4579 focus work in Milestone 7.

### 6. Public sector in Finland and the EU
- Target users include municipal services, healthcare triage and emergency lines.
- These need Finnish, Swedish and English, plus immigrant languages.
- They require self-hosting or EU data residency, and a consent announcement before translation starts. GPLv3 and self-hosting are an advantage here.
- **Check output-language coverage first.** OpenAI's translate model outputs only 13 languages, and Finnish may not be among them.

## Capabilities: have and need

| Capability | Status | Where it lands |
|---|---|---|
| Media anchoring, rtpengine over ng, engine pool | **Have** | AthenaSIP |
| RFC 4733 / INFO / in-band DTMF | **Have** | AthenaSIP and rtpengine; the translator forwards events |
| WebRTC ↔ RTP, SRTP, transcoding | **Have** — via rtpengine | rtpengine, unchanged |
| Recording | **Have** — via rtpengine | rtpengine; split sessions give original and translated tracks |
| Call visibility, records, hang-up API | **Have** | AthenaSIP; add languages and translation state |
| Parse `hlang-*` and `translate` from SDP | **Need** | AthenaSIP (`media::Flags`) |
| `translate` policy hook; `translation` behaviour setting | **Need** | AthenaSIP (Lua and `builtin://`) |
| `translation` key on ng `offer`/`answer` | **Need** | AthenaSIP rtpengine driver |
| ng shim: pass-through, split, fan-out | **Need** | Shim |
| Jitter buffer, decode/encode, pacing, RTCP | **Need** | Translator |
| Mute / duck / replace; echo gating | **Need** | Translator |
| Announcement to both parties | **Need** | Translator |
| Engine adapters, per-pair choice, failover | **Need** | Translator |

## Reuse beyond translation

The translator's per-leg PCM interface is the same component AI voice agents, live transcription and compliance analytics need. With a different worker behind the shim, the same split serves all of them, and AthenaSIP stays a clean proxy.

## Suggested next steps

The milestones are in `translating-media-engine-design.md`. In short:

1. Build the translator alone: plain RTP, one engine, EN↔FI, with latency measured.
2. Build the shim against `rtpengine-ng-client`.
3. Add AthenaSIP support and run an end-to-end call on `corvus-fi-1`.
4. Prove WebRTC through the split.
5. Submit the draft with an implementation report.

Alongside these, finish the security-review items and fuzzing before anything carries real customer calls. The contact-centre translation trunk is still the clearest buyer.
