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

#include "realm.h"
#include "sip_identity.h"

namespace athenasip::types {

// Somebody who may register. The password is never held, only HA1 (RFC 2617), once per algorithm, since one cannot
// be derived from the other. A subscriber provisioned with a password has both; one imported as a bare MD5 HA1 has
// only that, and is challenged for MD5 alone.
class Subscriber {
 public:
  uint64_t id;
  std::shared_ptr<SIPIdentity> identity;

  // MD5, which every SIP client speaks (RFC 3261 22.4).
  std::string ha1;

  // SHA-256 (RFC 8760), empty when this subscriber has no credential for it.
  std::string ha1_sha256;

  // What this subscriber's endpoint is, when the operator knows (Asterisk's webrtc=yes). It decides the first
  // description produced towards the leg, above the realm and the transport and below what the leg itself has said.
  // Empty takes the realm's behaviour.
  std::optional<MediaPolicy::Profiles> media_profile;
};
}  // namespace athenasip::types