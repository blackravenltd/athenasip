//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>

#include "../util.h"
#include "sip_uri.h"

namespace athenasip::types {

class SIPIdentity {
 public:
  // Constructors
  SIPIdentity();
  explicit SIPIdentity(const std::string& identity);

  // Data members
  bool wrapped;
  std::optional<std::string> display_name;
  std::shared_ptr<SIPUri> uri;
  std::unordered_map<std::string, std::string> tags;  // Stores parameters (e.g. tag, etc.)

  // Member functions
  void parse(const std::string& identity);
  std::string to_string() const;

  // Friend concatenation operators
  friend std::string operator+(const SIPIdentity& identity, const std::string& str);
  friend std::string operator+(const std::string& str, const SIPIdentity& identity);
};

}  // namespace athenasip::types
