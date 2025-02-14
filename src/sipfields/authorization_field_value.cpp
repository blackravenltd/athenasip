//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "authorization_field_value.h"

using namespace athenasip;
using namespace athenasip::siptypes;

namespace athenasip::sipfields {

bool AuthorizationFieldValue::parse(const std::string& val) {
  try {
    value = std::make_shared<Authorization>(val);
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

std::string AuthorizationFieldValue::to_string() const { return value->to_string(); }

}  // namespace athenasip::sipfields