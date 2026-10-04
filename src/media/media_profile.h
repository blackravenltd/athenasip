//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

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

}  // namespace athenasip::media
