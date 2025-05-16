//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <optional>
#include <regex>
#include <sstream>
#include <string>

#include "sip_identity.h"

namespace athenasip::types {
class Subscriber {
 public:
  uint64_t id;
  std::shared_ptr<SIPIdentity> identity;
  std::string ha1;
};
}  // namespace athenasip::types