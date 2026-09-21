//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "media_engine.h"

#include <string>
#include <vector>

#include "../sdp.h"

namespace athenasip::media {

namespace {

bool starts_with(const std::string& value, const char* prefix) { return value.rfind(prefix, 0) == 0; }

}  // namespace

Flags Flags::from_sdp(const std::string& sdp_text) {
  Flags flags;

  SDP sdp;

  // Nothing readable says nothing, and the engine that is handed the description is
  // what refuses it. Guessing here would be worse than saying so.
  if (!sdp.parse(sdp_text)) return flags;

  auto read = [&flags](const std::vector<std::string>& attributes) {
    for (const auto& attribute : attributes) {
      // RFC 8839. Either half of the ICE credentials is enough to say the far end
      // expects connectivity checks rather than the address in the c= line.
      if (starts_with(attribute, "ice-ufrag:") || starts_with(attribute, "ice-pwd:") || starts_with(attribute, "candidate:")) flags.ice = true;

      // RFC 8122: a fingerprint is the DTLS handshake being announced.
      if (starts_with(attribute, "fingerprint:")) flags.dtls = true;

      // RFC 4568: SDES keys in the description itself.
      if (starts_with(attribute, "crypto:")) flags.srtp = true;

      // RFC 5761.
      if (attribute == "rtcp-mux") flags.rtcp_mux = true;
    }
  };

  read(sdp.session_attributes());

  for (const auto& media : sdp.media()) {
    read(media.attributes());

    // RFC 3711 and RFC 5764: SAVP is SRTP and SAVPF is SRTP with feedback, whichever way
    // the keys were agreed. The profile is the statement, not the attributes under it.
    if (media.description.proto.find("SAVP") != std::string::npos) flags.srtp = true;
  }

  return flags;
}

}  // namespace athenasip::media
