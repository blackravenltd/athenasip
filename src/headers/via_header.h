//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
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
  // Public members representing parts of the SIP Via header.
  // For example, given:
  //   "SIP/2.0/TLS client.example.com;branch=z9hG4bK776asdhds"
  // version  -> "SIP/2.0/TLS"
  // host     -> "client.example.com"
  // parameters -> { {"branch", "z9hG4bK776asdhds"} }
  std::string version;
  std::string host;
  std::unordered_map<std::string, std::string> parameters;

  ViaHeader() = default;
  explicit ViaHeader(const std::string& value) { parse(value); }

  // Parses a Via header string into version, host, and parameters.
  // Returns true if parsing is successful.
  bool parse(const std::string& value);

  // Returns the Via header as a string.
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

}  // namespace athenasip::headers
