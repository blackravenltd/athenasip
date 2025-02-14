//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include "field_value.h"

namespace athenasip::sipfields {

// Default Implementation
class StringFieldValue : public FieldValue {
 public:
  StringFieldValue() = default;
  explicit StringFieldValue(const std::string& value) : _value(value) {}

  bool parse(const std::string& value) override {
    _value = value;
    return true;
  }

  std::string to_string() const override { return _value; }

 private:
  std::string _value;
};

}  // namespace athenasip::sipfields