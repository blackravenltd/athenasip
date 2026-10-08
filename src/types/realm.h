//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/json.hpp>
#include <cstdint>
#include <optional>
#include <regex>
#include <sstream>
#include <string>

namespace athenasip::types {

// What a call asks of the media engine, after the realm's settings are laid over the server's default (see
// Behaviour): whether this node anchors media, and how a leg's profile is decided before the leg has described itself.
//
// The defaults follow the standards except for anchoring: RFC 3261 16.6 has a proxy forward a body untouched, and
// anchoring is a deliberate deviation. Within it, each leg's profile passes through as the leg wrote it.
struct MediaPolicy {
  // Whether the node anchors media when an engine is configured. Off, descriptions travel untouched and media goes
  // end to end, which fails for anything behind NAT.
  bool anchor = true;

  // How a leg's profile is decided. The engine cannot: at offer time the far leg has not described itself.
  enum class Profiles {
    // ws and wss are WebRTC, everything else plain RTP. Right where a WebSocket always means a browser.
    FromTransport,

    // The default: the engine keeps what it was handed, so a callee is offered what the caller offered.
    Mirror,

    // Every leg is plain RTP, for a realm whose WebSocket clients are SIP phones rather than browsers.
    PlainRtp,

    // Every leg is WebRTC.
    WebRtc,

    // Every leg is SRTP with the keys in the description (RFC 4568). No transport implies it, so it must be set.
    SrtpSdes,
  };

  Profiles profiles = Profiles::Mirror;

  // "transport", "mirror", "rtp", "webrtc" or "srtp". Nothing for anything else, which is an error to report.
  static std::optional<Profiles> parse_profiles(const std::string& value);

  // The same for a stored value, where anything unreadable is the fallback.
  static Profiles profiles_from_string(const std::string& value, Profiles fallback = Profiles::Mirror);
  static std::string to_string(Profiles value);
};

// How a realm differs from the server's default behaviour (Config::behaviour). A setting is unset unless the realm
// chose it, and an unset one is the server's, so changing the server default changes every realm that has not.
struct Behaviour {
  std::optional<bool> media_anchor;
  std::optional<MediaPolicy::Profiles> media_profile;

  // How often, in seconds, a client registered in this realm is sent OPTIONS (Asterisk's qualify, Kamailio's
  // nathelper ping). Zero, the default, is never. An SDP in the reply (RFC 3261 11.2) says what media the client takes.
  std::optional<std::uint32_t> qualify_interval;

  static constexpr std::uint32_t kQualifyMinimum = 5;
  static constexpr std::uint32_t kQualifyMaximum = 86400;

  // Zero, or within [kQualifyMinimum, kQualifyMaximum].
  static bool valid_qualify_interval(std::int64_t seconds) { return seconds == 0 || (seconds >= kQualifyMinimum && seconds <= kQualifyMaximum); }

  std::uint32_t qualify_over(std::uint32_t server) const { return qualify_interval.value_or(server); }

  // Whether a Contact this node forwards is rewritten to where the message came from (Asterisk's rewrite_contact,
  // Kamailio's fix_nated_contact). Default false, as RFC 3261 16.6 expects; for endpoints behind NAT talking to
  // something that ignores Record-Route.
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
  Realm();
  explicit Realm(const std::string& name);

  uint64_t id;
  std::string name;
  std::string nonce_secret;
  uint32_t nonce_expiry = 3600;

  // The longest registration this realm grants; a client asking for more is given this (RFC 3261 10.3 step 7).
  uint32_t registration_timeout = 5000;

  // The shortest registration this realm accepts; a client asking for less gets 423 with this in Min-Expires. Zero,
  // the default, accepts any. RFC 3261 10.3 says to reject only when refreshes would degrade the registrar.
  uint32_t registration_minimum = 0;

  // What this realm does differently from the server's default.
  Behaviour behaviour;

  // Anything a routing script wants to know about the realm. The node reads none of it.
  boost::json::object attributes;

  std::string to_string() const;
};

}  // namespace athenasip::types
