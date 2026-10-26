# A Translating Media Engine for rtpengine — Design Note

*26 October 2026 · companion to `draft-cully-mmusic-sdp-translate-00`*

## What it is

A media engine that any SIP proxy can drive over rtpengine's ng protocol. It behaves exactly like rtpengine until a call needs translation. For those calls, it delivers each party's speech to the other in that party's language.

The engine has two parts:

- **The shim.** It speaks ng to the proxy and ng to an unmodified rtpengine. It decides which calls are translated and rearranges their media through the translator.
- **The translator.** A worker that terminates plain RTP in a single codec. For each direction it holds a streaming session with a translation engine (OpenAI, Gemini, Azure and others), and it sends the result back as RTP.

The signalling is `draft-cully-mmusic-sdp-translate-00`: one SDP attribute, `translate`, with the languages taken from RFC 8373's `hlang-send` and `hlang-recv`. AthenaSIP is the reference proxy. Kamailio and OpenSIPS can use it by passing one ng key.

## Why a wrapper, not a fork

rtpengine, as far as I know, has no plugin interface for processing audio; its decoding lives inside its own transcoding path, so adding translation there means forking it. A fork would inherit Sipwise's release cadence indefinitely. Both projects are GPLv3, so licensing doesn't prevent a fork; ongoing maintenance does.

The wrapper leaves rtpengine untouched and uses what it already does well:
- ICE, DTLS-SRTP and WebRTC
- transcoding
- DTMF
- recording

The cost is one localhost UDP hop, which is microseconds next to 400–900 ms of translation.

Revisit a fork only if measurements show the hop matters. It won't.

## Components

```
            ng                    ng
  proxy ────────►  shim  ─────────────────►  rtpengine
 (AthenaSIP,        │                          ▲     ▲
  Kamailio, …)      │ control                  │RTP  │RTP
                    ▼                          │     │
               translator ◄────── RTP ─────────┘     │
                    │  ◄──────────────── RTP ────────┘
                    │ WebSocket (PCM16)
                    ▼
        translation engines (OpenAI, Gemini, Azure, …)

  Alice ◄──RTP/SRTP/WebRTC──► rtpengine ◄──RTP/SRTP/WebRTC──► Bob
```

For a translated call, rtpengine holds two sessions in place of one, and the translator is the far end of both.

## The split

A proxy's call `X`, with tags `A` (caller) and `B` (callee), becomes two rtpengine calls:

| rtpengine call | Outside end | Inside end | Inside codec |
|---|---|---|---|
| `X~a` | Alice, with her SDP, ICE and crypto | translator leg *a* | one fixed codec |
| `X~b` | Bob, with his SDP, ICE and crypto | translator leg *b* | the same codec |

rtpengine transcodes each outside leg to the inside codec. The inside codec should be L16 at 16 kHz if the deployed rtpengine supports it; otherwise Opus at 16 kHz. Check this against the codec list of the rtpengine version in use. The translator therefore handles one codec, plain RTP, and no ICE or crypto.

**Calls without translation are never split.** The shim forwards their commands to rtpengine byte for byte.

### Offer

The proxy sends `offer X` with Alice's SDP. If the call is a candidate for translation (see *When a call is translated*):

1. The shim sends `offer X~a` to rtpengine with Alice's SDP and the proxy's flags that concern Alice's side, adding `codec: {mask: all, transcode: [inside codec]}`. rtpengine returns an SDP for its inside port on `X~a`.
2. The translator opens leg *a* against that SDP and returns its own SDP.
3. The shim sends `answer X~a` with the translator's SDP. rtpengine returns the SDP for Alice. The shim holds it until the answer.
4. The translator opens leg *b* and returns an SDP for its side of `X~b`.
5. The shim sends `offer X~b` with the translator's SDP and the proxy's flags that concern Bob's side (for example `transport-protocol` and ICE). rtpengine returns the SDP for Bob.
6. The shim copies Alice's media-level `hlang-*` and `translate` lines into that SDP and returns it to the proxy as the result of `offer X`.

The media path is committed at the offer, before anyone knows whether translation is needed. A candidate call that turns out not to need translation runs through the translator in passthrough mode.

### Answer

The proxy sends `answer X` with Bob's SDP.

7. The shim sends `answer X~b` with Bob's SDP and passes rtpengine's result to translator leg *b*.
8. The shim decides, following the draft's section 4.2, from Alice's offer, Bob's answer and the proxy's `translation` key. The outcome is one of:
   - translate Alice→Bob from *fi* to *en* and Bob→Alice from *en* to *fi*
   - passthrough
   - refused
9. The shim takes the SDP for Alice held from step 3 and adjusts it:
   - copies in Bob's `hlang-*`;
   - sets `translate:active`, or removes the attribute, or passes on `refuse`.

   It returns the result to the proxy.

### Everything else

| ng command | For a split call |
|---|---|
| `delete` | Both sessions, then the translator's legs |
| `query` | Both sessions, merged into one answer in the shape the proxy expects: Alice's and Bob's outside legs |
| `play DTMF` | To the session on the named participant's side |
| `start recording` / `stop recording` | Both sessions. Each records one party's original audio and the translated audio that party hears, which together are the audit trail. |
| `play media` | Used by the shim itself for announcements; passed through for the proxy |
| re-offer / re-answer (hold, a language change, withdrawal) | Steps 1–9 against the existing sessions, using `replace` flags as rtpengine expects |
| `ping`, `list`, `statistics`, anything unknown | Passed through |

**State.** The shim maps `X` to `X~a`, `X~b` and the translator's legs. That state lives with the shim, on the rtpengine host. A call's media cannot move between hosts anyway, and a proxy with an rtpengine pool already sends every command for a call to the same engine.

## When a call is translated

The proxy decides by policy and tells the shim with one ng key. rtpengine never sees this key, because the shim removes it.

```
"translation": {
  "allow": "yes",                 // policy allows; absent or "no": no translation, and the shim removes a=translate:request
  "mode": "duck",                 // duck (default) | mute | replace
  "languages-a": ["fi"],          // used only when Alice's SDP has no hlang
  "languages-b": ["en"],          // used only when Bob's SDP has no hlang
  "engine": "openai"              // optional preference; the shim's configuration otherwise
}
```

The shim treats a call as a candidate when `allow` is `yes` and one of the following holds:
- Alice's offer carries `a=translate:request`;
- the proxy marked the call as translated by policy, in which case the shim adds the `request` attribute itself, as the draft requires.

A shim configured with `trust_sdp: true` acts on `a=translate:request` without the key. This is for proxies that cannot be changed. It moves the decision about who may consume translation onto the network's own perimeter, so it is off by default.

**Language precedence:**
1. `hlang-*` in the SDP
2. `languages-a` and `languages-b` from the proxy
3. the engine's automatic language detection, where it has one
4. passthrough

## The translator

Each call has two legs and, while it is translating, two engine sessions.

```
leg a RTP ─► jitter buffer ─► decode ─► PCM 16 kHz ─► engine (fi→en) ─► PCM ─► resample/encode ─┐
                                   └──────────── original, ducked ───────────────────────────────┤
                                                                                       mix, pace ─► leg b RTP
(and the same from leg b to leg a)
```

- **It is a source, not a relay.** Translated speech arrives late and with a different length from the original. The translator writes its own sequence numbers and timestamps, sets the marker bit at each talkspurt, fills silence (or comfort noise, if negotiated) and sends its own RTCP.
- **What the listener hears.** `duck` mixes the original quietly under the translation, which keeps the speaker's prosody and the listener's trust. `mute` sends the translation alone. `replace` is the same as mute but sends the original before translation starts.
- **Echo.** A speakerphone hears the translation it is played and sends it back, so it would be translated again. Mitigation is half-duplex gating: while translated speech plays into leg *b*, the translator gates leg *b*'s input to its engine, but still mixes it as original. The gating is on by default and can be turned off per call when both ends cancel their own echo.
- **DTMF.** The inside legs negotiate `telephone-event`, and the translator forwards events from one leg to the other untranslated. It suppresses translation input while events or tones are present.
- **Announcement.** When translation starts, the translator plays into each leg a short pre-rendered prompt in that party's language saying the call is being translated. It plays this before any translated speech. This is how the callee, who never sees `translate:active` in the first exchange, is told.
- **Engines.** Each engine sits behind one interface:

  ```
  open(source_language, target_language) → session
  session.push(pcm)
  session.on_audio(pcm)
  session.on_text(text)    // captions, and the record
  session.close()
  ```

  Adapters for OpenAI `gpt-realtime-translate`, Gemini 3.5 Live Translate and Azure Live Interpreter. If an engine fails at open time, the call goes to the next engine configured for that language pair. If it fails mid-call, the translator plays a short notice and falls back to passthrough. An engine that does not cover a pair is never offered that pair; OpenAI's 13 output languages, for example, may not include Finnish.
- **Cost.** A translated two-way call is two engine sessions, so at OpenAI's published $0.034 per minute, about $0.068 per call minute before infrastructure.

## AthenaSIP's part

1. **Parse.** `media::Flags::from_sdp` gains `hlang_send`, `hlang_recv` and `translate` for each media description.
2. **Policy.** Add a `translate(call, offer)` hook to the routing policy. Under `lua://` it returns `athenasip.translate.allow{mode = …, languages = …}` or `.deny()`. `builtin://` reads a behaviour setting, `translation: off | on_request | always`, which can be set per realm, subscriber or trunk and is `off` by default. Subscriber and number attributes supply `languages-*` for endpoints that never send `hlang`.
3. **Driver.** `RtpengineMediaEngine` adds the `translation` key to `offer` and `answer` when the policy allows. Nothing else in the driver changes, because the shim speaks ng.
4. **Visibility.** `GET /api/v1/calls/{call}` shows each participant's languages and whether the call is translated. Call records keep it. An event is published when translation starts or stops.
5. **Tests.** A sipp scenario with `hlang` and `translate`, run against a shim whose engine is a fake that echoes with a fixed delay.

**For Kamailio and OpenSIPS:** pass the `translation` dictionary in `rtpengine_offer()` and `rtpengine_answer()`. An example route belongs in the shim's documentation.

## Milestones

1. **Translator alone.** Plain RTP in a fixed codec, one engine adapter, passthrough and translate, announcements, echo gating. Tested EN↔FI on 8 kHz and wideband recordings, measuring first-audio latency and lag.
2. **Shim.** ng pass-through, the split for candidate calls, and fan-out of `delete`, `query` and recording. Tested against `rtpengine-ng-client`.
3. **AthenaSIP.** Parsing, the policy hook, the driver key and the API fields. An end-to-end call on `corvus-fi-1`.
4. **WebRTC.** A browser on one side through rtpengine's DTLS-SRTP, which proves the split holds for every media profile.
5. **Draft.** Submit `draft-cully-mmusic-sdp-translate-00` with an implementation report from milestones 1–4.

## Open questions

- **Inside codec.** L16 at 16 kHz if rtpengine supports it, which avoids a lossy step; otherwise Opus. Measure.
- **One-way translation.** For example a listener-only leg at an event. The draft can express it, since `request` is per party, but the shim needs a mode that translates one direction and passes the other through.
- **Captions.** Engines return text alongside audio. The translator could publish it as an event per call, for an admin console or a WebRTC data channel. Doing so doesn't touch the draft.
