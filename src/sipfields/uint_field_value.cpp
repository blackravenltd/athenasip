//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "uint_field_value.h"

using namespace athenasip;

namespace athenasip::sipfields {

bool UIntFieldValue::parse(const std::string& val) {
  try {
    value = std::stoul(val);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

std::string UIntFieldValue::to_string() const { return std::to_string(value); }

}  // namespace athenasip::sipfields