//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <optional>
#include <regex>
#include <sstream>
#include <string>

namespace athenasip::types {

// What a realm's calls ask of the media engine. These are the two questions an
// operator has that the signalling cannot answer for itself: whether this node should
// put itself in the media path at all, and how to decide what each leg of a call
// needs when the leg has not yet said.
struct MediaPolicy {
  // Whether the node anchors media when an engine is configured. Off means every
  // description travels untouched and the media goes end to end, which is right for
  // two endpoints that can reach each other and wrong for anything behind a NAT.
  bool anchor = true;

  // How a leg's profile is decided. The engine cannot decide it: at offer time the far
  // leg has not described itself, and the engine sees neither where a message is going
  // nor over what.
  enum class Profiles {
    // ws and wss are WebRTC and everything else is plain RTP. Right wherever a
    // WebSocket means a browser, which is almost everywhere.
    FromTransport,

    // Say nothing and let the engine keep what it was handed. For a realm whose calls
    // are always like to like.
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

  Profiles profiles = Profiles::FromTransport;

  // "transport", "mirror", "rtp", "webrtc" or "srtp". Anything else is the fallback,
  // because a
  // realm with a typo in it should keep working the way it did rather than change
  // what it does to media.
  static Profiles profiles_from_string(const std::string& value, Profiles fallback = Profiles::FromTransport);
  static std::string to_string(Profiles value);
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

  // What this realm's calls ask of the media engine. Defaults to anchoring and to
  // reading each leg from its transport, which is what a node did before a realm could
  // say otherwise.
  MediaPolicy media;

  std::string to_string() const;
};

}  // namespace athenasip::types
