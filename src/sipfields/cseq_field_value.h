//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>

#include "field_value.h"

namespace athenasip::sipfields {

class CSeqFieldValue : public FieldValue {
 public:
  CSeqFieldValue() = default;
  explicit CSeqFieldValue(const std::string& value) { parse(value); }
  explicit CSeqFieldValue(const uint64_t seq, const std::string& val) : sequence(seq), method(val) {}

  bool parse(const std::string& val) override;
  std::string to_string() const override;

  // Public members to store the CSeq components.
  uint64_t sequence = 0;
  std::string method;
};

// Register this field type under the "CSeq" header name.
struct CSeqFieldValueRegister {
  CSeqFieldValueRegister() {
    auto reg = []() -> std::shared_ptr<FieldValue> { return std::make_shared<CSeqFieldValue>(); };
    FieldValue::register_factory("CSeq", reg);
  }
};
static CSeqFieldValueRegister s_cseqFieldValueRegister;

}  // namespace athenasip::sipfields
