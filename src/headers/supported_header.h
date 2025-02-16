//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>
#include <string>
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <sstream>

#include "header.h"

namespace athenasip::headers {

class SupportedHeader : public Header {
 public:
  SupportedHeader() = default;
  explicit SupportedHeader(const std::string& value) { parse(value); }

  bool parse(const std::string& val) override;
  std::string to_string() const override;

  // Public member to store each supported feature.
  std::unordered_set<std::string> features;
};

// Register this field type under the "Supported" header name.
struct SupportedHeaderRegister {
  SupportedHeaderRegister() {
    auto reg = []() -> std::shared_ptr<Header> { return std::make_shared<SupportedHeader>(); };
    Header::register_factory("Supported", reg);
  }
};
static SupportedHeaderRegister s_supportedHeaderRegister;

}  // namespace athenasip::headers
