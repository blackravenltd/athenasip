//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>
#include <string>
#include <unordered_set>

#include "field_value.h"

namespace athenasip::sipfields {

class SupportedFieldValue : public FieldValue {
 public:
  SupportedFieldValue() = default;
  explicit SupportedFieldValue(const std::string& value) { parse(value); }

  bool parse(const std::string& val) override;
  std::string to_string() const override;

  // Public member to store each supported feature.
  std::unordered_set<std::string> features;
};

// Register this field type under the "Supported" header name.
struct SupportedFieldValueRegister {
  SupportedFieldValueRegister() {
    auto reg = []() -> std::shared_ptr<FieldValue> { return std::make_shared<SupportedFieldValue>(); };
    FieldValue::register_factory("Supported", reg);
  }
};
static SupportedFieldValueRegister s_supportedFieldValueRegister;

}  // namespace athenasip::sipfields
