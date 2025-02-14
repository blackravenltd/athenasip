//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include "field_value.h"

using namespace athenasip;

namespace athenasip::sipfields {

class UIntFieldValue : public FieldValue {
 public:
  UIntFieldValue() = default;
  explicit UIntFieldValue(uint64_t val) : value(val) {}

  bool parse(const std::string& val) override;
  std::string to_string() const override;

  uint64_t value = 0;
};

// Register this field type
struct UIntFieldValueRegister {
  UIntFieldValueRegister() {
    auto reg = []() { return std::make_shared<UIntFieldValue>(); };
    FieldValue::register_factory("Content-Length", reg);
  }
};
static UIntFieldValueRegister s_uintFieldValueRegister;

}  // namespace athenasip::sipfields