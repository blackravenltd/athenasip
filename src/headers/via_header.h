//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <unordered_map>

#include "../util.h"
#include "header.h"

namespace athenasip::headers {

class ViaHeader : public Header {
 public:
  std::string version;
  std::string host;
  std::unordered_map<std::string, std::string> parameters;

  ViaHeader() = default;
  explicit ViaHeader(const std::string& value) { parse(value); }

  bool parse(const std::string& value);

  std::string to_string() const;
};

// Register this field type
struct ViaHeaderRegister {
  ViaHeaderRegister() {
    auto reg = []() { return std::make_shared<ViaHeader>(); };
    Header::register_factory("Via", reg);
  }
};
static ViaHeaderRegister s_viaHeaderRegister;

} // namespace athenasip::headers
