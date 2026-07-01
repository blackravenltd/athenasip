//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "uint_header.h"

using namespace athenasip;

namespace athenasip::headers {

bool UIntHeader::parse(const std::string& val) {
  try {
    value = std::stoul(val);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

std::string UIntHeader::to_string() const { return std::to_string(value); }

}  // namespace athenasip::headers