//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <optional>
#include <regex>
#include <sstream>
#include <string>

namespace athenasip::types {

// What a call asks of the media engine, once the realm's settings have been laid over the
// server's default (see Behaviour). These are the two questions an operator has that the
// signalling cannot answer for itself: whether this node should put itself in the media
// path at all, and how to decide what each leg of a call needs when the leg has not yet
// said.
//
// The values here are the shipped default, decided on 2026-10-01: exactly what the
// standards say, apart from anchoring. RFC 3261 16.6 has a proxy forward a body untouched,
// and anchoring is a recorded, deliberate deviation from that - the one most servers in
// front of rtpengine make. Within it, each leg's profile passes through as the leg wrote it.
struct MediaPolicy {
  // Whether the node anchors media when an engine is configured. Off means every
  // description travels untouched and the media goes end to end, which is right for
  // two endpoints that can reach each other and wrong for anything behind a NAT.
  bool anchor = true;

  // How a leg's profile is decided. The engine cannot decide it: at offer time the far
  // leg has not described itself, and the engine sees neither where a message is going
  // nor over what.
  enum class Profiles {
    // ws and wss are WebRTC and everything else is plain RTP: Kamailio's usual routing.
    // Right wherever a WebSocket means a browser and nothing else means one.
    FromTransport,

    // Say nothing and let the engine keep what it was handed, so a callee is offered what
    // the caller offered. The default: it imposes nothing, and like-to-like calls work
    // whatever the transports.
    Mirror,

    // Every leg is plain RTP, for a realm whose WebSocket clients are SIP phones
    // rather than browsers: RFC 7118 is SIP over WebSocket and says nothing about
    // WebRTC.
    PlainRtp,

    // Every leg is WebRTC.
    WebRtc,

    // Every leg is SRTP with the keys in the description (RFC 4568), which is a desk
    // phone that wants encryption and has never heard of DTLS. No transport tells
    // these apart from plain RTP, so this one can only be asked for.
    SrtpSdes,
  };

  Profiles profiles = Profiles::Mirror;

  // "transport", "mirror", "rtp", "webrtc" or "srtp", and nothing else: what a config file
  // or an API body names is either one of these or a mistake to report.
  static std::optional<Profiles> parse_profiles(const std::string& value);

  // The same, for a stored value, where anything unreadable is the fallback: a realm with
  // a typo in it keeps working the way it did rather than changing what it does to media.
  static Profiles profiles_from_string(const std::string& value, Profiles fallback = Profiles::Mirror);
  static std::string to_string(Profiles value);
};

// How a realm differs from the server's default behaviour (Config::behaviour). Each setting
// is unset unless the realm chose it, and an unset one is the server's - so changing the
// server's default changes every realm that has not chosen otherwise, and a realm says
// only what is different about it. One place for every behaviour that differs between SIP
// servers, so that a realm can be set to behave like any of them (the 2026-10-01 decision
// in TODO/ACTIVE.md).
struct Behaviour {
  std::optional<bool> media_anchor;
  std::optional<MediaPolicy::Profiles> media_profile;

  // How often, in seconds, a client registered in this realm is sent OPTIONS (Asterisk's
  // qualify, Kamailio's nathelper ping), and zero for never. RFC 3261 does not ask for it,
  // so the shipped default is zero. The reply says the client is there, and an SDP in it
  // (11.2) says what media the client takes.
  std::optional<std::uint32_t> qualify_interval;

  static constexpr std::uint32_t kQualifyMinimum = 5;
  static constexpr std::uint32_t kQualifyMaximum = 86400;

  // Zero, or often enough to matter and not so often that thousands of registrations
  // become a flood.
  static bool valid_qualify_interval(std::int64_t seconds) { return seconds == 0 || (seconds >= kQualifyMinimum && seconds <= kQualifyMaximum); }

  std::uint32_t qualify_over(std::uint32_t server) const { return qualify_interval.value_or(server); }

  // Whether a Contact in a request or response this node forwards is rewritten to where the
  // message came from: Asterisk's rewrite_contact, Kamailio's fix_nated_contact. A proxy
  // forwards what an endpoint said about itself (RFC 3261 16.6), so the shipped default is
  // false; it is for endpoints behind a NAT talking to something that ignores Record-Route.
  std::optional<bool> rewrite_contact;

  // The effective policy: this realm's settings over the server's default.
  MediaPolicy over(const MediaPolicy& server) const {
    auto effective = server;
    if (media_anchor) effective.anchor = *media_anchor;
    if (media_profile) effective.profiles = *media_profile;
    return effective;
  }
};

class Realm {
 public:
  // Constructors
  Realm();
  explicit Realm(const std::string& name);

  uint64_t id;
  std::string name;
  std::string nonce_secret;
  uint32_t nonce_expiry = 3600;

  // The longest registration this realm grants. A client asking for more is given this
  // instead (RFC 3261 10.3 step 7, "the registrar MAY choose an expiration less than
  // the requested expiration interval").
  uint32_t registration_timeout = 5000;

  // The shortest registration this realm accepts, and zero - the default - accepts any.
  // A client asking for less is refused with 423 Interval Too Brief and told this value
  // in Min-Expires. The RFC is explicit that this should stay off unless the refreshes
  // are actually costing something: "registrars should accept brief registrations; a
  // request should only be rejected if the interval is so short that the refreshes
  // would degrade registrar performance".
  uint32_t registration_minimum = 0;

  // What this realm does differently from the server's default; nothing, unless it says.
  Behaviour behaviour;

  std::string to_string() const;
};

}  // namespace athenasip::types
