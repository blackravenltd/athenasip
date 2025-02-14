//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include "../siptypes/sip_identity.h"
#include "field_value.h"

using namespace athenasip;
using namespace athenasip::siptypes;

namespace athenasip::sipfields {

class SIPIdentityFieldValue : public FieldValue {
 public:
  SIPIdentityFieldValue() = default;
  explicit SIPIdentityFieldValue(const std::string& value) : value(std::make_shared<SIPIdentity>(value)) {}
  explicit SIPIdentityFieldValue(std::shared_ptr<SIPIdentity> val) : value(val) {}

  bool parse(const std::string& val) override;
  std::string to_string() const override;

  std::shared_ptr<SIPIdentity> value;
};

// Register this field type
struct SIPIdentityFieldValueRegister {
  SIPIdentityFieldValueRegister() {
    auto reg = []() { return std::make_shared<SIPIdentityFieldValue>(); };
    FieldValue::register_factory("To", reg);
    FieldValue::register_factory("From", reg);
    FieldValue::register_factory("Contact", reg);
  }
};
static SIPIdentityFieldValueRegister s_uintFieldValueRegister;

}  // namespace athenasip::sipfields