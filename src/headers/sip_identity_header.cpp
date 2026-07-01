//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "sip_identity_header.h"

using namespace athenasip;

namespace athenasip::headers {

bool SIPIdentityHeader::parse(const std::string& val) {
  try {
    value = std::make_shared<SIPIdentity>(val);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

std::string SIPIdentityHeader::to_string() const { return value->to_string(); }

}  // namespace athenasip::headers