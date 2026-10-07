//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "media_engine.h"

#include <optional>
#include <string>
#include <vector>

#include "../sdp.h"
#include "../util.h"

namespace athenasip::media {

namespace {

bool starts_with(const std::string& value, const char* prefix) { return value.rfind(prefix, 0) == 0; }

}  // namespace

Flags Flags::from_sdp(const std::string& sdp_text) {
  Flags flags;

  SDP sdp;

  // An unreadable description sets nothing; the engine handed it is what refuses it.
  if (!sdp.parse(sdp_text)) return flags;

  flags.readable = true;

  auto read = [&flags](const std::vector<std::string>& attributes) {
    for (const auto& attribute : attributes) {
      // RFC 8839: any ICE attribute means the far end expects connectivity checks.
      if (starts_with(attribute, "ice-ufrag:") || starts_with(attribute, "ice-pwd:") || starts_with(attribute, "candidate:")) flags.ice = true;

      // RFC 8122: a fingerprint announces DTLS.
      if (starts_with(attribute, "fingerprint:")) flags.dtls = true;

      // RFC 4568: SDES keys.
      if (starts_with(attribute, "crypto:")) flags.srtp = true;

      // RFC 5761.
      if (attribute == "rtcp-mux") flags.rtcp_mux = true;
    }
  };

  read(sdp.session_attributes());

  for (const auto& media : sdp.media()) {
    read(media.attributes());

    // RFC 3711, RFC 5764: an SAVP or SAVPF profile is SRTP however the keys are agreed.
    if (media.description.proto.find("SAVP") != std::string::npos) flags.srtp = true;

    // RFC 5764 section 8, RFC 7850: UDP/TLS/RTP/SAVP(F) and TCP/DTLS/RTP/SAVPF are DTLS-SRTP by name, fingerprint or not
    // (a capability description in an OPTIONS 200 has none).
    if (media.description.proto.find("TLS/RTP/SAVP") != std::string::npos) flags.dtls = true;
  }

  return flags;
}

// DTLS means WebRTC (RFC 8122, RFC 5764), SDES keys mean SRTP (RFC 4568), anything else is plain RTP. ICE alone
// (RFC 8839) is not WebRTC.
std::optional<Flags::Profile> Flags::stated() const {
  if (!readable) return std::nullopt;
  if (dtls) return Profile::WebRtc;
  if (srtp) return Profile::SrtpSdes;

  return Profile::PlainRtp;
}

Flags::Profile Flags::profile_for_transport(const std::string& transport) {
  const auto name = Util::to_lower(transport);

  if (name.empty()) return Flags::Profile::Mirror;
  if (name == "ws" || name == "wss") return Flags::Profile::WebRtc;

  return Flags::Profile::PlainRtp;
}

}  // namespace athenasip::media
