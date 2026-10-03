//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

namespace athenasip::media {

// What a session description has to be, as opposed to what is in it. It lives here
// rather than inside Flags because a Call remembers one per leg, and call.h cannot see
// the media contract - the contract is what sees the Call.
//
// Mirror leaves it to the engine, which is right for a call whose ends match and wrong
// for one that does not. The others say plainly which.
//
// SrtpSdes is the desk phone that wants its media encrypted and has never heard of
// DTLS: RTP/SAVP with the keys in the description (RFC 4568), rather than a browser's
// UDP/TLS/RTP/SAVPF.
enum class Profile { Mirror, PlainRtp, WebRtc, SrtpSdes };

// A profile in the words the behaviour settings use, so what is logged or reported can be
// set as it is read.
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
