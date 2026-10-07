//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include "header.h"

namespace athenasip::headers {

// The default: the field value kept as text.
class StringHeader : public Header {
 public:
  StringHeader() = default;
  explicit StringHeader(const std::string& _value) : value(_value) {}

  bool parse(const std::string& _value) override {
    value = _value;
    return true;
  }

  std::string to_string() const override { return value; }

  std::string value;

 private:
};

}  // namespace athenasip::headers