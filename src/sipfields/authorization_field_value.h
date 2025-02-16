//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>
#include <string>

#include "../siptypes/authorization.h"
#include "field_value.h"

namespace athenasip::sipfields {

class AuthorizationFieldValue : public FieldValue {
 public:
  AuthorizationFieldValue() = default;
  explicit AuthorizationFieldValue(const std::string& value) : value(std::make_shared<athenasip::siptypes::Authorization>()) { parse(value); }
  explicit AuthorizationFieldValue(std::shared_ptr<athenasip::siptypes::Authorization> val) : value(val) {}

  bool parse(const std::string& val) override;
  std::string to_string() const override;

  // Public member storing the parsed WWW-Authorization value.
  std::shared_ptr<athenasip::siptypes::Authorization> value;
};

// Register this field type under the "WWW-Authorization" header name.
struct AuthorizationFieldValueRegister {
  AuthorizationFieldValueRegister() {
    auto reg = []() -> std::shared_ptr<FieldValue> { return std::make_shared<AuthorizationFieldValue>(); };
    FieldValue::register_factory("WWW-Authorization", reg);
    FieldValue::register_factory("Authorization", reg);
  }
};
static AuthorizationFieldValueRegister s_AuthorizationFieldValueRegister;

}  // namespace athenasip::sipfields
