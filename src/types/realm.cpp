//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "realm.h"

#include <regex>
#include <sstream>

#include "../util.h"

namespace athenasip::types {

std::optional<MediaPolicy::Profiles> MediaPolicy::parse_profiles(const std::string& value) {
  const auto name = Util::to_lower(Util::trim(value));

  if (name == "transport") return Profiles::FromTransport;
  if (name == "mirror") return Profiles::Mirror;
  if (name == "rtp") return Profiles::PlainRtp;
  if (name == "webrtc") return Profiles::WebRtc;
  if (name == "srtp") return Profiles::SrtpSdes;

  return std::nullopt;
}

MediaPolicy::Profiles MediaPolicy::profiles_from_string(const std::string& value, Profiles fallback) { return parse_profiles(value).value_or(fallback); }

std::string MediaPolicy::to_string(Profiles value) {
  switch (value) {
    case Profiles::FromTransport:
      return "transport";
    case Profiles::Mirror:
      return "mirror";
    case Profiles::PlainRtp:
      return "rtp";
    case Profiles::WebRtc:
      return "webrtc";
    case Profiles::SrtpSdes:
      return "srtp";
  }

  return "mirror";
}

Realm::Realm() {}

Realm::Realm(const std::string& _name) { name = _name; }

std::string Realm::to_string() const { return name; }

}  // namespace athenasip::types
