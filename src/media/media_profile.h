//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <optional>
#include <string>

namespace athenasip::media {

// The profile a session description must have. Defined outside Flags because a Call keeps one per leg and call.h
// cannot include the media contract.
//
// Mirror leaves the choice to the engine. SrtpSdes is RTP/SAVP with the keys in the description (RFC 4568), as
// opposed to WebRTC's UDP/TLS/RTP/SAVPF.
enum class Profile { Mirror, PlainRtp, WebRtc, SrtpSdes };


// The profile as the behaviour settings spell it.
inline const char* setting_name(Profile profile) {
  switch (profile) {
    case Profile::WebRtc:
      return "webrtc";
    case Profile::PlainRtp:
      return "rtp";
    case Profile::SrtpSdes:
      return "srtp";
    case Profile::Mirror:
      return "mirror";
  }
  return "";
}

// The reverse, for a stored name. Nothing for anything else.
inline std::optional<Profile> parse_setting(const std::string& name) {
  for (const auto profile : {Profile::WebRtc, Profile::PlainRtp, Profile::SrtpSdes, Profile::Mirror}) {
    if (name == setting_name(profile)) return profile;
  }
  return std::nullopt;
}

}  // namespace athenasip::media
