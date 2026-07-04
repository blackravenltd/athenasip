//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include "header.h"

namespace athenasip::headers {

// Default Implementation
class StringHeader : public Header {
 public:
  StringHeader() = default;
  explicit StringHeader(const std::string& value) : _value(value) {}

  bool parse(const std::string& value) override {
    _value = value;
    return true;
  }

  std::string to_string() const override { return _value; }

 private:
  std::string _value;
};

}  // namespace athenasip::headers