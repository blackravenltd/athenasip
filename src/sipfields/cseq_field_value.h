//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include "field_value.h"
#include <cstdint>
#include <string>
#include <memory>
#include <sstream>

namespace athenasip::sipfields {

class CSeqFieldValue : public FieldValue {
 public:
  CSeqFieldValue() = default;
  // Optionally, a constructor that parses the input string.
  explicit CSeqFieldValue(const std::string& value) { parse(value); }

  bool parse(const std::string& val) override;
  std::string to_string() const override;

  // Public members to store the CSeq components.
  uint64_t sequence = 0;
  std::string method;
};

// Register this field type under the "CSeq" header name.
struct CSeqFieldValueRegister {
  CSeqFieldValueRegister() {
    auto reg = []() -> std::shared_ptr<FieldValue> {
      return std::make_shared<CSeqFieldValue>();
    };
    FieldValue::register_factory("CSeq", reg);
  }
};
static CSeqFieldValueRegister s_cseqFieldValueRegister;

}  // namespace athenasip::sipfields
