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

#include "sip_identity.h"

namespace athenasip::types {

// What this server knows about somebody who may register.
//
// The password is never held. What is held is HA1 - the hash of user, realm and
// password (RFC 2617) - once per algorithm this account can authenticate with, because
// the two hashes are computed from the password and cannot be derived from each other.
// An account provisioned with a password has both; one imported as a bare MD5 HA1 has
// only that, and is challenged for MD5 alone.
class Account {
 public:
  uint64_t id;
  std::shared_ptr<SIPIdentity> identity;

  // MD5, which every SIP client speaks (RFC 3261 22.4).
  std::string ha1;

  // SHA-256 (RFC 8760), empty when this account has no credential for it.
  std::string ha1_sha256;
};
}  // namespace athenasip::types