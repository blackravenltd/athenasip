```
Network Working Group                                           T. Cully
Internet-Draft                                         BlackRaven Limited
Intended status: Standards Track                         26 October 2026
Expires: 29 April 2027


       A Session Description Protocol (SDP) Attribute for Requesting
                   Real-Time Translation of Speech
                   draft-cully-mmusic-sdp-translate-00
```

## Abstract

This document defines a media-level SDP attribute, `translate`. With it, an endpoint can request or refuse real-time translation of the speech in a media stream by an intermediary on the media path. The intermediary uses the same attribute to tell the endpoint that translation is active. The languages involved are those already negotiated with the `hlang-send` and `hlang-recv` attributes of RFC 8373.

## Status of This Memo

This Internet-Draft is submitted in full conformance with the provisions of BCP 78 and BCP 79.

## 1. Introduction

RFC 8373 lets the parties to a session declare the languages they speak and understand in each media stream. When the two parties share no language, RFC 8373 lets the call proceed and notes that it may be routed to an interpreter or relay service. It does not give an endpoint a way to ask for interpretation, or to learn that interpretation is taking place.

Intermediaries that anchor media, such as Session Border Controllers or media relays controlled by a SIP proxy, can now translate speech in real time with machine translation. This document defines the minimum signalling needed for that:

- an endpoint asks for translation of a stream, or forbids it;
- the intermediary tells each endpoint that translation is active.

This document defines no new language negotiation; the languages are those of RFC 8373. It defines no SIP header field, option tag or method, and does not specify how translation is performed.

## 2. Conventions and Terminology

The key words "MUST", "MUST NOT", "REQUIRED", "SHALL", "SHALL NOT", "SHOULD", "SHOULD NOT", "RECOMMENDED", "NOT RECOMMENDED", "MAY", and "OPTIONAL" in this document are to be interpreted as described in BCP 14 [RFC2119] [RFC8174] when, and only when, they appear in all capitals, as shown here.

**Translator:** an intermediary on the media path of a stream that can deliver one party's speech to the other party in a different language, and that inspects and modifies the SDP exchanged for the stream. Translation itself happens on the media path; a signalling element such as a SIP proxy may decide on the translator's behalf, under local policy.

**Endpoint:** a party to the session other than a translator.

## 3. The "translate" Attribute

```
attribute       =/ translate-attr
translate-attr  =  "translate:" translate-value
translate-value =  "request" / "refuse" / "active" / token
```

`attribute` is defined in [RFC8866] and `token` in [RFC8866]. The ABNF follows [RFC5234].

The attribute is a media-level attribute. It applies to the media description in which it appears. At most one `translate` attribute appears per media description.

The values have the following meanings:

**request**
: Sent by an endpoint. If the two parties share no language for this stream, the endpoint asks that the stream be translated.

**refuse**
: Sent by an endpoint. The endpoint forbids translation of this stream.

**active**
: Sent only by a translator. The stream is being translated.

A receiver MUST treat a value it does not understand as if the attribute were absent.

When the media description also carries `hlang-send` and `hlang-recv`, those values keep their meaning from [RFC8373]: they are the languages of the party that wrote them. A translator MUST NOT change them. When `translate:active` is present, the receiver infers that the peer's speech is delivered to it in a language from its own `hlang-recv`. It also infers that its own speech is delivered to the peer in a language from the peer's `hlang-recv`.

## 4. Offer/Answer Procedures

The procedures in this section apply in addition to those of [RFC3264] and [RFC8373].

### 4.1. Endpoint Procedures

An endpoint that wants translation of a stream includes `a=translate:request` in the media description, in an offer or an answer. It SHOULD also include `hlang-send` and `hlang-recv`.

An endpoint that forbids translation of a stream includes `a=translate:refuse`. An endpoint that receives an offer containing `a=translate:request` and objects to translation MUST include `a=translate:refuse` in its answer.

An endpoint MUST NOT include `a=translate:active`.

An endpoint that receives `a=translate:active` SHOULD indicate to its user that the stream is translated.

A subsequent offer without `a=translate:request` withdraws an earlier request. A subsequent offer with `a=translate:request` makes a new one, for example after the user has changed `hlang-*`.

### 4.2. Translator Procedures

A translator that receives an offer containing `a=translate:request` decides, by local policy, whether it would translate the stream. It then does one of the following:

- If it would, it forwards the offer with the attribute unchanged, so the answerer can refuse.
- If it would not, it removes the attribute.

A translator that translates by local policy without a request from the offerer MUST add `a=translate:request` to the offer it forwards.

When the translator receives the answer, it translates the stream only if all of the following hold:

1. The offer it forwarded contained `a=translate:request`.
2. Neither the offer nor the answer contained `a=translate:refuse`.
3. The parties share no language for the stream. This is determined as in [RFC8373] where `hlang-*` are present, and otherwise by local policy.

If it translates, it replaces any `translate` attribute in the answer with `a=translate:active` before forwarding the answer. If it does not translate, it forwards any `a=translate:refuse` in the answer unchanged and removes any other `translate` attribute.

A translator that starts or stops translating a stream later in the session MUST send a new offer to each endpoint, reflecting the change.

The answerer does not receive `a=translate:active` in the first offer/answer exchange, because the decision follows its answer. Its consent is that it did not refuse. Section 6 requires that it is also told within the media.

### 4.3. Without a Translator

An endpoint ignores an unknown attribute ([RFC8866]). An offer containing `a=translate:request` that reaches an answerer without passing a translator therefore results in an answer with no `translate` attribute, or with `refuse`. The offerer learns that no translation takes place. No other behaviour changes.

## 5. Multiplexing Considerations

The mux category of the `translate` attribute is NORMAL [RFC8859]. Each media description sharing a BUNDLE transport keeps its own value.

## 6. Security Considerations

**Integrity.** A translator modifies SDP, which defeats end-to-end integrity protection of the SDP. It also alters media content, which defeats end-to-end media security. An endpoint that requires either SHOULD send `a=translate:refuse`.

**Disclosure.** Machine translation produces speech the speaker did not utter, and may err. A translator MUST announce within the media, to each party, that translation is in progress before delivering translated speech. This applies in particular to a party that did not request translation. A translator MUST honour `refuse`.

**Voice cloning.** A translator MUST NOT synthesise speech in the voice of a party without that party's explicit consent. That consent is outside the scope of this document.

**Third parties.** Translation may send speech to a third party, such as a translation service. Translators SHOULD make this, and any retention, known to the parties under applicable law.

**Abuse.** Translation consumes resources and may incur cost. A translator SHOULD translate only for parties its policy authorises; `request` is a request, not an entitlement.

## 7. IANA Considerations

IANA is requested to register the following attribute in the "att-field (media level only)" subregistry of the "Session Description Protocol (SDP) Parameters" registry, as specified in [RFC8866]:

| Field | Value |
|---|---|
| Contact name | Tom Cully, mail@tomcully.com |
| Attribute name | translate |
| Attribute syntax | See Section 3 of this document |
| Attribute semantics | See Section 3 of this document |
| Usage level | media |
| Charset dependent | No |
| Purpose | Request, refuse, or indicate real-time translation of the speech in a media stream |
| Offer/answer procedures | See Section 4 of this document |
| Mux category | NORMAL |
| Reference | This document |

## 8. References

### 8.1. Normative References

- **[RFC2119]** Bradner, S., "Key words for use in RFCs to Indicate Requirement Levels", BCP 14, RFC 2119, March 1997.
- **[RFC3264]** Rosenberg, J. and H. Schulzrinne, "An Offer/Answer Model with the Session Description Protocol (SDP)", RFC 3264, June 2002.
- **[RFC5234]** Crocker, D. and P. Overell, "Augmented BNF for Syntax Specifications: ABNF", STD 68, RFC 5234, January 2008.
- **[RFC8174]** Leiba, B., "Ambiguity of Uppercase vs Lowercase in RFC 2119 Key Words", BCP 14, RFC 8174, May 2017.
- **[RFC8373]** Gellens, R., "Negotiating Human Language in Real-Time Communications", RFC 8373, May 2018.
- **[RFC8859]** Nandakumar, S., "A Framework for Session Description Protocol (SDP) Attributes When Multiplexing", RFC 8859, January 2021.
- **[RFC8866]** Begen, A., Kyzivat, P., Perkins, C., and M. Handley, "SDP: Session Description Protocol", RFC 8866, January 2021.

### 8.2. Informative References

- **[RFC3261]** Rosenberg, J., et al., "SIP: Session Initiation Protocol", RFC 3261, June 2002.

## Appendix A. Example

Alice speaks Finnish and Bob speaks English. Only the media descriptions are shown.

Alice's offer, as sent:

```
m=audio 49170 RTP/AVP 0 101
a=hlang-send:fi
a=hlang-recv:fi
a=translate:request
```

The translator's policy allows it, so the offer reaches Bob with the attribute unchanged. The connection data is rewritten as for any media relay.

Bob's answer, as sent:

```
m=audio 51372 RTP/AVP 0 101
a=hlang-send:en
a=hlang-recv:en
```

The parties share no language and neither refused, so the translator translates. It forwards to Alice:

```
m=audio 30000 RTP/AVP 0 101
a=hlang-send:en
a=hlang-recv:en
a=translate:active
```

Alice hears Bob in Finnish, and Bob hears Alice in English. Each first hears a short announcement that the call is being translated.

## Author's Address

Tom Cully
BlackRaven Limited
Email: mail@tomcully.com
